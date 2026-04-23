#include "modbus_rtu.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct modbus_rtu_context {
    modbus_rtu_config_t cfg;
    SemaphoreHandle_t lock;
    bool owns_uart_driver;
};

static const char *TAG = "modbus_rtu";

static const char *command_word_to_name(uint16_t cmd)
{
    switch (cmd) {
    case 0x0004:
        return "Authorize";
    case 0x0006:
        return "Start Batch";
    case 0x0007:
        return "Stop Batch";
    case 0x0009:
        return "End Transaction";
    case 0x000A:
        return "Clear Status";
    case 0x000B:
        return "Clear Alarms";
    default:
        return "Unknown";
    }
}

/* Keep direction switching tight to avoid dropping the first bytes of fast slave responses. */
#define MODBUS_RTU_TX_SETTLE_US      150
#define MODBUS_RTU_RX_SETTLE_US      150
#define MODBUS_RTU_TURNAROUND_US     200

static void log_frame_line(modbus_rtu_handle_t handle, const char *prefix, const uint8_t *data, size_t len)
{
    if (!handle->cfg.debug || !data || len == 0 || len > 260) {
        return;
    }

    char line[(260 * 3) + 1] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        int n = snprintf(&line[pos], sizeof(line) - pos, "%02X%s", data[i], (i + 1 < len) ? " " : "");
        if (n <= 0 || (size_t)n >= (sizeof(line) - pos)) {
            break;
        }
        pos += (size_t)n;
    }
    ESP_LOGI(TAG, "%s%s", prefix, line);
}

static void log_frame_summary(modbus_rtu_handle_t handle, const char *prefix, const uint8_t *frame, size_t len)
{
    if (!handle->cfg.debug || !frame || len < 2) {
        return;
    }

    uint8_t slave = frame[0];
    uint8_t fc = frame[1];

    if (fc == 0x03) {
        // Request (no CRC): [slave][0x03][addr hi][addr lo][count hi][count lo]
        if (len == 6) {
            uint16_t addr = ((uint16_t)frame[2] << 8) | frame[3];
            uint16_t count = ((uint16_t)frame[4] << 8) | frame[5];
            ESP_LOGI(TAG, "%sFC03 slave=0x%02X start=0x%04X count=%u", prefix, slave, addr, count);
            return;
        }

        // Response (no CRC): [slave][0x03][byte_count][data...]
        if (len >= 3) {
            uint8_t byte_count = frame[2];
            if ((size_t)(3 + byte_count) == len && (byte_count % 2U) == 0U) {
                ESP_LOGI(TAG,
                         "%sFC03 slave=0x%02X regs=%u bytes=%u",
                         prefix,
                         slave,
                         (unsigned)(byte_count / 2U),
                         (unsigned)byte_count);
                return;
            }
        }
    }

    if (fc == 0x10) {
        // Response (no CRC): [slave][0x10][addr hi][addr lo][count hi][count lo]
        if (len == 6) {
            uint16_t addr = ((uint16_t)frame[2] << 8) | frame[3];
            uint16_t count = ((uint16_t)frame[4] << 8) | frame[5];
            ESP_LOGI(TAG, "%sFC10 ACK slave=0x%02X addr=0x%04X count=%u", prefix, slave, addr, count);
            return;
        }

        // Request (no CRC): [slave][0x10][addr hi][addr lo][count hi][count lo][byte_count][data...]
        if (len >= 7) {
            uint16_t addr = ((uint16_t)frame[2] << 8) | frame[3];
            uint16_t count = ((uint16_t)frame[4] << 8) | frame[5];
            uint8_t byte_count = frame[6];
            ESP_LOGI(TAG,
                     "%sFC10 slave=0x%02X start=0x%04X count=%u bytes=%u",
                     prefix,
                     slave,
                     addr,
                     count,
                     (unsigned)byte_count);
            if (addr == 0x0000 && count >= 1 && len >= 9) {
                uint16_t cmd = ((uint16_t)frame[7] << 8) | frame[8];
                ESP_LOGI(TAG, "%sCMD=0x%04X (%s)", prefix, cmd, command_word_to_name(cmd));
            }
            return;
        }
    }

    if ((fc & 0x80U) != 0 && len >= 3) {
        ESP_LOGW(TAG, "%sEXCEPTION slave=0x%02X fc=0x%02X code=0x%02X", prefix, slave, fc & 0x7FU, frame[2]);
        return;
    }

    ESP_LOGI(TAG, "%sFC=0x%02X slave=0x%02X len=%u", prefix, fc, slave, (unsigned)len);
}

