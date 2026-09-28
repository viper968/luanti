# ESP32-S3 port (experimental)

Work toward running `luantiserver` on the Waveshare ESP32-S3-Touch-LCD-2.8B
(ESP32-S3R8: 8 MB PSRAM, 16 MB flash, TF card). See
[`doc/esp32s3_server_port.md`](../doc/esp32s3_server_port.md) for the
feasibility study and the plan.

## `board_test/`: hardware bring-up

A small ESP-IDF app that checks the board and measures what the port depends on:

* chip / flash / PSRAM size and free heap (internal vs PSRAM)
* memcpy bandwidth for internal RAM vs PSRAM
* float vs double speed (Lua numbers are doubles; the S3 FPU is single-precision)
* I²C scan, TCA9554 expander setup for headless use (LCD held in reset, backlight off)
* PCF85063 RTC time and battery voltage
* TF card mount in 1-bit SDMMC mode, plus sequential and random 4 KiB read/write speed
* Wi-Fi connect (power save off) and a UDP echo server on port 30000
* a live status web page (see below)

### Build and flash (ESP-IDF v5.5.x)

```bash
cd esp32/board_test
idf.py set-target esp32s3
idf.py menuconfig        # "Luanti board test" -> Wi-Fi SSID/password (optional)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # Windows: -p COMx
```

Put a **FAT32**-formatted TF card in the slot first. The test writes and then
deletes a 4 MB `lbt_test.bin`. If flashing fails, hold **BOOT**, tap
**RESET**, release **BOOT**, and try again.

### Wi-Fi latency test

Once the monitor shows `UDP: echoing on port 30000`, run from a PC on the same
network:

```bash
python3 esp32/tools/udp_ping.py <board-ip>
```

### Status web page

With Wi-Fi configured, the monitor prints `Status page: http://<board-ip>/`.
Open that address in a browser on the same network to see live:

* CPU load per core (FreeRTOS idle-time accounting)
* internal RAM and PSRAM in use, plus lowest-ever free and largest free block
  (fragmentation)
* TF card space used, read/write throughput and totals
* Wi-Fi down/up throughput, totals, packet counts and signal strength
* 2-minute history graphs, kept on the board so a page reload doesn't lose them

The page is the reusable `components/status_web` ESP-IDF component, which the
real server firmware will use too. The TF card is counted by a FatFs disk driver
that wraps IDF's SDMMC one, and Wi-Fi by hooking the lwIP interface's
input/output functions. Both count every byte, not just the game's. JSON is at
`/api/stats` and `/api/history`. There's no login, so only use it on a
network you trust.
