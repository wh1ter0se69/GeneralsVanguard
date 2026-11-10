#include "OpenALAudioCache.h"

#include "Common/AudioEventInfo.h"
#include "Common/AudioEventRTS.h"
#include "Common/file.h"
#include "Common/FileSystem.h"

#include <cstring>
#include <limits>

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
OpenALAudioFileCache::OpenALAudioFileCache() : m_maxSize(14*1024*1024), m_currentlyUsedSize(0)
{
}

// GeneralsX: Was decodeFFmpeg, and hard-coded to AVFrame. AudioDecoder
// hands back interleaved PCM on every platform - the planar conversion this function used to carry
// now lives inside the FFmpeg implementation of the facade, so macOS, Linux and iOS still run
// exactly the same conversion and produce exactly the same bytes.
Bool OpenALAudioFileCache::decodeAudio(OpenAudioFile* file)
{
	std::vector<UnsignedByte> audioData;

	file->m_decoder->setUserData(file);
	file->m_decoder->setFrameCallback([&audioData](const UnsignedByte* data,
		Int numBytes, Int numSamples, Int /*numChannels*/, Int /*bitsPerSample*/,
		Int /*sampleRate*/, void* userData) {
			OpenAudioFile* file = static_cast<OpenAudioFile*>(userData);
			audioData.reserve(audioData.size() + numBytes);
			audioData.insert(audioData.end(), data, data + numBytes);
			file->m_fileSize += numBytes;
			file->m_totalSamples += numSamples;
		});

	// Read every chunk in the file.
	while (file->m_decoder->decodeChunk()) {
	}

	// Format and rate come from the container rather than from the last chunk, which is what this
	// did before and keeps a truncated final chunk from mislabelling the buffer.
	const Int sampleRate = file->m_decoder->getSampleRate();

	// Fill the buffer with the audio data
	alBufferData(file->m_buffer, OpenALAudioManager::getALFormat(file->m_decoder->getNumChannels(), file->m_decoder->getBytesPerSample() * 8),
		audioData.data(), audioData.size(), sampleRate);

	// Calculate the duration in MS
	file->m_duration = sampleRate > 0
		? (file->m_totalSamples / (float)sampleRate) * 1000.0f
		: 0.0f;

	return true;
}

//-------------------------------------------------------------------------------------------------
OpenALAudioFileCache::~OpenALAudioFileCache()
{
	// Free all the samples that are open.
	OpenFilesHashIt it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_openCount > 0) {
			DEBUG_CRASH(("Sample '%s' is still playing, and we're trying to quit.\n", it->second.m_eventInfo->m_audioName.str()));
		}

		releaseOpenAudioFile(&it->second);
		// Don't erase it from the map, cause it makes this whole process way more complicated, and 
		// we're about to go away anyways.
	}
}

