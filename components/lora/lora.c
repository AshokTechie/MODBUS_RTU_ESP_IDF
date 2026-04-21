#include "lora.h"

#include "board_gpios.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "smartload_lora";

#define LORA_BAUD_RATE      9600
#define LORA_CMD_TIMEOUT_MS 2000
#define LORA_RETRIES        3

static bool s_lora_ok = false;
static bool s_uart_ready = false;

#if HAS_LORA_M0M1

static void lora_enter_config_mode(void)
{
    gpio_set_level(LORA_M0, 1);
    gpio_set_level(LORA_M1, 1);
    /* E220 modules can take a moment to enter AT/config mode without AUX wired. */
    vTaskDelay(pdMS_TO_TICKS(400));
}

static void lora_enter_normal_mode(void)
{
    gpio_set_level(LORA_M0, 0);
    gpio_set_level(LORA_M1, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static bool lora_send_at_once(const char *cmd, const char *expected, char *out_resp, size_t out_resp_size)
{
    uint8_t tmp = 0;
    while (uart_read_bytes(LORA_UART_NUM, &tmp, 1, 0) == 1) {
    }

    if (out_resp && out_resp_size > 0) {
        out_resp[0] = '\0';
    }

    for (int attempt = 1; attempt <= LORA_RETRIES; attempt++) {
        int written = uart_write_bytes(LORA_UART_NUM, cmd, strlen(cmd));
        ESP_LOGD(TAG, "AT write attempt %d/%d (written=%d): '%s'", attempt, LORA_RETRIES, written, cmd);

        char resp[128] = {0};
        int pos = 0;
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LORA_CMD_TIMEOUT_MS);

        while (xTaskGetTickCount() < deadline && pos < (int)sizeof(resp) - 1) {
            uint8_t c = 0;
            if (uart_read_bytes(LORA_UART_NUM, &c, 1, pdMS_TO_TICKS(50)) == 1) {
                ESP_LOGD(TAG, "AT rx byte: 0x%02X '%c'", c, (c >= 32 && c <= 126) ? (char)c : '.');
                resp[pos++] = (char)c;
                vTaskDelay(pdMS_TO_TICKS(2));
                while (uart_read_bytes(LORA_UART_NUM, &c, 1, 0) == 1 && pos < (int)sizeof(resp) - 1) {
                    ESP_LOGD(TAG, "AT rx byte: 0x%02X '%c'", c, (c >= 32 && c <= 126) ? (char)c : '.');
                    resp[pos++] = (char)c;
                }

                if (strstr(resp, expected) || strstr(resp, "OK")) {
                    ESP_LOGI(TAG, "AT OK: '%s' -> '%s'", cmd, resp);
                    if (out_resp && out_resp_size > 0) {
                        snprintf(out_resp, out_resp_size, "%s", resp);
                    }
                    return true;
                }
                if (strstr(resp, "ERROR")) {
                    ESP_LOGW(TAG, "AT ERROR on attempt %d: '%s'", attempt, cmd);
                    break;
                }
            }
        }

        if (attempt < LORA_RETRIES) {
            ESP_LOGW(TAG, "AT timeout attempt %d/%d: '%s' (rx_len=%d)", attempt, LORA_RETRIES, cmd, pos);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGW(TAG, "AT failed: '%s'", cmd);
    if (out_resp && out_resp_size > 0) {
        snprintf(out_resp, out_resp_size, "%s", "<no/invalid response>");
    }
    return false;
}

#endif

esp_err_t lora_init(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = LORA_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(LORA_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(LORA_UART_NUM,
                       LORA_TX_PIN,
                       LORA_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(LORA_UART_NUM, 512, 0, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "LoRa UART already initialized");
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_uart_ready = true;

#if HAS_LORA_M0M1
    const gpio_config_t gcfg = {
        .pin_bit_mask = (1ULL << LORA_M0) | (1ULL << LORA_M1),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&gcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LoRa GPIO setup failed: %s", esp_err_to_name(err));
        return err;
    }

    lora_enter_config_mode();
    char at_resp[128] = {0};
    s_lora_ok = lora_send_at_once("AT\r\n", "OK", at_resp, sizeof(at_resp));
    if (!s_lora_ok) {
        /* Some E220 firmwares are picky about line endings. Try a bare "AT". */
        s_lora_ok = lora_send_at_once("AT", "OK", at_resp, sizeof(at_resp));
    }
    if (!s_lora_ok) {
        ESP_LOGW(TAG,
                 "LoRa no-response (UART%d TX=%d RX=%d M0=%d M1=%d baud=%d). "
                 "Check: power level (3.3V), GND common, TX/RX crossed, and M0/M1 wiring. Last='%s'",
                 LORA_UART_NUM, LORA_TX_PIN, LORA_RX_PIN, LORA_M0, LORA_M1, LORA_BAUD_RATE, at_resp);
    }
    lora_enter_normal_mode();
#else
    s_lora_ok = true;
#endif

    ESP_LOGI(TAG, "LoRa init: %s (UART%d TX=%d RX=%d)",
             s_lora_ok ? "OK" : "FAILED",
             LORA_UART_NUM,
             LORA_TX_PIN,
             LORA_RX_PIN);

    return s_lora_ok ? ESP_OK : ESP_FAIL;
}

bool lora_is_ok(void)
{
    return s_lora_ok;
}

esp_err_t lora_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_uart_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    int written = uart_write_bytes(LORA_UART_NUM, data, len);
    return (written == (int)len) ? ESP_OK : ESP_FAIL;
}
