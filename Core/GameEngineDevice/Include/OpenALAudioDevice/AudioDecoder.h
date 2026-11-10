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

//////// AudioDecoder.h /////////////////////////
// GeneralsX:
/////////////////////////////////////////////////
//
// One decoder interface for the OpenAL backend, with two implementations selected at compile
// time by RTS_BUILD_OPTION_FFMPEG:
//
//   * FFmpeg  - macOS, Linux, iOS. Wraps the existing FFmpegFile verbatim.
//   * dr_wav / dr_mp3 - Windows x64, where FFmpeg is not available and is not needed.
//
// WHY THIS EXISTS. generalszh.exe is x64 and retail mss32.dll is x86, so Windows has no audio
// at all; the fix is the OpenAL backend the other three platforms already run. That backend
// hard-included libavcodec and held FFmpegFile* as struct members, which made it unbuildable
// on a box with no FFmpeg. Rather than #ifdef the two call sites, they now name this class and
// nothing else.
//
// WHY THE CONTRACT IS "INTERLEAVED". FFmpeg hands back planar audio for some codecs, and both
// call sites carried their own copy of the planar->interleaved loop. That conversion now lives
// inside the FFmpeg implementation, so the two call sites are identical on every platform.
// dr_wav and dr_mp3 emit interleaved s16 for PCM, MS-ADPCM and IMA-ADPCM alike and never need
// it. Measured over the 25 shipped BIGs: 3530 audio entries, 2572 IMA-ADPCM, 951 PCM, 7 MP3,
// zero unhandled - so those two decoders are complete coverage, which is what keeps FFmpeg
// (and therefore vcpkg.json, which IS digested) out of the Windows build entirely.
//
// NOTHING FFMPEG- OR DR-SPECIFIC MAY APPEAR IN THIS HEADER. It is included by
// OpenALAudioCache.h, which is compiled on Windows where the FFmpeg headers do not exist. The
// implementation types are hidden behind a pimpl for exactly that reason.

#pragma once

#include <Lib/BaseType.h>

#include <functional>

class File;

//-------------------------------------------------------------------------------------------------
/** Called once per decoded chunk with interleaved PCM.
	*
	* Deliberately describes the samples rather than naming an ALenum: this class decodes, it does
	* not know about the audio backend. Callers turn the shape into a format themselves - which is
	* also what keeps it out of OpenALAudioManager::getALFormat, a protected member reachable only
	* through friendship.
	*
	* @param data           interleaved samples, valid only for the duration of the call
	* @param numBytes       size of \a data
	* @param numSamples     samples per channel in this chunk
	* @param numChannels    channels in \a data
	* @param bitsPerSample  bit depth of \a data
	* @param sampleRate     Hz
	* @param userData       whatever was handed to setUserData
	*/
typedef std::function<void(const UnsignedByte* data,
													 Int numBytes,
													 Int numSamples,
													 Int numChannels,
													 Int bitsPerSample,
													 Int sampleRate,
													 void* userData)> AudioDecodeCallback;

//-------------------------------------------------------------------------------------------------
class AudioDecoder
{
public:
	AudioDecoder();
	~AudioDecoder();

	/// Takes ownership of \a file. Returns FALSE if the container has no decodable audio stream.
	Bool open(File* file);
	void close();

	void setFrameCallback(AudioDecodeCallback callback) { m_frameCallback = callback; }
	void setUserData(void* userData) { m_userData = userData; }

	/// Decode the next chunk and invoke the frame callback. Returns FALSE at end of data or on a
	/// decode error; isAtEof() distinguishes the two.
	Bool decodeChunk();

	Bool isAtEof() const { return m_atEof; }
	Bool hasAudio() const;

	Int getNumChannels() const;
	Int getSampleRate() const;
	Int getBytesPerSample() const;
	Int getSizeForSamples(Int numSamples) const;

private:
	// Non-copyable: owns a File and a decoder handle.
	AudioDecoder(const AudioDecoder&);
	AudioDecoder& operator=(const AudioDecoder&);

	struct Impl;
	Impl* m_impl;

	AudioDecodeCallback m_frameCallback;
	void* m_userData;
	Bool m_atEof;
};
