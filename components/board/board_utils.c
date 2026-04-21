#include "board_utils.h"

#include "board_gpios.h"

#include "driver/i2c.h"
#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "board_utils";

uint64_t board_read_chip_id(void)
{
    uint8_t custom_mac[6] = {0};
    uint64_t custom_mac64 = 0;
    esp_efuse_mac_get_custom(custom_mac);
    for (int i = 0; i < 6; i++) {
        custom_mac64 |= ((uint64_t)custom_mac[i]) << (i * 8);
    }

    uint8_t efuse_val = 0;
    esp_efuse_read_block(EFUSE_BLK_KEY0, &efuse_val, 0, 8);
    bool new_schema = (efuse_val & 0x01) != 0;

    uint64_t chip_id = 0;

    if (!new_schema) {
        uint64_t mac_to_use = 0;
        if (custom_mac64 != 0) {
            mac_to_use = custom_mac64;
        } else {
            esp_efuse_mac_get_default((uint8_t *)&mac_to_use);
        }

        for (int i = 0; i < 17; i += 8) {
            chip_id |= ((mac_to_use >> (40 - i)) & 0xff) << i;
        }
    } else {
        if (custom_mac64 != 0) {
            chip_id = custom_mac64;
        } else {
            uint8_t mac[6] = {0};
            esp_efuse_mac_get_default(mac);
            for (int i = 0; i < 6; i++) {
                chip_id |= ((uint64_t)mac[i]) << ((5 - i) * 8);
            }
        }
    }

    return chip_id;
}
const char *board_get_board_code(void)
{
#if defined(HARDWARE_NEW_PCB)
    return "n";
#elif defined(HARDWARE_ORPACK)
    return "o";
#elif defined(HARDWARE_RELCON)
    return "r";
#elif defined(HARDWARE_EVIDEN)
    return "e";
#elif defined(HARDWARE_ESP32DEV) || defined(LILYGO_ESP32_CLASSIC)
    return "l";
#else
    return "u";
#endif
}
bool board_rtc_health(esp_err_t *err_out)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RTC_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    if (err_out) {
        *err_out = err;
    }

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "RTC 0x%02X responded OK", RTC_ADDR);
    } else {
        ESP_LOGW(TAG, "RTC 0x%02X not found: %s", RTC_ADDR, esp_err_to_name(err));
    }

    return err == ESP_OK;
}
