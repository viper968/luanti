// Measures the mapblock codec on a whole world without loading it into memory.
//
// Build (from the repository root):
//   g++ -O2 -std=c++17 -pthread -Isrc util/mapblock_codec/scan.cpp src/mapblock_codec.cpp
//       -o mbscan -lzstd -lsqlite3
//
// Usage:
//   mbscan <map.sqlite> [threads]
//
// Every version 29 block is decompressed, recompressed with zstd at the
// levels Luanti uses (setting -1 and 9) and with the codec, decoded again and
// checked to round-trip. Older blocks are counted but skipped.
// zstd is driven exactly like src/serialization.cpp does (streaming API,
// per-thread contexts), so its timings are what the engine sees.

#include "common.h"
#include "mapblock_codec.h"
#include <sqlite3.h>
#include <zstd.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

namespace {

struct ZSTD_Deleter {
	void operator()(ZSTD_CStream *s) { ZSTD_freeCStream(s); }
	void operator()(ZSTD_DStream *s) { ZSTD_freeDStream(s); }
};

// Same as compressZstd() in src/serialization.cpp
void compressZstd(const std::string &data, std::ostream &os, int level)
{
	thread_local std::unique_ptr<ZSTD_CStream, ZSTD_Deleter> stream(ZSTD_createCStream());
	ZSTD_initCStream(stream.get(), level);
	const size_t bufsize = 16384;
	char output_buffer[bufsize];
	ZSTD_inBuffer input = { data.data(), data.size(), 0 };
	ZSTD_outBuffer output = { output_buffer, bufsize, 0 };
	while (input.pos < input.size) {
		ZSTD_compressStream(stream.get(), &output, &input);
		os.write(output_buffer, output.pos);
		output.pos = 0;
	}
	size_t ret;
	do {
		ret = ZSTD_endStream(stream.get(), &output);
		os.write(output_buffer, output.pos);
		output.pos = 0;
	} while (ret != 0 && !ZSTD_isError(ret));
}

// Same as decompressZstd() in src/serialization.cpp; false on error
bool decompressZstd(std::istream &is, std::ostream &os)
{
	thread_local std::unique_ptr<ZSTD_DStream, ZSTD_Deleter> stream(ZSTD_createDStream());
	ZSTD_initDStream(stream.get());
	const size_t bufsize = 16384;
	char output_buffer[bufsize];
	char input_buffer[bufsize];
	ZSTD_outBuffer output = { output_buffer, bufsize, 0 };
	ZSTD_inBuffer input = { input_buffer, 0, 0 };
	size_t ret;
	do {
		if (input.size == input.pos) {
			is.read(input_buffer, bufsize);
			input.size = is.gcount();
			input.pos = 0;
			if (input.size == 0)
				return false;
		}
		ret = ZSTD_decompressStream(stream.get(), &output, &input);
		if (ZSTD_isError(ret))
			return false;
		os.write(output_buffer, output.pos);
		output.pos = 0;
	} while (ret != 0);
	return true;
}

struct Stats {
	u64 blocks = 0, stored = 0, zDefault = 0, z9 = 0, codec = 0, bad = 0, raw = 0;
	double tzDefault = 0, tz9 = 0, tzDec = 0, tc = 0, td = 0;
	// Database blob sizes (version byte included) by block kind:
	// [0] uniform blocks (all nodes identical), [1] mixed blocks
	u64 kindBlocks[2] = {}, kindZstd[2] = {}, kindCodec[2] = {};
	// Per-block saving of the codec against zstd (Luanti default level)
	std::vector<float> savings;
	void add(const Stats &o)
	{
		blocks += o.blocks; stored += o.stored; zDefault += o.zDefault; z9 += o.z9;
		codec += o.codec; bad += o.bad; raw += o.raw;
		tzDefault += o.tzDefault; tz9 += o.tz9; tzDec += o.tzDec; tc += o.tc; td += o.td;
		for (int k = 0; k < 2; k++) {
			kindBlocks[k] += o.kindBlocks[k]; kindZstd[k] += o.kindZstd[k]; kindCodec[k] += o.kindCodec[k];
		}
		savings.insert(savings.end(), o.savings.begin(), o.savings.end());
	}
};

std::string canonical(const std::string &raw)
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

void process(const std::string &blob, Stats &st)
{
	// blob excludes the version byte
	std::string raw;
	{
		std::istringstream is(blob, std::ios::binary);
		std::ostringstream os(std::ios::binary);
		Timer t0;
		if (!decompressZstd(is, os)) {
			st.bad++;
			return;
		}
		st.tzDec += t0.sec();
		raw = os.str();
	}

	// Luanti maps its levels -1..9 to zstd 0 (= default) .. 10
	size_t zsize;
	{
		std::ostringstream os(std::ios::binary);
		Timer t1;
		compressZstd(raw, os, 0);
		st.tzDefault += t1.sec();
		zsize = os.str().size();
		st.zDefault += zsize;
	}
	{
		std::ostringstream os(std::ios::binary);
		Timer t2;
		compressZstd(raw, os, 10);
		st.tz9 += t2.sec();
		st.z9 += os.str().size();
	}

	std::ostringstream os(std::ios::binary);
	Timer t3;
	mapblock_codec::compress(raw, os);
	st.tc += t3.sec();
	const std::string enc = os.str();
	st.codec += enc.size();

	std::istringstream is(enc, std::ios::binary);
	std::ostringstream dec(std::ios::binary);
	Timer t4;
	try {
		mapblock_codec::decompress(is, dec);
	} catch (std::exception &e) {
		st.bad++;
		return;
	}
	st.td += t4.sec();
	if (canonical(dec.str()) != canonical(raw))
		st.bad++;
	st.blocks++;
	st.stored += blob.size() + 1;
	st.raw += raw.size();

	const Block b = parseRaw(raw);
	bool uniform = true;
	for (int i = 1; i < NODES && uniform; i++)
		uniform = b.content[i] == b.content[0] && b.p1[i] == b.p1[0] && b.p2[i] == b.p2[0];
	const int kind = uniform ? 0 : 1;
	st.kindBlocks[kind]++;
	st.kindZstd[kind] += zsize + 1;
	st.kindCodec[kind] += enc.size() + 1;
	st.savings.push_back(1.0f - (float)(enc.size() + 1) / (float)(zsize + 1));
}

}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <map.sqlite> [threads]\n", argv[0]);
		return 1;
	}
	const int nthreads = argc > 2 ? atoi(argv[2]) : (int)std::thread::hardware_concurrency();

	std::mutex mtx;
	std::condition_variable cv_work, cv_space;
	std::deque<std::vector<std::string>> queue;
	bool done = false;
	Stats total;

	std::vector<std::thread> workers;
	for (int w = 0; w < nthreads; w++) {
		workers.emplace_back([&] {
			Stats st;
			for (;;) {
				std::vector<std::string> batch;
				{
					std::unique_lock<std::mutex> lock(mtx);
					cv_work.wait(lock, [&] { return !queue.empty() || done; });
					if (queue.empty())
						break;
					batch = std::move(queue.front());
					queue.pop_front();
				}
				cv_space.notify_one();
				for (auto &b : batch)
					process(b, st);
			}
			std::lock_guard<std::mutex> lock(mtx);
			total.add(st);
		});
	}

	sqlite3 *db;
	if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db, "SELECT data FROM blocks", -1, &st, nullptr);
	u64 seen = 0, other = 0, otherBytes = 0;
	Timer wall;
	std::vector<std::string> batch;
	auto push = [&] {
		std::unique_lock<std::mutex> lock(mtx);
		cv_space.wait(lock, [&] { return queue.size() < 64; });
		queue.push_back(std::move(batch));
		batch.clear();
		cv_work.notify_one();
	};
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *d = (const char *)sqlite3_column_blob(st, 0);
		int n = sqlite3_column_bytes(st, 0);
		if (++seen % 500000 == 0)
			fprintf(stderr, "  %llu blocks read, %.0f s\n", (unsigned long long)seen, wall.sec());
		if (n < 2 || (u8)d[0] != 29) {
			other++;
			otherBytes += n;
			continue;
		}
		batch.emplace_back(d + 1, n - 1);
		if (batch.size() == 1024)
			push();
	}
	if (!batch.empty())
		push();
	sqlite3_finalize(st);
	sqlite3_close(db);
	{
		std::lock_guard<std::mutex> lock(mtx);
		done = true;
	}
	cv_work.notify_all();
	for (auto &t : workers)
		t.join();

	const double nb = std::max<u64>(total.blocks, 1);
	printf("%llu blocks in version 29 (%llu older blocks, %.1f MB, skipped), %.0f s\n",
		(unsigned long long)total.blocks, (unsigned long long)other, otherBytes / 1e6, wall.sec());
	printf("%-28s %12s %8s %14s\n", "", "bytes", "vs. -1", "per block");
	auto row = [&](const char *name, u64 bytes, double t) {
		printf("%-28s %12llu %7.1f%% ", name, (unsigned long long)bytes,
			100.0 * bytes / std::max<u64>(total.zDefault, 1));
		if (t > 0)
			printf("  compress %5.1f us", t * 1e6 / nb);
		printf("\n");
	};
	row("as stored in the database", total.stored, 0);
	row("zstd, Luanti setting -1", total.zDefault, total.tzDefault);
	row("zstd, Luanti setting 9", total.z9, total.tz9);
	row("mapblock codec", total.codec, total.tc);
	printf("decompress: zstd %.1f us/block, mapblock codec %.1f us/block\n",
		total.tzDec * 1e6 / nb, total.td * 1e6 / nb);

	// Per-block view: database blob sizes including the version byte, so
	// this is exactly what each block costs inside the database, without
	// any SQLite overhead.
	printf("\nper block (blob bytes incl. version byte, no database overhead):\n");
	const u64 zb = total.zDefault + total.blocks, cb = total.codec + total.blocks;
	printf("  uncompressed %.0f B/block | zstd %.1f B/block (%.1fx) | codec %.1f B/block (%.1fx)\n",
		total.raw / nb, zb / nb, (double)total.raw / zb, cb / nb, (double)total.raw / cb);
	printf("  saving vs zstd: total bytes %.1f%%", 100.0 * (1.0 - (double)cb / zb));
	if (!total.savings.empty()) {
		auto &v = total.savings;
		double mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		printf(" | per-block mean %.1f%% | per-block median %.1f%%", 100.0 * mean, 100.0 * v[v.size() / 2]);
	}
	printf("\n");
	const char *kinds[2] = {"uniform", "mixed"};
	for (int k = 0; k < 2; k++) {
		const double n = std::max<u64>(total.kindBlocks[k], 1);
		printf("  %-7s %9llu blocks (%4.1f%%): zstd %7.1f B/block, codec %7.1f B/block, saving %.1f%%\n",
			kinds[k], (unsigned long long)total.kindBlocks[k], 100.0 * total.kindBlocks[k] / nb,
			total.kindZstd[k] / n, total.kindCodec[k] / n,
			100.0 * (1.0 - (double)total.kindCodec[k] / std::max<u64>(total.kindZstd[k], 1)));
	}
	printf("round trip: %s (%llu failures)\n", total.bad ? "FAILED" : "ok",
		(unsigned long long)total.bad);
	return total.bad ? 1 : 0;
}