static uint16_t crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return crc;
}

static void set_tx_mode(modbus_rtu_handle_t handle, bool enable)
{
    if (handle->cfg.de_pin < 0 && handle->cfg.re_pin < 0) {
        return;
    }

#if RS485_EN_ALWAYS_HIGH
    if (handle->cfg.de_pin >= 0) {
        gpio_set_level(handle->cfg.de_pin, 1);
    }
    if (handle->cfg.re_pin >= 0) {
        gpio_set_level(handle->cfg.re_pin, 1);
    }
    esp_rom_delay_us(MODBUS_RTU_TX_SETTLE_US);
    return;
#endif

    if (handle->cfg.de_pin >= 0) {
        gpio_set_level(handle->cfg.de_pin, enable ? 1 : 0);
    }
    if (handle->cfg.re_pin >= 0) {
        gpio_set_level(handle->cfg.re_pin, enable ? 1 : 0);
    }
    esp_rom_delay_us(enable ? MODBUS_RTU_TX_SETTLE_US : MODBUS_RTU_RX_SETTLE_US);
}

static esp_err_t send_frame(modbus_rtu_handle_t handle, const uint8_t *data, size_t len)
{
    uint16_t crc = crc16(data, len);
    uint8_t trailer[2] = { (uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF) };
    uint8_t full_frame[260] = {0};
    size_t frame_len = len + sizeof(trailer);

    if (frame_len <= sizeof(full_frame)) {
        memcpy(full_frame, data, len);
        memcpy(full_frame + len, trailer, sizeof(trailer));
        log_frame_line(handle, "[TX] ", full_frame, frame_len);
        log_frame_summary(handle, "TX_SUMMARY: ", data, len);
    }

    set_tx_mode(handle, true);
    ESP_RETURN_ON_ERROR(uart_flush_input(handle->cfg.uart_num), TAG, "uart flush failed");
    ESP_RETURN_ON_ERROR(uart_write_bytes(handle->cfg.uart_num, data, len) < 0 ? ESP_FAIL : ESP_OK, TAG, "uart write failed");
    ESP_RETURN_ON_ERROR(uart_write_bytes(handle->cfg.uart_num, trailer, sizeof(trailer)) < 0 ? ESP_FAIL : ESP_OK, TAG, "uart crc write failed");
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(handle->cfg.uart_num, pdMS_TO_TICKS(1000)), TAG, "uart tx timeout");
    esp_rom_delay_us(MODBUS_RTU_TURNAROUND_US);
    set_tx_mode(handle, false);

    return ESP_OK;
}

static esp_err_t receive_frame(modbus_rtu_handle_t handle, uint8_t *buffer, size_t max_len, size_t *out_len)
{
    int read = 0;
    int total = 0;
    TickType_t timeout = pdMS_TO_TICKS(handle->cfg.response_timeout_ms);

    while (total < 3) {
        read = uart_read_bytes(handle->cfg.uart_num, buffer + total, 3 - total, timeout);
        if (read <= 0) {
            if (handle->cfg.debug) {
                size_t buffered = 0;
                (void)uart_get_buffered_data_len(handle->cfg.uart_num, &buffered);
                ESP_LOGW(TAG,
                         "RX timeout waiting header (read=%d total=%d buffered=%u). "
                         "Likely wiring/baud/parity/DE-RE issue.",
                         read,
                         total,
                         (unsigned)buffered);
            }
            return ESP_ERR_TIMEOUT;
        }
        total += read;
    }

    size_t expected = 5;
    if (buffer[1] == 0x03) {
        expected = (size_t)(3 + buffer[2] + 2);
    } else if (buffer[1] == 0x10) {
        expected = 8;
    } else if ((buffer[1] & 0x80U) != 0) {
        expected = 5;
    }

    if (expected > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    while ((size_t)total < expected) {
        read = uart_read_bytes(handle->cfg.uart_num,
                               buffer + total,
                               expected - (size_t)total,
                               timeout);
        if (read <= 0) {
            if (handle->cfg.debug) {
                size_t buffered = 0;
                (void)uart_get_buffered_data_len(handle->cfg.uart_num, &buffered);
                ESP_LOGW(TAG,
                         "RX timeout mid-frame (read=%d total=%d expected=%u buffered=%u)",
                         read,
                         total,
                         (unsigned)expected,
                         (unsigned)buffered);
                ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, (size_t)total, ESP_LOG_INFO);
            }
            return ESP_ERR_TIMEOUT;
        }
        total += read;
    }

    uint16_t rx_crc = (uint16_t)buffer[expected - 2] | ((uint16_t)buffer[expected - 1] << 8);
    uint16_t calc_crc = crc16(buffer, expected - 2);
    if (rx_crc != calc_crc) {
        if (handle->cfg.debug) {
            log_frame_line(handle, "[RX_BAD_CRC] ", buffer, expected);
        }
        return ESP_ERR_INVALID_CRC;
    }

    *out_len = expected;
    if (handle->cfg.debug) {
        log_frame_line(handle, "[RX] ", buffer, expected);
        log_frame_summary(handle, "RX_SUMMARY: ", buffer, expected - 2);
    }
    return ESP_OK;
}

