#include "common.h"
#include "codec.h"
#include <sqlite3.h>
#include <zstd.h>
#include <cstdio>
#include <cstdlib>

static std::string zc(const std::string &s, int lvl) {
	std::string o(ZSTD_compressBound(s.size()), 0);
	o.resize(ZSTD_compress(o.data(), o.size(), s.data(), s.size(), lvl));
	return o;
}
static std::string zd(const std::string &s) {
	thread_local std::string buf(1 << 22, 0);
	size_t r = ZSTD_decompress(buf.data(), buf.size(), s.data(), s.size());
	if (ZSTD_isError(r)) throw std::runtime_error("zstd");
	return buf.substr(0, r);
}
static void load(const char *path, std::vector<Block> &out) {
	sqlite3 *db;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr)) exit(1);
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db, "SELECT data FROM blocks", -1, &st, nullptr);
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *d = (const char *)sqlite3_column_blob(st, 0);
		int n = sqlite3_column_bytes(st, 0);
		if (n < 2 || (u8)d[0] != 29) continue;
		out.push_back(parseRaw(zd(std::string(d + 1, n - 1))));
	}
	sqlite3_finalize(st); sqlite3_close(db);
}

int main(int argc, char **argv) {
	// usage: bench2 train1,train2,.. test1,test2,..
	std::vector<Block> train, test;
	auto split = [](std::string s) { std::vector<std::string> r; size_t p;
		while ((p = s.find(',')) != std::string::npos) { r.push_back(s.substr(0, p)); s = s.substr(p + 1); }
		r.push_back(s); return r; };
	for (auto &f : split(argv[1])) load(f.c_str(), train);
	for (auto &f : split(argv[2])) load(f.c_str(), test);
	printf("train blocks %zu, test blocks %zu\n", train.size(), test.size());
	int conf = argc > 3 ? atoi(argv[3]) : 0;
	if (argc > 4) mbc::g_limit = atoi(argv[4]);

	// ---- train
	auto P = std::make_unique<mbc::Priors>();
	for (int pass = 0; pass < 2; pass++) {
		mbc::Stats st; mbc::g_stats = &st;
		for (auto &b : train) mbc::encode(b, *P);
		mbc::g_stats = nullptr;
		mbc::buildPriors(st, *P, conf);
	}

	std::vector<std::string> rawv;
	for (auto &b : test) rawv.push_back(writeRaw(b));
	size_t base = 0;
	auto run = [&](const char *name, auto enc, auto dec) {
		std::vector<std::string> out(test.size());
		Timer t; size_t tot = 0;
		for (size_t i = 0; i < test.size(); i++) { out[i] = enc(i); tot += out[i].size(); }
		double ct = t.sec();
		Timer t2; size_t bad = 0;
		for (size_t i = 0; i < test.size(); i++) bad += !dec(i, out[i]);
		double dt = t2.sec();
		if (!base) base = tot;
		printf("%-14s %9zu bytes (%5.1f%%)  comp %6.1f us/blk  decomp %5.1f us/blk %s\n",
			name, tot, 100.0 * tot / base, ct * 1e6 / test.size(), dt * 1e6 / test.size(),
			bad ? "ROUNDTRIP FAIL" : "");
		return tot;
	};
	if (!getenv("NOZSTD")) for (int lvl : {3, 9, 19}) {
		char nm[32]; snprintf(nm, sizeof nm, "zstd -%d", lvl);
		run(nm, [&](size_t i) { return zc(rawv[i], lvl); },
			[&](size_t i, const std::string &o) { return zd(o) == rawv[i]; });
	}
	// cost accounting pass
	{
		mbc::Stats st; mbc::g_stats = &st;
		memset(mbc::g_cost, 0, sizeof mbc::g_cost);
		for (auto &b : test) mbc::encode(b, *P);
		mbc::g_stats = nullptr;
		const char *cat[] = {"content", "param1", "param2", "header", "names", "mono", "same"};
		printf("cost bytes:"); for (int k = 0; k < 7; k++) printf(" %s %.0f", cat[k], mbc::g_cost[k] / 8); printf("\n");
	}
	{
		std::vector<std::string> enc; for (auto &b : test) enc.push_back(mbc::encode(b, *P));
		Timer t; for (auto &e : enc) { Block d = mbc::decode(e, *P); asm volatile("" :: "r"(&d) : "memory"); }
		printf("custom pure decode: %.1f us/blk\n", t.sec() * 1e6 / enc.size());
	}
	run("custom", [&](size_t i) { return mbc::encode(test[i], *P); },
		[&](size_t i, const std::string &o) {
			Block d = mbc::decode(o, *P), e = test[i];
			mbc::canonicalize(d); mbc::canonicalize(e);
			return writeRaw(d) == writeRaw(e); });
}
