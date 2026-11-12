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
// One decoder interface for the OpenAL backend, chosen at compile time by RTS_BUILD_OPTION_FFMPEG:
// FFmpeg (macOS, Linux, iOS) or dr_wav / dr_mp3 (Windows x64, no FFmpeg). Output is interleaved PCM.
// Nothing FFmpeg- or dr-specific may appear here: OpenALAudioCache.h includes it on Windows.

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
