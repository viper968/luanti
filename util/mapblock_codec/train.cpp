// Trains the priors of the engine's mapblock codec (src/mapblock_codec.cpp)
// and benchmarks it against zstd.
//
// Build (from the repository root):
//   g++ -O2 -std=c++17 -DMAPBLOCK_CODEC_TRAINING -Isrc util/mapblock_codec/train.cpp
//       src/mapblock_codec.cpp -o mbtrain -lzstd -lsqlite3
//
// Usage:
//   mbtrain <train.sqlite,...> <test.sqlite,...> [out_priors.h]
//
// Worlds must be stored in serialization version 29 (zstd). With
// out_priors.h given, the trained priors are written there (normally
// src/mapblock_codec_priors.h). Set NOZSTD=1 to skip the zstd baselines.
// Without training (TRAIN = "-") the priors compiled into the codec are used.

#include "common.h"
#include "mapblock_codec.h"
#include <sqlite3.h>
#include <zstd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <sstream>

namespace mbc = mapblock_codec;

static std::string zstdCompress(const std::string &s, int level)
{
	std::string o(ZSTD_compressBound(s.size()), 0);
	o.resize(ZSTD_compress(o.data(), o.size(), s.data(), s.size(), level));
	return o;
}

static std::string zstdDecompress(const std::string &s)
{
	thread_local std::string buf(1 << 24, 0);
	size_t r = ZSTD_decompress(buf.data(), buf.size(), s.data(), s.size());
	if (ZSTD_isError(r))
		throw std::runtime_error("zstd");
	return buf.substr(0, r);
}

// Loads all version 29 blocks of a world as uncompressed streams
static void load(const std::string &path, std::vector<std::string> &out)
{
	sqlite3 *db;
	if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
		fprintf(stderr, "cannot open %s\n", path.c_str());
		exit(1);
	}
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db, "SELECT data FROM blocks", -1, &st, nullptr);
	size_t skipped = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *d = (const char *)sqlite3_column_blob(st, 0);
		int n = sqlite3_column_bytes(st, 0);
		if (n < 2 || (u8)d[0] != 29) {
			skipped++;
			continue;
		}
		out.push_back(zstdDecompress(std::string(d + 1, n - 1)));
	}
	if (skipped)
		fprintf(stderr, "%s: skipped %zu blocks not in version 29\n", path.c_str(), skipped);
	sqlite3_finalize(st);
	sqlite3_close(db);
}

static std::vector<std::string> splitList(std::string s)
{
	std::vector<std::string> r;
	size_t p;
	while ((p = s.find(',')) != std::string::npos) {
		r.push_back(s.substr(0, p));
		s = s.substr(p + 1);
	}
	r.push_back(s);
	return r;
}

// Name-order independent form of an uncompressed block
static std::string canonical(const std::string &raw)
{
	Block b = parseRaw(raw);
	std::vector<u16> order(b.names.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&](u16 a, u16 c) { return b.names[a] < b.names[c]; });
	std::vector<u16> remap(b.names.size());
	std::vector<std::string> names(b.names.size());
	for (size_t i = 0; i < order.size(); i++) {
		remap[order[i]] = i;
		names[i] = b.names[order[i]];
	}
	b.names.swap(names);
	for (int i = 0; i < NODES; i++)
		b.content[i] = remap.at(b.content[i]);
	return writeRaw(b);
}

static std::string encode(const std::string &raw)
{
	std::ostringstream os(std::ios::binary);
	mbc::compress(raw, os);
	return os.str();
}

static std::string decode(const std::string &data)
{
	std::istringstream is(data, std::ios::binary);
	std::ostringstream os(std::ios::binary);
	mbc::decompress(is, os);
	if ((size_t)is.tellg() != data.size())
		throw std::runtime_error("decoder did not consume exactly its input");
	return os.str();
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <train.sqlite,...|-> <test.sqlite,...> [out_priors.h]\n", argv[0]);
		return 1;
	}
	std::vector<std::string> train, test;
	if (std::string(argv[1]) != "-")
		for (auto &f : splitList(argv[1]))
			load(f, train);
	for (auto &f : splitList(argv[2]))
		load(f, test);
	printf("train blocks %zu, test blocks %zu\n", train.size(), test.size());

	if (!train.empty()) {
		mbc::training::useFlatPriors();
		// Two passes: statistics of the second pass are gathered with
		// trained models, which is how they will be used.
		for (int pass = 0; pass < 2; pass++) {
			mbc::training::beginStats();
			for (auto &raw : train)
				encode(raw);
			mbc::training::usePriorsFromStats();
		}
		if (argc > 3) {
			std::ofstream f(argv[3], std::ios::binary);
			f << mbc::training::priorsSource();
			printf("wrote %s\n", argv[3]);
		}
	}

	size_t base = 0;
	auto report = [&](const char *name, size_t total, double ct, double dt) {
		if (!base)
			base = total;
		printf("%-14s %10zu bytes (%5.1f%%)  comp %7.1f us/blk  decomp %5.1f us/blk\n",
			name, total, 100.0 * total / base, ct * 1e6 / test.size(), dt * 1e6 / test.size());
	};

	if (!getenv("NOZSTD")) {
		for (int level : {3, 9, 19}) {
			std::vector<std::string> out(test.size());
			Timer t;
			size_t total = 0;
			for (size_t i = 0; i < test.size(); i++)
				total += (out[i] = zstdCompress(test[i], level)).size();
			double ct = t.sec();
			Timer t2;
			for (auto &o : out)
				zstdDecompress(o);
			char name[32];
			snprintf(name, sizeof name, "zstd -%d", level);
			report(name, total, ct, t2.sec());
		}
	}

	mbc::training::beginStats();
	std::vector<std::string> out(test.size());
	Timer t;
	size_t total = 0;
	for (size_t i = 0; i < test.size(); i++)
		total += (out[i] = encode(test[i])).size();
	double ct = t.sec();
	const double *cost = mbc::training::costBytes();
	Timer t2;
	std::vector<std::string> dec(test.size());
	for (size_t i = 0; i < test.size(); i++)
		dec[i] = decode(out[i]);
	double dt = t2.sec();
	report("mapblock_codec", total, ct, dt);
	printf("estimated bytes: rows %.0f  whole-node %.0f  content %.0f  param1 %.0f  param2 %.0f"
		"  header %.0f  names %.0f  mono %.0f\n",
		cost[7], cost[0], cost[1], cost[2], cost[3], cost[4], cost[5], cost[6]);

	size_t bad = 0;
	for (size_t i = 0; i < test.size(); i++)
		bad += canonical(dec[i]) != canonical(test[i]);
	printf("round trip: %s (%zu mismatches)\n", bad ? "FAILED" : "ok", bad);
	return bad ? 1 : 0;
}
