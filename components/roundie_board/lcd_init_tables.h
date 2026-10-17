#pragma once

#include <stddef.h>
#include "esp_lcd_st77916.h"

const st77916_lcd_init_cmd_t *roundie_lcd_table(unsigned variant, size_t *count);
