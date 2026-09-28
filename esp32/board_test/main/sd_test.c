// TF card mount + throughput test, sized around what luantiserver does:
// sequential media reads, and small random reads/writes like SQLite pages.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "board.h"
#include "sd_test.h"

#define MOUNT "/sdcard"
#define TEST_FILE MOUNT "/lbt_test.bin"
#define CHUNK (32 * 1024)
#define PAGE 4096
#define RANDOM_OPS 256

static sdmmc_card_t *s_card;

bool sd_mount(void)
{
	// D3 must be high when the card sees CMD0, or it enters SPI mode
	board_expander_set(EXIO_SD_D3, true);

	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; // 40 MHz, falls back if the card can't

	sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
	slot.width = 1;
	slot.clk = BOARD_SD_CLK;
	slot.cmd = BOARD_SD_CMD;
	slot.d0 = BOARD_SD_D0;
	slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

	esp_vfs_fat_sdmmc_mount_config_t mcfg = {
		.format_if_mount_failed = false,
		.max_files = 8,
		.allocation_unit_size = 16 * 1024,
	};
	esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mcfg, &s_card);
	if (err != ESP_OK) {
		// 40 MHz can fail on long traces or old cards; retry at 20 MHz
		host.max_freq_khz = SDMMC_FREQ_DEFAULT;
		err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mcfg, &s_card);
	}
	if (err != ESP_OK) {
		printf("SD: mount failed (%s). Is a FAT32-formatted card inserted?\n",
			esp_err_to_name(err));
		return false;
	}
	sdmmc_card_print_info(stdout, s_card);
	return true;
}

sdmmc_card_t *sd_card(void)
{
	return s_card;
}

static double mb_per_s(size_t bytes, int64_t us)
{
	return us > 0 ? (bytes / (1024.0 * 1024.0)) / (us / 1e6) : 0;
}

void sd_benchmark(int test_mb)
{
	// DMA-capable internal buffer; PSRAM buffers would go through a bounce copy
	uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	if (!buf) {
		printf("SD: no buffer\n");
		return;
	}
	for (int i = 0; i < CHUNK; i++)
		buf[i] = (uint8_t)(i * 31);
	const size_t total = (size_t)test_mb * 1024 * 1024;

	// POSIX I/O, like SQLite: no stdio buffering layer in between
	// Sequential write (map saves, world backups)
	int fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("SD: cannot create %s\n", TEST_FILE);
		free(buf);
		return;
	}
	int64_t t0 = esp_timer_get_time();
	for (size_t done = 0; done < total; done += CHUNK)
		write(fd, buf, CHUNK);
	fsync(fd);
	int64_t t1 = esp_timer_get_time();
	close(fd);
	printf("SD sequential write: %.2f MB/s (%d MB)\n", mb_per_s(total, t1 - t0), test_mb);

	// Sequential read (sending media to clients)
	fd = open(TEST_FILE, O_RDONLY);
	if (fd < 0) {
		printf("SD: cannot reopen %s\n", TEST_FILE);
		free(buf);
		return;
	}
	t0 = esp_timer_get_time();
	size_t got = 0;
	ssize_t n;
	while ((n = read(fd, buf, CHUNK)) > 0)
		got += n;
	t1 = esp_timer_get_time();
	printf("SD sequential read:  %.2f MB/s\n", mb_per_s(got, t1 - t0));

	// Random 4 KiB reads (SQLite page lookups when loading map blocks)
	const long pages = total / PAGE;
	t0 = esp_timer_get_time();
	for (int i = 0; i < RANDOM_OPS; i++)
		pread(fd, buf, PAGE, (off_t)(esp_random() % pages) * PAGE);
	t1 = esp_timer_get_time();
	close(fd);
	printf("SD random 4K read:   %.0f IOPS (%.2f ms each)\n",
		RANDOM_OPS / ((t1 - t0) / 1e6), (t1 - t0) / 1000.0 / RANDOM_OPS);

	// Random 4 KiB writes + fsync (SQLite commits during map saves)
	fd = open(TEST_FILE, O_RDWR);
	if (fd < 0) {
		printf("SD: cannot reopen %s\n", TEST_FILE);
		free(buf);
		return;
	}
	const int wops = RANDOM_OPS / 4;
	t0 = esp_timer_get_time();
	for (int i = 0; i < wops; i++) {
		pwrite(fd, buf, PAGE, (off_t)(esp_random() % pages) * PAGE);
		fsync(fd);
	}
	t1 = esp_timer_get_time();
	close(fd);
	printf("SD random 4K write+fsync: %.0f IOPS (%.2f ms each)\n",
		wops / ((t1 - t0) / 1e6), (t1 - t0) / 1000.0 / wops);

	unlink(TEST_FILE);
	free(buf);

	FATFS *fs;
	DWORD free_clusters;
	if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
		uint64_t free_b = (uint64_t)free_clusters * fs->csize * 512;
		uint64_t total_b = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
		printf("SD free: %llu MB of %llu MB, cluster size %u KB\n",
			free_b >> 20, total_b >> 20, (unsigned)(fs->csize / 2));
	}
}