esp_err_t modbus_rtu_init(const modbus_rtu_config_t *cfg, modbus_rtu_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    esp_err_t ret = ESP_OK;
    bool uart_installed_here = false;

    ESP_LOGI(TAG,
             "Init: UART%d TX=%d RX=%d DE=%d RE=%d baud=%lu timeout_ms=%lu",
             cfg->uart_num,
             cfg->tx_pin,
             cfg->rx_pin,
             cfg->de_pin,
             cfg->re_pin,
             (unsigned long)cfg->baudrate,
             (unsigned long)cfg->response_timeout_ms);

    modbus_rtu_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    handle->cfg = *cfg;
    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    handle->owns_uart_driver = false;

    uart_config_t uart_cfg = {
        .baud_rate = (int)cfg->baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* UART ownership is the board layer. If a driver is already installed (e.g., by board_init_rs485),
     * reuse it and avoid re-install/re-config that can conflict. */
    bool already_installed = uart_is_driver_installed(cfg->uart_num);
    if (!already_installed) {
        ret = uart_driver_install(cfg->uart_num, 1024, 0, 0, NULL, 0);
        if (ret == ESP_OK) {
            uart_installed_here = true;
            handle->owns_uart_driver = true;
        }
        ESP_GOTO_ON_ERROR(ret, fail, TAG, "uart_driver_install failed");

        ESP_GOTO_ON_ERROR(uart_param_config(cfg->uart_num, &uart_cfg), fail, TAG, "uart_param_config failed");
        ESP_GOTO_ON_ERROR(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin,
                                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                          fail, TAG, "uart_set_pin failed");
    } else {
        ESP_LOGI(TAG, "UART%d driver already installed; Modbus RTU will reuse it", cfg->uart_num);
    }

    /* Even when we reuse a pre-installed driver (board layer), enforce the expected pins and
     * serial framing here, otherwise Modbus can silently inherit mismatched config. */
    ESP_GOTO_ON_ERROR(uart_param_config(cfg->uart_num, &uart_cfg), fail, TAG, "uart_param_config failed (reuse)");
    ESP_GOTO_ON_ERROR(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                      fail, TAG, "uart_set_pin failed (reuse)");
    (void)uart_flush_input(cfg->uart_num);

    if (cfg->de_pin < 0 && cfg->re_pin < 0) {
        ESP_LOGW(TAG,
                 "RS485 DE/RE pins not configured. If your transceiver requires direction control, "
                 "Modbus will timeout. (TX=%d RX=%d UART%d)",
                 cfg->tx_pin,
                 cfg->rx_pin,
                 cfg->uart_num);
    }

    if (cfg->de_pin >= 0) {
        gpio_set_direction(cfg->de_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg->de_pin, 0);
    }
    if (cfg->re_pin >= 0) {
        gpio_set_direction(cfg->re_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg->re_pin, 0);
    }

    *out_handle = handle;
    return ESP_OK;

fail:
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    if (uart_installed_here) {
        uart_driver_delete(cfg->uart_num);
    }
    free(handle);
    return ret;
}

void modbus_rtu_deinit(modbus_rtu_handle_t handle)
{
    if (!handle) {
        return;
    }
    if (handle->owns_uart_driver) {
        uart_driver_delete(handle->cfg.uart_num);
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}

void modbus_rtu_set_debug(modbus_rtu_handle_t handle, bool enable)
{
    if (handle) {
        handle->cfg.debug = enable;
    }
}

void modbus_rtu_set_timeout(modbus_rtu_handle_t handle, uint32_t timeout_ms)
{
    if (handle) {
        handle->cfg.response_timeout_ms = timeout_ms;
    }
}

esp_err_t modbus_rtu_read_holding_registers(modbus_rtu_handle_t handle,
                                            uint8_t slave_addr,
                                            uint16_t start_addr,
                                            uint16_t count,
                                            uint16_t *out_regs)
{
    ESP_RETURN_ON_FALSE(handle && out_regs && count > 0 && count <= 125, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    uint8_t req[6] = {
        slave_addr,
        0x03,
        (uint8_t)((start_addr >> 8) & 0xFF),
        (uint8_t)(start_addr & 0xFF),
        (uint8_t)((count >> 8) & 0xFF),
        (uint8_t)(count & 0xFF),
    };

    xSemaphoreTake(handle->lock, portMAX_DELAY);

    esp_err_t err = send_frame(handle, req, sizeof(req));
    if (err != ESP_OK) {
        xSemaphoreGive(handle->lock);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t response[256] = {0};
    size_t len = 0;
    err = receive_frame(handle, response, sizeof(response), &len);
    xSemaphoreGive(handle->lock);
    if (err != ESP_OK) {
        return err;
    }

    if ((response[1] & 0x80U) != 0) {
        if (handle->cfg.debug && len >= 3) {
            ESP_LOGW(TAG, "FC03 exception: code=0x%02X", response[2]);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (len < 5 || response[0] != slave_addr || response[1] != 0x03 || response[2] != count * 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint16_t i = 0; i < count; i++) {
        out_regs[i] = ((uint16_t)response[3 + (i * 2)] << 8) | (uint16_t)response[4 + (i * 2)];
    }

    if (handle->cfg.debug) {
        ESP_LOGI(TAG,
                 "FC03 OK slave=0x%02X start=0x%04X count=%u",
                 slave_addr,
                 start_addr,
                 count);
    }

    return ESP_OK;
}

esp_err_t modbus_rtu_write_multiple_registers(modbus_rtu_handle_t handle,
                                              uint8_t slave_addr,
                                              uint16_t start_addr,
                                              uint16_t count,
                                              const uint16_t *regs)
{
    ESP_RETURN_ON_FALSE(handle && regs && count > 0 && count <= 123, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    uint8_t request[7 + (123 * 2)] = {0};
    request[0] = slave_addr;
    request[1] = 0x10;
    request[2] = (uint8_t)((start_addr >> 8) & 0xFF);
    request[3] = (uint8_t)(start_addr & 0xFF);
    request[4] = (uint8_t)((count >> 8) & 0xFF);
    request[5] = (uint8_t)(count & 0xFF);
    request[6] = (uint8_t)(count * 2);

    for (uint16_t i = 0; i < count; i++) {
        request[7 + (i * 2)] = (uint8_t)((regs[i] >> 8) & 0xFF);
        request[8 + (i * 2)] = (uint8_t)(regs[i] & 0xFF);
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);

    esp_err_t err = send_frame(handle, request, 7 + (count * 2));
    if (err != ESP_OK) {
        xSemaphoreGive(handle->lock);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t response[16] = {0};
    size_t len = 0;
    err = receive_frame(handle, response, sizeof(response), &len);
    xSemaphoreGive(handle->lock);
    if (err != ESP_OK) {
        return err;
    }

    if ((response[1] & 0x80U) != 0) {
        if (handle->cfg.debug && len >= 3) {
            ESP_LOGW(TAG, "FC10 exception: code=0x%02X", response[2]);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (len < 8 || response[0] != slave_addr || response[1] != 0x10) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t echo_addr = ((uint16_t)response[2] << 8) | response[3];
    uint16_t echo_count = ((uint16_t)response[4] << 8) | response[5];
    if (echo_addr != start_addr || echo_count != count) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (handle->cfg.debug) {
        ESP_LOGI(TAG,
                 "FC10 ACK slave=0x%02X addr=0x%04X count=%u",
                 slave_addr,
                 echo_addr,
                 echo_count);
    }
    return ESP_OK;
}
