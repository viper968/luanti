// A FatFs disk driver for SDMMC cards that behaves like IDF's own
// (components/fatfs/diskio/diskio_sdmmc.c) but also counts bytes moved.
// IDF's driver functions are static, so they're re-implemented here on top
// of the public sdmmc_* API and swapped in for the already-mounted drive.
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ffconf.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "status_web.h"
#include "stats_internal.h"

static const char *TAG = "stats_sd";

static sdmmc_card_t *s_card;
static BYTE s_pdrv = FF_DRV_NOT_USED;

BYTE stats_sd_pdrv(void)
{
	return s_pdrv;
}

static DSTATUS cnt_initialize(BYTE pdrv)
{
	return sdmmc_get_status(s_card) == ESP_OK ? 0 : STA_NOINIT;
}

static DSTATUS cnt_status(BYTE pdrv)
{
	return 0;
}

static DRESULT cnt_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
	esp_err_t err = sdmmc_read_sectors(s_card, buff, sector, count);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "read failed (0x%x)", err);
		return RES_ERROR;
	}
	stats_add(&g_sd_rd_bytes, count * s_card->csd.sector_size);
	stats_add(&g_sd_ops, 1);
	return RES_OK;
}

static DRESULT cnt_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
	esp_err_t err = sdmmc_write_sectors(s_card, buff, sector, count);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "write failed (0x%x)", err);
		return RES_ERROR;
	}
	stats_add(&g_sd_wr_bytes, count * s_card->csd.sector_size);
	stats_add(&g_sd_ops, 1);
	return RES_OK;
}

static DRESULT cnt_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	switch (cmd) {
	case CTRL_SYNC:
		return RES_OK;
	case GET_SECTOR_COUNT:
		*((DWORD *)buff) = s_card->csd.capacity;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*((WORD *)buff) = s_card->csd.sector_size;
		return RES_OK;
#if FF_USE_TRIM
	case CTRL_TRIM: {
		if (sdmmc_can_trim(s_card) != ESP_OK)
			return RES_PARERR;
		DWORD start = ((DWORD *)buff)[0], end = ((DWORD *)buff)[1];
		sdmmc_erase_arg_t arg = sdmmc_can_discard(s_card) == ESP_OK ?
			SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG;
		return sdmmc_erase_sectors(s_card, start, end - start + 1, arg) == ESP_OK ?
			RES_OK : RES_ERROR;
	}
#endif
	}
	return RES_ERROR;
}

esp_err_t stats_attach_sd(sdmmc_card_t *card)
{
	BYTE pdrv = ff_diskio_get_pdrv_card(card);
	if (pdrv == FF_DRV_NOT_USED)
		return ESP_ERR_NOT_FOUND;
	static const ff_diskio_impl_t impl = {
		.init = cnt_initialize,
		.status = cnt_status,
		.read = cnt_read,
		.write = cnt_write,
		.ioctl = cnt_ioctl,
	};
	s_card = card;
	s_pdrv = pdrv;
	// The mounted FATFS looks the driver up on every call, so this takes
	// effect immediately. Unmounting later still works: IDF's unmount only
	// unregisters the drive number.
	ff_diskio_register(pdrv, &impl);
	return ESP_OK;
}
