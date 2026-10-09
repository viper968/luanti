// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <iostream>
#include <string>
#include <string_view>

/*
	Mapblock codec used for on-disk serialization version 30.

	It replaces the zstd layer of version 29: the input is exactly the
	uncompressed byte stream that MapBlock::serialize() produces for a disk
	block of version >= 29 (flags, lighting, timestamp, name-id mapping,
	bulk node data, then metadata/static objects/node timers).
	Node data is coded with a spatial context model and a binary arithmetic
	coder; see doc/world_format.md for an overview.

	The output is self-delimiting. Decoding produces a semantically identical
	stream; the order of the name-id mapping may differ (ids are reassigned in
	name order), which is invisible after MapBlock::correctBlockNodeIds().
*/
namespace mapblock_codec
{

// Throws SerializationError if `raw` is not a valid disk block.
void compress(std::string_view raw, std::ostream &os);

// Reads one compressed block from `is` and writes the uncompressed stream to `os`.
// Throws SerializationError on corrupt input.
void decompress(std::istream &is, std::ostream &os);

#ifdef MAPBLOCK_CODEC_TRAINING
// Hooks used by util/mapblock_codec to (re)generate mapblock_codec_priors.h.
// Not thread-safe; never enabled in engine builds.
namespace training
{
	// Use flat (untrained) priors for subsequent calls.
	void useFlatPriors();
	// Start counting model statistics during compress().
	void beginStats();
	// Stop counting and switch to priors derived from the statistics.
	void usePriorsFromStats();
	// C++ source of the current priors, suitable for mapblock_codec_priors.h.
	std::string priorsSource();
	// Estimated coded size in bytes per category since the last beginStats():
	// 0 whole-node, 1 content, 2 param1, 3 param2, 4 header, 5 names, 6 mono, 7 rows
	const double *costBytes();
}
#endif

}
