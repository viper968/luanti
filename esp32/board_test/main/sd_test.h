#pragma once
#include <stdbool.h>

#include "sdmmc_cmd.h"

bool sd_mount(void);
sdmmc_card_t *sd_card(void);
void sd_benchmark(int test_mb);
