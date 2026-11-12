# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# openal-soft 1.24.2 StartSources: never dereference a null voice.
#
# Run by cmake/openal.cmake as the openal_soft PATCH_COMMAND: cmake -DSRC=<source dir> -P <this file>.
#
# StartSources (al/source.cpp) counts the reusable voices, allocates more if the count falls short,
# then searches for a free voice per source and ASSUMEs the search succeeds. In the game's mass
# kill-and-replace bursts (thousands of alSourcePlay per frame, each replacing a playing source) the
# search has run off the end of every active voice (393 of them, the iterator equal to end, the voice
# register 0) and the next store faulted at 0x17c, on macOS and on Windows. The interleaving that
# breaks the count is not identified; this makes the failure impossible instead: activate one more
# voice, as SetVoiceOffset already does when it finds none, and if even that one is busy, stop the
# source cleanly (the engine then releases the sound) rather than crash.
#
# Exact-anchor edit, no external patch tool (Windows builders have none): fails the configure if the
# anchor is not found exactly once, and does nothing when the guard is already in place.

if(NOT DEFINED SRC)
    message(FATAL_ERROR "openal-startsources-guard: pass -DSRC=<openal-soft source dir>")
endif()
set(_file "${SRC}/al/source.cpp")
file(READ "${_file}" _text)

string(FIND "${_text}" "GeneralsX: StartSources voice guard" _done)
if(NOT _done EQUAL -1)
    return()
endif()

set(_anchor "        ASSUME(voice != nullptr);

        voice->mPosition.store(0, std::memory_order_relaxed);")

string(LENGTH "${_text}" _len)
string(REPLACE "${_anchor}" "" _without "${_text}")
string(LENGTH "${_without}" _len_without)
string(LENGTH "${_anchor}" _len_anchor)
math(EXPR _hits "(${_len} - ${_len_without}) / ${_len_anchor}")
if(NOT _hits EQUAL 1)
    message(FATAL_ERROR "openal-startsources-guard: anchor found ${_hits} times in ${_file}, expected 1")
endif()

set(_guard "        if(!voice) UNLIKELY
        {
            /* GeneralsX: StartSources voice guard. The reusable-voice count above did not hold
             * for this search; activate one more voice rather than dereference null. */
            auto &allvoices = *context->mVoices.load(std::memory_order_relaxed);
            if(allvoices.size() == voicelist.size())
                context->allocVoices(1);
            context->mActiveVoiceCount.fetch_add(1, std::memory_order_release);
            voicelist = context->getVoicesSpan();
            voiceiter = voicelist.begin() + vidx;
            Voice *v{*voiceiter};
            if(v->mPlayState.load(std::memory_order_acquire) == Voice::Stopped
                && v->mSourceID.load(std::memory_order_relaxed) == 0u
                && v->mPendingChange.load(std::memory_order_relaxed) == false)
                voice = v;
            else
            {
                /* Still none: have the mixer stop any old voice (clearing its pending flag)
                 * and leave the source stopped. */
                cur->mVoice = cur->mOldVoice;
                cur->mOldVoice = nullptr;
                cur->mSourceID = source->id;
                cur->mState = VChangeState::Stop;
                source->Offset = 0.0;
                source->OffsetType = AL_NONE;
                source->state = AL_STOPPED;
                source->VoiceIdx = InvalidVoiceIndex;
                continue;
            }
        }
${_anchor}")

string(REPLACE "${_anchor}" "${_guard}" _text "${_text}")
file(WRITE "${_file}" "${_text}")
message(STATUS "openal-startsources-guard: applied to ${_file}")
