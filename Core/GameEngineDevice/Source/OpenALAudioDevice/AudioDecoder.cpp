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

// Two implementations of the AudioDecoder facade, chosen by a definition CMake sets from RTS_BUILD_OPTION_FFMPEG;
// exactly one is compiled in. SAGE_AUDIO_DECODER_DR (all shipping builds): dr_wav / dr_mp3 over the VFS.
// SAGE_AUDIO_DECODER_FFMPEG is set by no build. NEVER use it for a build that joins a lobby: the decoder choice is a
// simulation input (getFileLengthMS) not covered by the sourceID digest; the 7 MP3s differ by 3 MPEG granules.

#include "OpenALAudioDevice/AudioDecoder.h"

#include "Common/file.h"
#include "Common/Debug.h"

#include <cstring>
#include <vector>

#if defined(SAGE_AUDIO_DECODER_FFMPEG) && defined(SAGE_AUDIO_DECODER_DR)
#error "Exactly one audio decoder implementation may be compiled per binary."
#endif
#if !defined(SAGE_AUDIO_DECODER_FFMPEG) && !defined(SAGE_AUDIO_DECODER_DR)
#error "No audio decoder implementation selected - see Core/GameEngineDevice/CMakeLists.txt."
#endif

#if defined(SAGE_AUDIO_DECODER_FFMPEG)

#include "VideoDevice/FFmpeg/FFmpegFile.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

//-------------------------------------------------------------------------------------------------
struct AudioDecoder::Impl
{
	FFmpegFile* m_file;
	// Scratch for planar->interleaved. Kept across chunks so a streaming track does not
	// reallocate once per decoded frame.
	std::vector<UnsignedByte> m_interleaved;

	Impl() : m_file(NULL) {}
};

//-------------------------------------------------------------------------------------------------
AudioDecoder::AudioDecoder() : m_impl(NULL), m_userData(NULL), m_atEof(FALSE)
{
}

//-------------------------------------------------------------------------------------------------
AudioDecoder::~AudioDecoder()
{
	close();
}

