# Feasibility study: running `luantiserver` on an ESP32-S3 (16 MB)

Goal: a Luanti dedicated server running on an ESP32-S3 that at least **2 players**
can join and play on, using an SD card (or SPI/USB storage) for the world and media.

TL;DR: **Possible, but only as a heavily cut-down build with a tiny game, a
pre-generated world, and a module that has 8 MB of PSRAM.** A module with
16 MB flash and *no* PSRAM (e.g. `N16`) cannot do this. An `N16R8`
(16 MB flash + 8 MB octal PSRAM) is the minimum realistic target.

---

## 1. Hardware reality check

| Resource | ESP32-S3 (N16R8) | Typical Luanti server use (Minetest Game, 2 players) |
|---|---|---|
| CPU | 2× Xtensa LX7 @ 240 MHz, single-precision FPU only | 1–4 x86/ARM cores @ GHz |
| Fast RAM | ~512 KB SRAM (≈300 KB usable after Wi-Fi/lwIP) | — |
| Slow RAM | 8 MB PSRAM (cached, ~40–80 MB/s) | 150–500 MB RSS |
| Code storage | 16 MB flash, code runs XIP through a 32–64 KB cache | ~10–15 MB binary |
| Network | 2.4 GHz Wi-Fi via lwIP (UDP ok, IPv6 optional) | — |
| Storage | SD card over SDMMC/SPI (FATFS), 1–20 MB/s | SSD |

Points that follow from that table:

* **"16 MB" is flash, not RAM.** Flash only holds the firmware (and maybe a
  read-only LittleFS partition). Everything the server keeps at runtime has to
  fit in PSRAM.
* **Lua numbers are `double`.** The S3 has no double-precision FPU, so every
  Lua arithmetic op is soft-float. Recompiling Lua with `float` numbers is **not**
  an option: `core.hash_node_position()` and similar APIs need 48-bit integers.
  Expect Lua to run 10–50× slower than on a PC.
* **LuaJIT has no Xtensa backend.** Use the bundled PUC Lua 5.1 (`lib/lua`,
  `-DENABLE_LUAJIT=OFF`).

## 2. Toolchain / OS port (ESP-IDF)

Build against **ESP-IDF v5.x** with CMake. The server build
(`-DBUILD_CLIENT=OFF -DBUILD_SERVER=ON`) already leaves out Irrlicht, OpenGL,
sound and fonts. What IDF gives you, and what needs work:

