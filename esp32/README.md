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

### Flashing safely

* The partition table (`board_test/partitions.csv`) is byte-identical to what
  these boards ship with (two 7.94 MB app slots, no FATFS). `idf.py flash`
  therefore only rewrites the bootloader, the same partition table, otadata and
  the app, all of which can be rewritten again at any time.
* The ESP32-S3's first-stage bootloader is in mask ROM. If a flashed app won't
  boot, hold **BOOT**, tap **RESET**, release **BOOT**, and flash again.
* The only irreversible operations are eFuse writes. **Never run `espefuse`
  burn commands, and don't enable secure boot or flash encryption** in menuconfig.
* The board uses the S3's native USB Serial/JTAG, so the baud rate doesn't affect
  speed. Writes are fast (compressed), but `read_flash` with esptool 4.6 manages
  only ~9 KB/s, so back up just the used region rather than all 16 MB.

Back up the used region before the first flash (about 4.5 minutes), then restore
it later if needed:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 read_flash 0x0 0x240000 backup.bin
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 backup.bin
```

Waveshare's demo zip also has a full factory image,
`Firmware/ESP32-S3-Touch-LCD-2.8B.bin`, written at `0x0`. Note that it uses a
different partition layout (3 MB apps + a 9.9 MB FATFS).

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
