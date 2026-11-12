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

// The content check a received map preview has to pass. Engine-free so a test can drive it.
// It decides whether remote bytes are written to disk under a .tga name, and the preview is the first leg of the map
// transfer, so refusing it means the map is never sent. It validates the header a decoder reads and that uncompressed
// images are not truncated. The TGA 2.0 footer is optional and not required.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace TransferValidation
{

	// The on-disk header, in bytes. This equals sizeof(TGAHeader) from WWLib/TARGA.h today, because
	// that header wraps its structs in #pragma pack(1) - but it is spelled out here so this file
	// needs no engine include, and so the value cannot silently follow a change to that packing.
	// tga-validate-test.cpp asserts the two still agree.
	static const uint32_t TGA_HEADER_BYTES = 18;

	// Image type codes, as WWLib/TARGA.h defines them. Repeated rather than included so this header
	// stays compilable on its own; the test asserts they still agree with TARGA.h.
	static const uint32_t TGA_TYPE_NOIMAGE           = 0;
	static const uint32_t TGA_TYPE_CMAPPED           = 1;
	static const uint32_t TGA_TYPE_TRUECOLOR         = 2;
	static const uint32_t TGA_TYPE_MONO              = 3;
	static const uint32_t TGA_TYPE_CMAPPED_ENCODED   = 9;
	static const uint32_t TGA_TYPE_TRUECOLOR_ENCODED = 10;
	static const uint32_t TGA_TYPE_MONO_ENCODED      = 11;

	inline bool isValidPixelDepth( uint32_t depth )
	{
		return depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32;
	}

	/**
	 * TRUE when the buffer is a structurally sound TARGA image, v1 or v2.
	 *
	 * On FALSE, *whyNot (when non-null) is set to a static string naming the first failed test, so
	 * the caller can log a reason rather than a bare refusal. It is never set on success.
	 */
	inline bool isValidTgaImage( const uint8_t *data, uint32_t dataSize, const char **whyNot = 0 )
	{
		const char *reason = 0;
		if (data == 0 || dataSize < TGA_HEADER_BYTES)
		{
			reason = "too small to hold a TARGA header";
		}
		else
		{
			const uint32_t idLength     = data[0];
			const uint32_t colorMapType = data[1];
			const uint32_t imageType    = data[2];
			const uint32_t cMapLength   = (uint32_t)data[5]  | ((uint32_t)data[6]  << 8);
			const uint32_t cMapDepth    = data[7];
			const uint32_t width        = (uint32_t)data[12] | ((uint32_t)data[13] << 8);
			const uint32_t height       = (uint32_t)data[14] | ((uint32_t)data[15] << 8);
			const uint32_t pixelDepth   = data[16];

			const bool uncompressed = (imageType == TGA_TYPE_CMAPPED)
				|| (imageType == TGA_TYPE_TRUECOLOR)
				|| (imageType == TGA_TYPE_MONO);
			const bool encoded = (imageType == TGA_TYPE_CMAPPED_ENCODED)
				|| (imageType == TGA_TYPE_TRUECOLOR_ENCODED)
				|| (imageType == TGA_TYPE_MONO_ENCODED);

			// 64-bit throughout below: width and height are each up to 65535, so the product times
			// four bytes overflows 32 bits, and an overflow here would turn "truncated" into "fits".
			const uint64_t paletteBytes = (colorMapType == 1)
				? (uint64_t)cMapLength * (uint64_t)((cMapDepth + 7) / 8)
				: (uint64_t)0;
			const uint64_t pixelsBegin = (uint64_t)TGA_HEADER_BYTES + (uint64_t)idLength + paletteBytes;

			if (colorMapType > 1)
				reason = "invalid colour map type";
			// TGA_TYPE_NOIMAGE is deliberately refused: a preview with no pixels is not a preview,
			// and accepting it would let a zero-image header carry an arbitrary tail.
			else if (!uncompressed && !encoded)
				reason = "unsupported image type";
			else if (!isValidPixelDepth(pixelDepth))
				reason = "invalid pixel depth";
			else if (width == 0 || height == 0)
				reason = "zero width or height";
			else if (colorMapType == 1 && !isValidPixelDepth(cMapDepth))
				reason = "invalid colour map depth";
			else if (pixelsBegin >= (uint64_t)dataSize)
				reason = "no pixel data after the header and palette";
			else if (uncompressed
				&& ((uint64_t)dataSize - pixelsBegin)
					< ((uint64_t)width * (uint64_t)height * (uint64_t)((pixelDepth + 7) / 8)))
				reason = "truncated - the declared image does not fit in the file";
			// A run-length encoded image cannot be sized without decoding it, so a sane header plus
			// at least one byte of payload is all that can honestly be asserted without running a
			// decoder over hostile input - which is the thing this guard exists to avoid doing.
		}

		if (reason != 0 && whyNot != 0)
			*whyNot = reason;
		return reason == 0;
	}

} // namespace TransferValidation