//-------------------------------------------------------------------------------------------------
ALuint OpenALAudioFileCache::getBufferForFile(const OpenFileInfo &fileInfo)
{
	AudioEventRTS *eventToOpenFrom = fileInfo.event;

	AsciiString strToFind;
	if (eventToOpenFrom)
	{
		switch (eventToOpenFrom->getNextPlayPortion())
		{
		case PP_Attack:
			strToFind = eventToOpenFrom->getAttackFilename();
			break;
		case PP_Sound:
			strToFind = eventToOpenFrom->getFilename();
			break;
		case PP_Decay:
			strToFind = eventToOpenFrom->getDecayFilename();
			break;
		case PP_Done:
			return 0;
		}
	}
	else
	{
		if (fileInfo.filename)
		{
			strToFind = *fileInfo.filename;
		}
		else
		{
			DEBUG_CRASH(("No filename to open\n"));
			return 0;
		}
	}

	auto it = m_openFiles.find(strToFind);

	if (it != m_openFiles.end()) {
		++it->second.m_openCount;
		return it->second.m_buffer;
	}

	// Couldn't find the file, so actually open it.
	File* file = TheFileSystem->openFile(strToFind.str());
	if (!file) {
		DEBUG_ASSERTLOG(strToFind.isEmpty(), ("Missing Audio File: '%s'\n", strToFind.str()));
		return 0;
	}

	UnsignedInt fileSize = file->size();

	OpenAudioFile openedAudioFile;
	alGenBuffers(1, &openedAudioFile.m_buffer);
	openedAudioFile.m_eventInfo = eventToOpenFrom ? eventToOpenFrom->getAudioEventInfo() : NULL;
	openedAudioFile.m_decoder = new AudioDecoder();

	// This transfer ownership of file
	if (!openedAudioFile.m_decoder->open(file)) {
		releaseOpenAudioFile(&openedAudioFile);
		return 0;
	}

	// GeneralsX: A failed alGenBuffers (e.g. AL_OUT_OF_MEMORY, no current
	// context) leaves m_buffer 0, and decodeAudio does not check AL errors. Cached with a live
	// reference that handle could never be closed (closeBuffer(0) returns at once) or evicted, and
	// every retry would hit it and add another reference. Refuse it here - after the decoder owns
	// the File, so releaseOpenAudioFile frees both, and BEFORE the full decode, so a persistent AL
	// failure costs a header parse per request rather than a whole-asset decode.
	if (openedAudioFile.m_buffer == 0) {
		releaseOpenAudioFile(&openedAudioFile);
		return 0;
	}

	if (eventToOpenFrom && eventToOpenFrom->isPositionalAudio()) {
		if (openedAudioFile.m_decoder->getNumChannels() > 1) {
			// GeneralsX @bugfix Bender 09/05/2026 Keep multichannel positional assets loadable so playback can fall back to non-spatial output.
			DEBUG_LOG(("OpenAL positional fallback armed for multichannel audio '%s' (%d channels)\n",
				strToFind.str(), openedAudioFile.m_decoder->getNumChannels()));
		}
	}

	if (!decodeAudio(&openedAudioFile)) {
		releaseOpenAudioFile(&openedAudioFile);
		return 0;
	}

	openedAudioFile.m_decoder->close();

	// KNOWN DEFECT, LEFT IN DELIBERATELY - DO NOT "TIDY" THIS AWAY WITHOUT READING THIS NOTE.
	//
	// decodeAudio() has just accumulated the DECODED size into m_fileSize, one chunk at a time.
	// This line throws that away and replaces it with the ON-DISK COMPRESSED size, which is then
	// what the whole cache budget is accounted in. Every eviction decision below, and the 14 MiB
	// m_maxSize ceiling, are therefore computed against a number that is not the memory actually
	// held.
	//
	// The error is not small and it is not uniform. IMA-ADPCM decodes to roughly 4x its on-disk
	// size, and 2572 of the 3530 audio entries in the shipped BIGs are IMA-ADPCM - so on a normal
	// match the cache believes it is holding 14 MiB while actually holding something closer to
	// four times that. PCM entries (951) are accounted correctly, so the overshoot varies with
	// which sounds a match happens to touch.
	//
	// NOT FIXED HERE ON PURPOSE. Correcting it makes evictions fire far more often, which changes
	// which samples are resident and when they are re-decoded. That is a real behaviour change,
	// and this landed as part of giving Windows an audio decoder at all - a change whose whole
	// discipline was that Windows must behave exactly like the other three peers. Fixing it is a
	// separate, deliberate change that lands on all four platforms together, with the memory
	// profile measured before and after.
	openedAudioFile.m_fileSize = fileSize;
	m_currentlyUsedSize += openedAudioFile.m_fileSize;
	if (m_currentlyUsedSize > m_maxSize) {
		DEBUG_LOG(("Audio Cache is full, trying to free some space\n"));
		// We need to free some samples, or we're not going to be able to play this sound.
		if (!freeEnoughSpaceForSample(openedAudioFile)) {
			DEBUG_LOG(("Couldn't free enough space for sample\n"));
			m_currentlyUsedSize -= openedAudioFile.m_fileSize;
			releaseOpenAudioFile(&openedAudioFile);
			return 0;
		}
	}

	// GeneralsX: The caller that triggered the load holds a reference, as the
	// cache-hit path above and Miles' openFile (m_openCount = 1) both count it. Leaving it at 0 made
	// that caller's closeBuffer wrap the unsigned count to UINT_MAX: a finished sample then looked
	// "playing" forever and could never be evicted, so once 14 MiB was resident every new sample of
	// equal priority failed to load (silence) and getFileLengthMS returned 0 (script timing).
	openedAudioFile.m_openCount = 1;
	m_openFiles[strToFind] = openedAudioFile;
	return openedAudioFile.m_buffer;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioFileCache::closeBuffer(ALuint bufferToClose)
{
	if (!bufferToClose) {
		return;
	}

	OpenFilesHash::iterator it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_buffer == bufferToClose) {
			--it->second.m_openCount;
			return;
		}
	}
}

