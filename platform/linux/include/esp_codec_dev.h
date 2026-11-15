#pragma once

/* Desktop: roundie_board.h names the speaker's codec handle type. The desktop
 * has no codec yet (audio is step M6), so the type stays opaque. */

typedef struct esp_codec_dev *esp_codec_dev_handle_t;
