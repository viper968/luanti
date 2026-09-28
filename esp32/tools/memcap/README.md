# memcap

`LD_PRELOAD` heap tracker/limiter, used to size `luantiserver` against the
ESP32-S3's PSRAM on a PC. It counts live heap bytes (`malloc_usable_size`) and
the peak. With `MEMCAP_LIMIT_KB`, allocations beyond the limit fail, like
running out of PSRAM on the board.

```bash
gcc -O2 -shared -fPIC -o libmemcap.so memcap.c -ldl -lpthread
MEMCAP_LIMIT_KB=8192 MEMCAP_REPORT=/tmp/mem.txt \
    LD_PRELOAD=$PWD/libmemcap.so ./bin/luantiserver ...
cat /tmp/mem.txt   # "live_kb peak_kb failed_allocs", rewritten every second
```

Only heap is counted. Code, thread stacks and mmapped files are not (on the
ESP32 those are flash or sized separately). On a 64-bit PC, pointer-heavy
structures are larger than on the 32-bit ESP32, so PC numbers are an upper bound.
