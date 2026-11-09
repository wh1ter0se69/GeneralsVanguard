/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// FILE: OpenALAudioStream.h //////////////////////////////////////////////////////////////////////////
// OpenALAudioStream implementation
// Author: Stephan Vedder, March 2025
#pragma once

#include "always.h"
#include <AL/al.h>
#include <stdint.h>
#include <functional>

#define AL_STREAM_BUFFER_COUNT 32

// ---------------------------------------------------------------------------
// Audio diagnostics
//
// Intermittent audio faults (a sudden blast of noise, the stalled-speech chirp)
// are hard to catch because by the time a human reacts the state that caused it
// is gone. So streams continuously record their interesting transitions into a
// small ring buffer, costing nothing until something asks for it, and the player
// triggers a dump the moment they hear the fault. The dump prints the recent
// history plus every live stream's current state, which is what actually
// distinguishes "restarted with stale buffers still queued" from "decoder handed
// us garbage".
//
// Trigger: F9 on desktop, a four-finger tap on iOS.
// ---------------------------------------------------------------------------
void AudioDebugRecord(const char *fmt, ...);
void AudioDebugDump(const char *reason);

class OpenALAudioStream final
{
public:
    OpenALAudioStream();
    ~OpenALAudioStream();

    // GeneralsX @bugfix 14/06/2026 The data callback now returns FALSE when the source
    // stream has reached true end-of-file (no more data will ever come), so update()
    // can let a finished one-shot speech reach a stable AL_STOPPED instead of endlessly
    // restarting its drained source. Returns TRUE on a normal refill or transient error.
    void setRequireDataCallback(std::function<bool()> callback) { m_requireDataCallback = callback; }
    ALuint getSource() const { return m_source; }
    // GeneralsX @bugfix 14/06/2026 True once the source stream signalled real EOF.
    bool isAtEnd() const { return m_endOfData; }

    bool bufferData(const uint8_t *data, size_t data_size, ALenum format, int samplerate);
    bool isPlaying();
    void update();
    void reset();

    void play() { alSourcePlay(m_source); }
    void pause() { alSourcePause(m_source); }
    void stop() { alSourceStop(m_source); }

    void setVolume(float vol) { alSourcef(m_source, AL_GAIN, vol); }

protected:
    std::function<bool()> m_requireDataCallback = nullptr;
    bool m_endOfData = false; ///< GeneralsX: source stream signalled true EOF; stop restarting it
    int m_stalledProbes = 0;  ///< GeneralsX: consecutive EOF-probes that produced no new data
    ALuint m_source = 0;
    ALuint m_buffers[AL_STREAM_BUFFER_COUNT] = {};
    unsigned int m_current_buffer_idx = 0;
};