| Luanti requirement | Where in tree | ESP-IDF status / work needed |
|---|---|---|
| C++17, exceptions, RTTI | `CMakeLists.txt:10`, used everywhere | Enable `CONFIG_COMPILER_CXX_EXCEPTIONS`, `CONFIG_COMPILER_CXX_RTTI`. |
| `std::thread` / mutex / condvar | `src/threading/*` | Works through IDF's pthread layer. **Default pthread stack is ~3 KB**, so set `esp_pthread_set_cfg()` (or edit `Thread::start`) to give ~32–64 KB stacks, allocated from PSRAM (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`). |
| `sys/prctl.h`, thread naming, affinity | `src/threading/thread.cpp:33` | Add an `#elif defined(ESP_PLATFORM)` branch (no-op or `vTaskSetThreadName`-style). `getNumberOfProcessors()` should return 2. |
| BSD UDP sockets | `src/network/socket.cpp`, `address.cpp` | lwIP provides them. Set `enable_ipv6 = false` / `ipv6_server = false` (defaults are true, `defaultsettings.cpp:445`) unless lwIP IPv6 is enabled. Raise `CONFIG_LWIP_UDP_RECVMBOX_SIZE` and socket buffers. |
| Filesystem (`opendir`, `stat`, `mkdir`, `rename`) | `src/filesys.cpp` | FATFS on SD via VFS. Enable long file names (`CONFIG_FATFS_LFN_HEAP`). Watch out for `rename()` over an existing file (FAT won't do it) and for missing `realpath()`. exFAT or LittleFS-on-SD also work. |
| `porting.cpp` (exec path, signals, `sysctl`, `uname`) | `src/porting.cpp` | New `ESP_PLATFORM` branch: hard-code the share/user paths to `/sdcard/luanti`, stub signal handling, and supply `main()` from `app_main()`. |
| SQLite3 (REQUIRED) | `src/CMakeLists.txt:215` | Compile the SQLite amalgamation as an IDF component. The standard unix VFS works over FATFS with locking turned off (`SQLITE_OMIT_WAL`, `-DSQLITE_THREADSAFE=1`, `unix-none` VFS). Existing community ports exist (e.g. `siara-cc/esp32-idf-sqlite3`). |
| zlib, zstd (REQUIRED) | `src/CMakeLists.txt:267` | Both compile fine. **Build zstd with `ZSTD_HEAPMODE=1` and a low level**; `map_compression_level_net/disk` = 1–3. zstd's default window buffers are large. |
| GMP (SRP auth) | `lib/gmp` (mini-gmp bundled) | Bundled mini-gmp compiles. SRP login is slow (a few seconds of bignum math) but happens only once per join. |
| jsoncpp | `lib/jsoncpp` | Bundled, ok. |
| cURL, gettext, ncurses, LevelDB, Redis, PostgreSQL, SpatialIndex, OpenSSL, Prometheus | options in `src/CMakeLists.txt` | **Disable all**: `-DENABLE_CURL=OFF -DENABLE_GETTEXT=OFF -DENABLE_CURSES=OFF -DENABLE_LEVELDB=OFF -DENABLE_REDIS=OFF -DENABLE_POSTGRESQL=OFF -DENABLE_SPATIAL=OFF -DENABLE_OPENSSL=OFF -DBUILD_UNITTESTS=OFF -DBUILD_DOCUMENTATION=OFF` |

Binary size: with `-Os`, no unit tests and no optional libs, the server should
come to about **5–9 MB**, which fits in a single large app partition on 16 MB
flash (`partitions.csv`: one ~12 MB factory app, no OTA).

## 3. Memory budget (the real problem)

Rough numbers for 8 MB PSRAM:

| Consumer | Default behaviour | Needed for ESP32 |
|---|---|---|
| **MapBlocks** | 16³ nodes × 4 B (`MapNode`: u16+u8+u8) = **16 KiB each** plus metadata/objects. Nothing caps the server-side block count: `Server::AsyncRunStep` passes `-1` as `max_loaded_blocks` (`src/server.cpp:751-753`). With `active_block_range=4` a single player keeps (2·4+1)³ = **729 blocks ≈ 12 MB active**, and more get loaded for sending. | `active_block_range=1` (27 blocks/player), `max_block_send_distance=3–4`, `server_unload_unused_data_timeout=5`, **plus a code change**: add a `server_mapblock_limit` setting and pass it instead of `-1` in `server.cpp:753`. Target ≤ 150 loaded blocks ≈ 2.5 MB. |
| **Mapgen** (see 3a) | `mg_name=v7`, `chunksize=5` → 80³ node chunk + 16-node margin in a VoxelManipulator ≈ 112³ × 4 B ≈ **5.6 MB**, plus several float noise buffers of 2 MB each. | **Don't generate on the device.** Pre-generate the world on a PC, copy `map.sqlite` to the SD card, and use `mg_name=singlenode` (or `flat` with `chunksize=1`, ≈ 48³×4 B ≈ 440 KB) for anything outside the pre-generated area. Keep `num_emerge_threads=1`. |
| **Lua states** | Main server state + **≥1 async worker state** (`AsyncEngine::initialize` always calls `addWorkerThread()`, `src/script/cpp_api/s_async.cpp:85`) + one per emerge thread when mods register mapgen scripts. Each loads `builtin/` (~1.5 MB of source). Minetest Game uses 20–60 MB of Lua heap. | Use a **tiny game** (a few dozen nodes, no mobs, few ABMs). Precompile Lua to bytecode (`luac`) so the parser doesn't spike. Consider a patch that makes the async worker lazy or optional. Target ≤ 2 MB total Lua heap. |
| **Node/item definitions** | `ContentFeatures` is a few hundred bytes to ~1 KB per node | Fine at < 200 nodes. |
| **Network / per-client** | Reliable-packet buffers, `max_simultaneous_block_sends_per_client=40`, `max_packets_per_iteration=1024` | `max_simultaneous_block_sends_per_client=4`, `max_packets_per_iteration=64`, `max_users=2`. |
| **Media** | Server hashes and sends every texture/sound/model to each client | Keep media on the SD card and set `remote_media` to an HTTP host (a PC, GitHub Pages, …) so clients download it from there, not from the ESP32. Otherwise keep the game's media to 1–2 MB. |
| Wi-Fi + lwIP + IDF | ~60–100 KB SRAM | Fixed cost. |

Put `CONFIG_SPIRAM_USE_MALLOC=y` and
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256` (small allocations stay in SRAM,
large ones go to PSRAM). Luanti does many small `std::map` / `std::string`
allocations, so fragmentation matters. Watch `heap_caps_get_free_size()`.

### 3a. Mapgen comparison (can we generate on the device after all?)

What sets mapgen memory is the **chunk size**, more than which mapgen you pick.
Each `Noise` object holds two `float` buffers of `sx·sy·sz` points
(`Noise::allocBuffers`, `src/noise.cpp:388`). The mapgen also locks a
VoxelManipulator covering the chunk plus a 1-block margin, at 4 B/node for data
+ 1 B/node for flags.

Per-buffer sizes (csize = chunksize × 16):

| chunksize | csize | VoxelManip (≈(csize+32)³ × 5 B) | one 3D noise (≈csize²·(csize+2) × 8 B) | one 2D noise |
|---|---|---|---|---|
| 1 | 16 | **0.55 MB** | 37 KB | 2 KB |
| 2 | 32 | 1.3 MB | 0.28 MB | 8 KB |
| 3 | 48 | 2.6 MB | 0.92 MB | 18 KB |
| 5 (default) | 80 | 7.0 MB | **4.2 MB** | 51 KB |

3D noise buffers per mapgen with default flags. Counted from the constructors in
`src/mapgen/mapgen_*.cpp`: caves = `cave1` + `cave2` (`cavegen.cpp:43`),
plus `cavern` when the caverns flag is on.

| Mapgen | 3D noises | 2D noises | Peak mapgen RAM @ chunksize 1 | @ chunksize 5 | CPU notes |
|---|---|---|---|---|---|
| singlenode | 0 | 0 | ~0.55 MB (VManip only) | 7 MB | Nothing in C++. Anything interesting has to come from Lua `on_generated`, which is slow here (soft-float doubles). |
| flat | 2 (caves) | 1–2 | ~0.65 MB | ~15 MB | Cheapest "real" terrain. |
| **v6** | **0**: its caves are random-walk tunnels, not noise | 8 | ~0.6 MB | ~7.5 MB | **Lowest memory of the real terrain mapgens.** Trees, grass and biomes are hard-coded in C++, so it does not use the Lua biome/decoration API. Needs a cubic chunk. The code warns that chunk heights divisible by 32 are buggy (`mapgen_v6.cpp:51`), so use chunksize **1 or 3**, not 2. Mudflow adds some CPU. |
| fractal | 2 (caves) | 2 | ~0.65 MB | ~15 MB | Low memory, but the **heaviest CPU**: `mgfractal_iterations` (default 11) float iterations per node. |
| v5 | 4 (ground + caves + cavern) | 3 | ~0.7 MB | ~24 MB | Moderate. |
| valleys | 4 (inter_valley_fill + caves + cavern) | 6 | ~0.7 MB | ~24 MB | Moderate. |
| carpathian | 4 (mnt_var + caves + cavern) | 13 | ~0.7 MB | ~24 MB | Heavy per-node terrain math plus many 2D noises. Slow but fits at chunksize 1. |
| v7 | 5–6 (mountain, ridge, [floatland], caves, cavern) | 7 | ~0.75 MB | **~28 MB** | Moderate to heavy. |

Add on top of that, for any mapgen using the biome API: 4 small 2D biome
noises, plus **every registered `blob` ore (one 3D noise) and `vein` ore
(two 3D noises)**. These are sized to the chunk and stay allocated
(`src/mapgen/mg_ore.cpp:365,453`). At chunksize 1 that's ~32 KB per ore. At
chunksize 5 a Minetest-Game ore set alone costs tens of MB.

Takeaways:

* **At `chunksize = 1`, every built-in mapgen fits in memory** (< 1 MB of
  mapgen buffers per emerge thread). At the default `chunksize = 5`, none of them
  fit in 8 MB PSRAM, not even singlenode.
* Noise and terrain math are `float`. The S3 has a single-precision FPU, so
  C++ mapgen runs at a sane speed. Lua mapgen callbacks are the expensive part.
* Best picks for live generation on the device: **v6** (least memory, no Lua
  decorations needed), then **flat** / **v5** / **valleys**. v7 and
  carpathian work at chunksize 1, just slower. Avoid fractal unless you drop
  `mgfractal_iterations`.
* Chunksize 1 has side effects: more mapgen calls, more chunk borders (trees
  and dungeons cut off at edges, more visible "seams"), and a bit more
  overhead per block.
* `chunksize` is a world-creation setting (`settingtypes.txt:2404`). If you
  pre-generate on a PC, **use the same chunksize (1) there** that the device
  will use. That keeps chunk alignment identical and avoids seams where the
  pre-generated area meets newly generated land.

## 4. CPU budget

* `dedicated_server_step = 0.2` (5 Hz instead of 11 Hz).
* `abm_interval = 2`, `nodetimer_interval = 1`, `liquid_loop_max = 500`,
  `active_object_send_range_blocks = 2`, `max_objects_per_block = 16`.
* `server_map_save_interval = 30`, `sqlite_synchronous = 0`. SD writes are
  slow, and a lost save is better than a stall.
* `time_speed` can stay as it is; `enable_rollback_recording = false`,
  `profiler_print_interval = 0`.
* Pin the network receive thread and the server thread to different cores.
* Hot code (`MapBlock` serialization, zstd, collision) should go in IRAM
  (`IRAM_ATTR`) or at least be kept small enough to stay in the flash cache.

**Optional big win:** When a block isn't loaded, the server reads it from disk,
deserializes it, then re-serializes and recompresses it for the network. For
blocks nobody is modifying, the stored blob can go straight to the client when
the on-disk and network serialization versions match (both use zstd since
format 29). That skips most of the RAM and CPU cost of sending blocks. It needs
a change in `ServerMap`/`RemoteClient::GetNextBlocks` and in how the
server sends blocks.

## 5. Suggested `minetest.conf` for the device

```ini
max_users = 2
enable_ipv6 = false
ipv6_server = false
dedicated_server_step = 0.2
active_block_range = 1
max_block_send_distance = 4
block_send_optimize_distance = 2
max_block_generate_distance = 2
max_simultaneous_block_sends_per_client = 4
max_packets_per_iteration = 64
active_object_send_range_blocks = 2
max_objects_per_block = 16
server_unload_unused_data_timeout = 5
server_map_save_interval = 30
sqlite_synchronous = 0
map_compression_level_disk = 1
map_compression_level_net = 1
num_emerge_threads = 1
emergequeue_limit_total = 32
emergequeue_limit_diskonly = 8
emergequeue_limit_generate = 4
chunksize = 1
# singlenode = pre-generated world only; v6 = cheapest live terrain (see 3a)
mg_name = singlenode
abm_interval = 2
nodetimer_interval = 1
liquid_loop_max = 500
enable_rollback_recording = false
remote_media = http://<your-pc-or-static-host>/media/
# after the proposed patch:
# server_mapblock_limit = 150
```

Clients also need to be told to keep their view range small. They can use
any setting they like, but the server won't send beyond `max_block_send_distance`.

## 6. Work plan (in order)

1. **Prove the reduced config on a PC first.** Run `luantiserver` on Linux
   under `ulimit -v` / cgroup `memory.max=8M`-ish with the settings above,
   a tiny game and a pre-generated world. Fix whatever is over budget there,
   where debugging is easy (valgrind massif / heaptrack). This is 80% of the work.
2. Add the `server_mapblock_limit` setting (`server.cpp:751`) and make the
   async Lua worker optional.
3. Create an ESP-IDF project: components for Luanti `src/`, bundled `lib/*`,
   zlib, zstd, sqlite. Add `ESP_PLATFORM` branches in `porting.cpp`,
   `threading/thread.cpp`, `filesys.cpp`.
4. Boot sequence: mount SD at `/sdcard`, connect Wi-Fi, set pthread defaults,
   call Luanti's `main()` with `--server --world /sdcard/luanti/worlds/w1
   --config /sdcard/luanti/minetest.conf`.
5. Measure the heap high-water mark and tick time with 1 and then 2 clients, and tune.
6. (Stretch) Pass stored map blobs straight to the network; lazy-load node metadata.

## 7. Expected result

With a tiny game, a pre-generated map and the settings above, 2 players
walking around, digging and placing within a small area should work, with
visibly slow block loading (~several blocks/s) and ~5 Hz server ticks. Running
Minetest Game, mobs, or live v7 mapgen will not fit. For comparison, a
Raspberry Pi Zero 2 W (512 MB RAM) runs a stock server easily. The ESP32-S3
is a fun "because we can" target, not a practical host.
