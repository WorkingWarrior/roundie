/* Non-deployed descriptor experiment using the pinned TinyUSB macros.
 * No device descriptor, VID/PID, callbacks, PHY initialization or app_main.
 */
#include "common/tusb_common.h"
#include "class/cdc/cdc.h"
#include "class/mtp/mtp.h"
#include "device/usbd.h"

enum {
    PROBE_CDC_CONTROL = 0,
    PROBE_CDC_DATA = 1,
    PROBE_MTP = 2,
    PROBE_INTERFACE_COUNT = 3,
    PROBE_DESCRIPTOR_SIZE = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MTP_DESC_LEN,
};

/* CDC: interrupt IN 81, bulk OUT 02 / IN 82.
 * MTP: bulk OUT 03 / IN 83, interrupt IN 84.
 * EP0 plus four IN endpoints reaches the S3 IN limit exactly.
 * Bit 6 records the battery/self-powered design; this does not solve VBUS sense.
 */
const uint8_t roundie_probe_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, PROBE_INTERFACE_COUNT, 0, PROBE_DESCRIPTOR_SIZE,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_CDC_DESCRIPTOR(PROBE_CDC_CONTROL, 1, 0x81, 8, 0x02, 0x82, 64),
    TUD_MTP_DESCRIPTOR(PROBE_MTP, 2, 0x84, 64, 16, 0x03, 0x83, 64),
};

_Static_assert(PROBE_CDC_DATA == PROBE_CDC_CONTROL + 1, "CDC IAD layout");
_Static_assert(sizeof(roundie_probe_configuration) == PROBE_DESCRIPTOR_SIZE,
               "configuration descriptor length");
