// Compares the memory use of the mapblock codec with zstd as Luanti uses it.
//
// Build (from the repository root, Linux/glibc only):
//   g++ -O2 -std=c++17 -Isrc util/mapblock_codec/memory.cpp src/mapblock_codec.cpp
//       -o mbmemory -lzstd -lsqlite3
//
// Usage:
//   mbmemory <world.sqlite,...>   (blocks in version 29 or 30)
//
// Every heap allocation in the process (including inside libzstd) is counted
// by interposing malloc & co. Each scenario runs in a fresh child process so
// that per-thread caches start out empty, like on a new emerge thread.
// Reported per scenario:
//   retained  heap still held after the run (per-thread contexts, caches)
//   peak      highest heap use above the starting point at any moment
//   max RSS   peak resident set size of the child process (includes the
//             loaded test data, which is the same for every scenario)

#include "mapblock_codec.h"
#include <sqlite3.h>
#include <zstd.h>
#include <malloc.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

/*
	Heap accounting
*/

extern "C" {
void *__libc_malloc(size_t);
void *__libc_calloc(size_t, size_t);
void *__libc_realloc(void *, size_t);
void *__libc_memalign(size_t, size_t);
void __libc_free(void *);
}

static long long g_heap = 0, g_peak = 0;

static inline void *track(void *p)
{
	if (p) {
		g_heap += malloc_usable_size(p);
		if (g_heap > g_peak)
			g_peak = g_heap;
	}
	return p;
}

extern "C" {
void *malloc(size_t n) { return track(__libc_malloc(n)); }
void *calloc(size_t a, size_t b) { return track(__libc_calloc(a, b)); }
void free(void *p)
{
	if (p)
		g_heap -= malloc_usable_size(p);
	__libc_free(p);
}
void *realloc(void *p, size_t n)
{
	if (p)
		g_heap -= malloc_usable_size(p);
	return track(__libc_realloc(p, n));
}
void *memalign(size_t a, size_t n) { return track(__libc_memalign(a, n)); }
void *aligned_alloc(size_t a, size_t n) { return track(__libc_memalign(a, n)); }
int posix_memalign(void **out, size_t a, size_t n)
{
	*out = track(__libc_memalign(a, n));
	return *out ? 0 : ENOMEM;
}
}

/*
	zstd, exactly as src/serialization.cpp uses it
*/

struct ZSTD_Deleter {
	void operator()(ZSTD_CStream *s) { ZSTD_freeCStream(s); }
	void operator()(ZSTD_DStream *s) { ZSTD_freeDStream(s); }
};

static void compressZstd(const std::string &data, std::ostream &os, int level)
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

static void decompressZstd(std::istream &is, std::ostream &os)
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
				return;
		}
		ret = ZSTD_decompressStream(stream.get(), &output, &input);
		os.write(output_buffer, output.pos);
		output.pos = 0;
	} while (ret != 0 && !ZSTD_isError(ret));
}

/*
	Test data
*/

static void load(const std::string &path, std::vector<std::string> &raws)
{
	sqlite3 *db;
	if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
		fprintf(stderr, "cannot open %s\n", path.c_str());
		exit(1);
	}
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db, "SELECT data FROM blocks", -1, &st, nullptr);
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *d = (const char *)sqlite3_column_blob(st, 0);
		int n = sqlite3_column_bytes(st, 0);
		if (n < 2)
			continue;
		std::istringstream is(std::string(d + 1, n - 1), std::ios::binary);
		std::ostringstream os(std::ios::binary);
		if ((unsigned char)d[0] == 29)
			decompressZstd(is, os);
		else if ((unsigned char)d[0] == 30)
			mapblock_codec::decompress(is, os);
		else
			continue;
		raws.push_back(os.str());
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
}

// Runs fn in a child process and reports its memory use
static void scenario(const char *name, const std::function<void()> &fn)
{
	fflush(stdout);
	int pipefd[2];
	if (pipe(pipefd) != 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		close(pipefd[0]);
		long long base = g_heap;
		g_peak = g_heap;
		fn();
		long long r[2] = { g_heap - base, g_peak - base };
		if (write(pipefd[1], r, sizeof r) != sizeof r)
			_exit(1);
		_exit(0);
	}
	close(pipefd[1]);
	long long r[2] = {};
	ssize_t got = read(pipefd[0], r, sizeof r);
	close(pipefd[0]);
	int status;
	struct rusage ru;
	wait4(pid, &status, 0, &ru);
	if (got != sizeof r) {
		printf("%-34s FAILED\n", name);
		return;
	}
	printf("%-34s retained %8.1f KiB   peak %8.1f KiB   max RSS %7.1f MiB\n",
		name, r[0] / 1024.0, r[1] / 1024.0, ru.ru_maxrss / 1024.0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <world.sqlite,...>\n", argv[0]);
		return 1;
	}
	// Test data is prepared in a child process so that this process never
	// touches either codec: otherwise every scenario would inherit warm
	// contexts and priors and their cost would not show up.
	char tmpl[] = "/tmp/mbmemoryXXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0)
		return 1;
	unlink(tmpl);
	pid_t pid = fork();
	if (pid == 0) {
		std::vector<std::string> raws;
		std::string list = argv[1];
		for (size_t p; ; list = list.substr(p + 1)) {
			p = list.find(',');
			load(list.substr(0, p), raws);
			if (p == std::string::npos)
				break;
		}
		std::string out;
		auto put = [&](const std::string &s) {
			uint32_t n = s.size();
			out.append((const char *)&n, 4);
			out += s;
		};
		for (auto &r : raws) {
			std::ostringstream os1(std::ios::binary), os2(std::ios::binary);
			compressZstd(r, os1, 0);
			mapblock_codec::compress(r, os2);
			put(r);
			put(os1.str());
			put(os2.str());
		}
		_exit(write(fd, out.data(), out.size()) == (ssize_t)out.size() ? 0 : 1);
	}
	int status;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 1;

	std::vector<std::string> raws, z29, c30;
	{
		off_t size = lseek(fd, 0, SEEK_END);
		std::string all(size, '\0');
		if (pread(fd, all.data(), size, 0) != size)
			return 1;
		close(fd);
		std::vector<std::string> *dst[3] = { &raws, &z29, &c30 };
		for (size_t p = 0, k = 0; p < all.size(); k++) {
			uint32_t n;
			memcpy(&n, all.data() + p, 4);
			dst[k % 3]->push_back(all.substr(p + 4, n));
			p += 4 + n;
		}
	}
	printf("%zu blocks\n\n", raws.size());

	auto zc = [&](int level) {
		return [&, level] {
			for (auto &r : raws) {
				std::ostringstream os(std::ios::binary);
				compressZstd(r, os, level);
			}
		};
	};
	// Luanti maps its levels -1..9 to zstd 0 (default, = 3) .. 10
	scenario("compress   zstd (Luanti -1)", zc(0));
	scenario("compress   zstd (Luanti 9)", zc(10));
	scenario("compress   mapblock_codec", [&] {
		for (auto &r : raws) {
			std::ostringstream os(std::ios::binary);
			mapblock_codec::compress(r, os);
		}
	});
	scenario("decompress zstd", [&] {
		for (auto &z : z29) {
			std::istringstream is(z, std::ios::binary);
			std::ostringstream os(std::ios::binary);
			decompressZstd(is, os);
		}
	});
	scenario("decompress mapblock_codec", [&] {
		for (auto &c : c30) {
			std::istringstream is(c, std::ios::binary);
			std::ostringstream os(std::ios::binary);
			mapblock_codec::decompress(is, os);
		}
	});
	return 0;
}