//-------------------------------------------------------------------------------------------------
Bool AudioDecoder::open(File* file)
{
	if (file == NULL) {
		return FALSE;
	}

	close();

	m_impl = new Impl();
	m_atEof = FALSE;

	// FFmpegFile takes ownership of the File, including on its own failure path.
	m_impl->m_file = new FFmpegFile();
	if (!m_impl->m_file->open(file)) {
		close();
		return FALSE;
	}

	m_impl->m_file->setFrameCallback([this](AVFrame* frame, int /*streamIdx*/, int streamType, void* /*userData*/) {
		if (streamType != AVMEDIA_TYPE_AUDIO || !m_frameCallback) {
			return;
		}

		const AVSampleFormat sampleFmt = static_cast<AVSampleFormat>(frame->format);
		const Int channels = frame->ch_layout.nb_channels;
		const Int bytesPerSample = av_get_bytes_per_sample(sampleFmt);
		const Int numBytes = av_samples_get_buffer_size(NULL, channels, frame->nb_samples, sampleFmt, 1);

		const UnsignedByte* data;
		if (av_sample_fmt_is_planar(sampleFmt)) {
			m_impl->m_interleaved.resize(numBytes);
			for (int sampleIdx = 0; sampleIdx < frame->nb_samples; ++sampleIdx) {
				const int byteOffset = sampleIdx * bytesPerSample;
				for (int channelIdx = 0; channelIdx < channels; ++channelIdx) {
					memcpy(&m_impl->m_interleaved[byteOffset * channels + channelIdx * bytesPerSample],
						&frame->data[channelIdx][byteOffset],
						bytesPerSample);
				}
			}
			data = m_impl->m_interleaved.data();
		} else {
			data = frame->data[0];
		}

		m_frameCallback(data, numBytes, frame->nb_samples, channels, bytesPerSample * 8,
			frame->sample_rate, m_userData);
		});

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void AudioDecoder::close()
{
	if (m_impl != NULL) {
		if (m_impl->m_file != NULL) {
			m_impl->m_file->close();
			delete m_impl->m_file;
		}
		delete m_impl;
		m_impl = NULL;
	}
	m_atEof = FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool AudioDecoder::decodeChunk()
{
	if (m_impl == NULL || m_impl->m_file == NULL) {
		return FALSE;
	}

	const Bool decoded = m_impl->m_file->decodePacket();
	m_atEof = m_impl->m_file->isAtEof();
	return decoded;
}

//-------------------------------------------------------------------------------------------------
Bool AudioDecoder::hasAudio() const
{
	return (m_impl != NULL && m_impl->m_file != NULL) ? m_impl->m_file->hasAudio() : FALSE;
}

Int AudioDecoder::getNumChannels() const
{
	return (m_impl != NULL && m_impl->m_file != NULL) ? m_impl->m_file->getNumChannels() : 0;
}

Int AudioDecoder::getSampleRate() const
{
	return (m_impl != NULL && m_impl->m_file != NULL) ? m_impl->m_file->getSampleRate() : 0;
}

Int AudioDecoder::getBytesPerSample() const
{
	return (m_impl != NULL && m_impl->m_file != NULL) ? m_impl->m_file->getBytesPerSample() : 0;
}

Int AudioDecoder::getSizeForSamples(Int numSamples) const
{
	return (m_impl != NULL && m_impl->m_file != NULL) ? m_impl->m_file->getSizeForSamples(numSamples) : 0;
}

#else // SAGE_AUDIO_DECODER_DR

// dr_wav and dr_mp3 are single-header libraries; this is the one translation unit that carries
// their implementations. NO_STDIO drops the fopen-based backends we cannot use anyway - every
// asset arrives as a File* from the game VFS, not a path on disk.
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DRWAV_NO_STDIO
#define DRMP3_NO_STDIO
#include "OpenALAudioDevice/dr_wav.h"
#include "OpenALAudioDevice/dr_mp3.h"

namespace
{
	// Samples per channel handed to OpenAL per decodeChunk(). OpenALAudioStream keeps 32 buffers
	// and refills whenever fewer than 16 are queued, so this is the granularity of a refill:
	// 4096 frames is ~186 ms at 22 kHz, giving a comfortable ~3 s of queued lookahead without
	// making any single decode call expensive.
	const Int DECODE_CHUNK_FRAMES = 4096;

	//-----------------------------------------------------------------------------------------------
	// dr_* IO shims over the engine's File. BIG entries arrive as RAMFile, whose seek is real and
	// entry-relative (RAMFile.cpp:353) - worth stating because FFmpeg never exercised it: it
	// passes a null seek proc to avio_alloc_context and reads the container strictly forward.
	//-----------------------------------------------------------------------------------------------
	size_t drFileRead(void* userData, void* bufferOut, size_t bytesToRead)
	{
		File* file = static_cast<File*>(userData);
		const Int bytesRead = file->read(bufferOut, static_cast<Int>(bytesToRead));
		return (bytesRead > 0) ? static_cast<size_t>(bytesRead) : 0;
	}

	// RAMFile::seek clamps to [0, size] and reports the clamped position rather than failing,
	// which matches what fseek does in dr_*'s own stdio backend - a seek past the end succeeds
	// and the next read returns 0, which is how the decoders detect EOF.
	drwav_bool32 drWavSeek(void* userData, int offset, drwav_seek_origin origin)
	{
		File* file = static_cast<File*>(userData);
		const File::seekMode mode = (origin == drwav_seek_origin_start) ? File::START : File::CURRENT;
		return (file->seek(offset, mode) >= 0) ? DRWAV_TRUE : DRWAV_FALSE;
	}

	drmp3_bool32 drMp3Seek(void* userData, int offset, drmp3_seek_origin origin)
	{
		File* file = static_cast<File*>(userData);
		File::seekMode mode = File::CURRENT;
		if (origin == drmp3_seek_origin_start) {
			mode = File::START;
		} else if (origin == drmp3_seek_origin_end) {
			mode = File::END;
		}
		return (file->seek(offset, mode) >= 0) ? DRMP3_TRUE : DRMP3_FALSE;
	}

	drmp3_bool32 drMp3Tell(void* userData, drmp3_int64* pCursor)
	{
		File* file = static_cast<File*>(userData);
		const Int pos = file->position();
		if (pos < 0) {
			return DRMP3_FALSE;
		}
		*pCursor = static_cast<drmp3_int64>(pos);
		return DRMP3_TRUE;
	}
}

//-------------------------------------------------------------------------------------------------
struct AudioDecoder::Impl
{
	enum Kind { KIND_NONE, KIND_WAV, KIND_MP3 };

	File* m_file;
	Kind m_kind;
	drwav m_wav;
	drmp3 m_mp3;
	Int m_channels;
	Int m_sampleRate;
	std::vector<drwav_int16> m_pcm;

	Impl() : m_file(NULL), m_kind(KIND_NONE), m_channels(0), m_sampleRate(0)
	{
		memset(&m_wav, 0, sizeof(m_wav));
		memset(&m_mp3, 0, sizeof(m_mp3));
	}

	// Identify by content, not by filename extension. The reference win64 port dispatches on the
	// extension, which silently drops anything named unexpectedly; the shipped BIGs hold 2572
	// IMA-ADPCM, 951 PCM and 7 MP3 entries, and a missed one is a silently absent sound rather
	// than a crash.
	static Kind sniff(File* file)
	{
		UnsignedByte head[12];
		const Int got = file->read(head, sizeof(head));
		file->seek(0, File::START);
		if (got < static_cast<Int>(sizeof(head))) {
			return KIND_NONE;
		}

		if (memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
			return KIND_WAV;
		}
		if (memcmp(head, "ID3", 3) == 0) {
			return KIND_MP3;
		}
		// Bare MPEG audio frame sync: eleven set bits.
		if (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) {
			return KIND_MP3;
		}
		return KIND_NONE;
	}
};

//-------------------------------------------------------------------------------------------------
AudioDecoder::AudioDecoder() : m_impl(NULL), m_userData(NULL), m_atEof(FALSE)
{
}

//-------------------------------------------------------------------------------------------------
AudioDecoder::~AudioDecoder()
{
	close();
}

//-------------------------------------------------------------------------------------------------
Bool AudioDecoder::open(File* file)
{
	if (file == NULL) {
		return FALSE;
	}

	close();

	m_impl = new Impl();
	m_impl->m_file = file;			// ownership transfers here, including on the failure paths below
	m_atEof = FALSE;

	switch (Impl::sniff(file))
	{
		case Impl::KIND_WAV:
			if (!drwav_init(&m_impl->m_wav, drFileRead, drWavSeek, file, NULL)) {
				DEBUG_LOG(("AudioDecoder: dr_wav refused a RIFF/WAVE container\n"));
				close();
				return FALSE;
			}
			m_impl->m_kind = Impl::KIND_WAV;
			m_impl->m_channels = static_cast<Int>(m_impl->m_wav.channels);
			m_impl->m_sampleRate = static_cast<Int>(m_impl->m_wav.sampleRate);
			break;

		case Impl::KIND_MP3:
			if (!drmp3_init(&m_impl->m_mp3, drFileRead, drMp3Seek, drMp3Tell, file, NULL)) {
				DEBUG_LOG(("AudioDecoder: dr_mp3 refused an MPEG container\n"));
				close();
				return FALSE;
			}
			m_impl->m_kind = Impl::KIND_MP3;
			m_impl->m_channels = static_cast<Int>(m_impl->m_mp3.channels);
			m_impl->m_sampleRate = static_cast<Int>(m_impl->m_mp3.sampleRate);
			break;

		default:
			DEBUG_LOG(("AudioDecoder: unrecognised audio container\n"));
			close();
			return FALSE;
	}

	if (m_impl->m_channels <= 0 || m_impl->m_sampleRate <= 0) {
		DEBUG_LOG(("AudioDecoder: container reports %d channels at %d Hz\n",
			m_impl->m_channels, m_impl->m_sampleRate));
		close();
		return FALSE;
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void AudioDecoder::close()
{
	if (m_impl != NULL) {
		if (m_impl->m_kind == Impl::KIND_WAV) {
			drwav_uninit(&m_impl->m_wav);
		} else if (m_impl->m_kind == Impl::KIND_MP3) {
			drmp3_uninit(&m_impl->m_mp3);
		}
		if (m_impl->m_file != NULL) {
			m_impl->m_file->close();
			m_impl->m_file = NULL;
		}
		delete m_impl;
		m_impl = NULL;
	}
	m_atEof = FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool AudioDecoder::decodeChunk()
{
	if (m_impl == NULL || m_impl->m_kind == Impl::KIND_NONE || m_atEof) {
		return FALSE;
	}

	const Int channels = m_impl->m_channels;
	m_impl->m_pcm.resize(static_cast<size_t>(DECODE_CHUNK_FRAMES) * channels);

	drwav_uint64 framesRead = 0;
	if (m_impl->m_kind == Impl::KIND_WAV) {
		framesRead = drwav_read_pcm_frames_s16(&m_impl->m_wav, DECODE_CHUNK_FRAMES, m_impl->m_pcm.data());
	} else {
		framesRead = drmp3_read_pcm_frames_s16(&m_impl->m_mp3, DECODE_CHUNK_FRAMES, m_impl->m_pcm.data());
	}

	if (framesRead == 0) {
		m_atEof = TRUE;
		return FALSE;
	}

	// A short read means the source is exhausted. Deliver this chunk, then report EOF, so the
	// caller's `while (decodeChunk())` terminates and the streaming path's require-data callback
	// stops restarting a finished one-shot.
	if (framesRead < static_cast<drwav_uint64>(DECODE_CHUNK_FRAMES)) {
		m_atEof = TRUE;
	}

	if (m_frameCallback) {
		const Int numSamples = static_cast<Int>(framesRead);
		const Int numBytes = numSamples * channels * static_cast<Int>(sizeof(drwav_int16));
		// dr_wav and dr_mp3 always hand back interleaved signed 16-bit, whatever went in.
		m_frameCallback(reinterpret_cast<const UnsignedByte*>(m_impl->m_pcm.data()),
			numBytes, numSamples, channels, 16, m_impl->m_sampleRate, m_userData);
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool AudioDecoder::hasAudio() const
{
	return (m_impl != NULL && m_impl->m_kind != Impl::KIND_NONE) ? TRUE : FALSE;
}

Int AudioDecoder::getNumChannels() const
{
	return (m_impl != NULL) ? m_impl->m_channels : 0;
}

Int AudioDecoder::getSampleRate() const
{
	return (m_impl != NULL) ? m_impl->m_sampleRate : 0;
}

Int AudioDecoder::getBytesPerSample() const
{
	// Always signed 16-bit out, whatever went in.
	return (m_impl != NULL && m_impl->m_kind != Impl::KIND_NONE) ? static_cast<Int>(sizeof(drwav_int16)) : 0;
}

Int AudioDecoder::getSizeForSamples(Int numSamples) const
{
	if (m_impl == NULL || m_impl->m_kind == Impl::KIND_NONE) {
		return 0;
	}
	return numSamples * m_impl->m_channels * static_cast<Int>(sizeof(drwav_int16));
}

#endif // SAGE_AUDIO_DECODER_FFMPEG
