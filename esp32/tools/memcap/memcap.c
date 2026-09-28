// memcap: LD_PRELOAD heap tracker/limiter for sizing luantiserver against the
// ESP32-S3's PSRAM. It counts live heap bytes (malloc_usable_size) and records
// the peak. With MEMCAP_LIMIT_KB set, allocations that would push live heap
// past the limit fail, like running out of PSRAM on the board.
//
//   MEMCAP_LIMIT_KB=8192 MEMCAP_REPORT=/tmp/mem.txt
//   LD_PRELOAD=./libmemcap.so ./bin/luantiserver ...
//
// The report file is rewritten every second: "live_kb peak_kb failed_allocs".
// Heap only: code, thread stacks and mmapped files are not counted (on the
// ESP32 those live in flash or are sized separately).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *(*real_malloc)(size_t);
static void (*real_free)(void *);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static int (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);
static void *(*real_memalign)(size_t, size_t);

static atomic_long live, peak, failed;
static long limit; // bytes, 0 = unlimited

// dlsym may allocate before the real functions are known
static char boot_buf[64 * 1024];
static size_t boot_used;
static int in_boot(void *p)
{
	return (char *)p >= boot_buf && (char *)p < boot_buf + sizeof(boot_buf);
}

static void resolve(void)
{
	static int busy;
	if (real_malloc || busy)
		return;
	busy = 1;
	real_malloc = dlsym(RTLD_NEXT, "malloc");
	real_free = dlsym(RTLD_NEXT, "free");
	real_calloc = dlsym(RTLD_NEXT, "calloc");
	real_realloc = dlsym(RTLD_NEXT, "realloc");
	real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
	real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
	real_memalign = dlsym(RTLD_NEXT, "memalign");
	busy = 0;
}

static int reserve(size_t n)
{
	long now = atomic_fetch_add(&live, (long)n) + (long)n;
	if (limit && now > limit) {
		atomic_fetch_sub(&live, (long)n);
		atomic_fetch_add(&failed, 1);
		return 0;
	}
	long p = atomic_load(&peak);
	while (now > p && !atomic_compare_exchange_weak(&peak, &p, now))
		;
	return 1;
}

// Account with the usable size, which is what the block really occupies
static void *track(void *p)
{
	if (p && !reserve(malloc_usable_size(p))) {
		real_free(p);
		return NULL;
	}
	return p;
}

void *malloc(size_t n)
{
	resolve();
	if (!real_malloc) {
		size_t a = (n + 15) & ~(size_t)15;
		if (boot_used + a > sizeof(boot_buf))
			return NULL;
		void *p = boot_buf + boot_used;
		boot_used += a;
		return p;
	}
	return track(real_malloc(n));
}

void *calloc(size_t a, size_t b)
{
	resolve();
	if (!real_calloc) {
		void *p = malloc(a * b);
		if (p)
			memset(p, 0, a * b);
		return p;
	}
	return track(real_calloc(a, b));
}

void free(void *p)
{
	if (!p || in_boot(p))
		return;
	resolve();
	atomic_fetch_sub(&live, (long)malloc_usable_size(p));
	real_free(p);
}

void *realloc(void *p, size_t n)
{
	resolve();
	if (!p)
		return malloc(n);
	if (in_boot(p)) {
		void *q = malloc(n);
		if (q)
			memcpy(q, p, n); // boot blocks are small; over-read stays in boot_buf
		return q;
	}
	if (n == 0) {
		free(p);
		return NULL;
	}
	size_t old = malloc_usable_size(p);
	// Pessimistically reserve the growth first so the limit is honoured
	if (n > old && !reserve(n - old))
		return NULL;
	void *q = real_realloc(p, n);
	if (!q) {
		if (n > old)
			atomic_fetch_sub(&live, (long)(n - old));
		return NULL;
	}
	// Correct from the estimate (n) to the true usable size
	long est = n > old ? (long)n : (long)old;
	atomic_fetch_add(&live, (long)malloc_usable_size(q) - est);
	return q;
}

int posix_memalign(void **out, size_t align, size_t n)
{
	resolve();
	int r = real_posix_memalign(out, align, n);
	if (r == 0 && !track(*out)) {
		*out = NULL;
		return 12; // ENOMEM
	}
	return r;
}

void *aligned_alloc(size_t align, size_t n)
{
	resolve();
	return track(real_aligned_alloc(align, n));
}

void *memalign(size_t align, size_t n)
{
	resolve();
	return track(real_memalign(align, n));
}

static const char *report_path;

static void write_report(void)
{
	if (!report_path)
		return;
	FILE *f = fopen(report_path, "w");
	if (!f)
		return;
	fprintf(f, "%ld %ld %ld\n", atomic_load(&live) / 1024, atomic_load(&peak) / 1024,
		atomic_load(&failed));
	fclose(f);
}

static void *reporter(void *arg)
{
	(void)arg;
	for (;;) {
		write_report();
		sleep(1);
	}
	return NULL;
}

// On a crash, print a backtrace to stderr (binary+offset; resolve with
// addr2line -f -e <binary> <offset>), then die with the original signal.
static void crash_handler(int sig)
{
	void *frames[64];
	int n = backtrace(frames, 64);
	char msg[64];
	int len = snprintf(msg, sizeof(msg), "\n[memcap] fatal signal %d, backtrace:\n", sig);
	write(2, msg, len);
	backtrace_symbols_fd(frames, n, 2);
	signal(sig, SIG_DFL);
	raise(sig);
}

__attribute__((constructor)) static void memcap_init(void)
{
	resolve();
	if (getenv("MEMCAP_BACKTRACE")) {
		// Load libgcc's unwinder now; backtrace() would otherwise do it inside the handler
		void *warm[2];
		backtrace(warm, 2);
		static char altstack[64 * 1024];
		stack_t ss = {.ss_sp = altstack, .ss_size = sizeof(altstack)};
		sigaltstack(&ss, NULL);
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = crash_handler;
		sa.sa_flags = SA_ONSTACK;
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
		sigaction(SIGFPE, &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
	}
	const char *l = getenv("MEMCAP_LIMIT_KB");
	if (l)
		limit = atol(l) * 1024;
	report_path = getenv("MEMCAP_REPORT");
	if (report_path) {
		pthread_t t;
		pthread_create(&t, NULL, reporter, NULL);
		pthread_detach(t);
	}
}

__attribute__((destructor)) static void memcap_fini(void)
{
	write_report();
	fprintf(stderr, "[memcap] peak heap %ld KB, live at exit %ld KB, failed allocations %ld\n",
		atomic_load(&peak) / 1024, atomic_load(&live) / 1024, atomic_load(&failed));
}