float OpenALAudioFileCache::getBufferLength(ALuint handle)
{
	if (!handle) {
		return 0.0f;
	}

	for (auto it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_buffer == handle) {
			return it->second.m_duration;
		}
	}

	return 0.0f;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioFileCache::setMaxSize(UnsignedInt size)
{
	// Protect the function, in case we're trying to use this value elsewhere.

	// Hardcoded to 14MiB for now, this is a workaround for the limit
	//  set by the default config files being 4MB and causing needless reloads.
	//m_maxSize = size;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioFileCache::releaseOpenAudioFile(OpenAudioFile* fileToRelease)
{
	if (fileToRelease->m_openCount > 0) {
		// This thing needs to be terminated IMMEDIATELY.
		TheAudio->closeAnySamplesUsingFile((const void*)(uintptr_t)fileToRelease->m_buffer);
	}

	if (fileToRelease->m_decoder) {
		// Free the decoder handles (and the File it took ownership of)
		delete fileToRelease->m_decoder;
	}

	if (fileToRelease->m_buffer)
	{
		// Free the OpenAL buffer
		alDeleteBuffers(1, &fileToRelease->m_buffer);
	}
	fileToRelease->m_decoder = NULL;
	fileToRelease->m_buffer = 0;
	fileToRelease->m_eventInfo = NULL;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioFileCache::freeEnoughSpaceForSample(const OpenAudioFile& sampleThatNeedsSpace)
{

	Int spaceRequired = m_currentlyUsedSize - m_maxSize;
	Int runningTotal = 0;
	// GeneralsX @bugfix fbraz3 16/04/2026 Handle cache entries without AudioEventInfo (filename-only loads).
	const Int requestedPriority = sampleThatNeedsSpace.m_eventInfo
		? sampleThatNeedsSpace.m_eventInfo->m_priority
		: std::numeric_limits<Int>::min();

	std::list<AsciiString> filesToClose;
	// First, search for any samples that have ref counts of 0. They are low-hanging fruit, and 
	// should be considered immediately.
	OpenFilesHashIt it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_openCount == 0) {
			// This is said low-hanging fruit.
			filesToClose.push_back(it->first);

			runningTotal += it->second.m_fileSize;

			if (runningTotal >= spaceRequired) {
				break;
			}
		}
	}

	// If we don't have enough space yet, then search through the events who have a count of 1 or more
	// and who are lower priority than this sound.
	// Mical said that at this point, sounds shouldn't care if other sounds are interruptable or not.
	// Kill any files of lower priority necessary to clear our the buffer.
	if (runningTotal < spaceRequired) {
		for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
			if (it->second.m_openCount > 0) {
				const Int candidatePriority = it->second.m_eventInfo
					? it->second.m_eventInfo->m_priority
					: std::numeric_limits<Int>::min();
				if (candidatePriority < requestedPriority) {
					filesToClose.push_back(it->first);
					runningTotal += it->second.m_fileSize;

					if (runningTotal >= spaceRequired) {
						break;
					}
				}
			}
		}
	}

	// We weren't able to find enough sounds to truncate. Therefore, this sound is not going to play.
	if (runningTotal < spaceRequired) {
		return FALSE;
	}

	std::list<AsciiString>::iterator ait;
	for (ait = filesToClose.begin(); ait != filesToClose.end(); ++ait) {
		OpenFilesHashIt itToErase = m_openFiles.find(*ait);
		if (itToErase != m_openFiles.end()) {
			releaseOpenAudioFile(&itToErase->second);
			m_currentlyUsedSize -= itToErase->second.m_fileSize;
			m_openFiles.erase(itToErase);
		}
	}

	return TRUE;
}