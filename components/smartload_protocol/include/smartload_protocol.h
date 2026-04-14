#pragma once

#include "esp_err.h"
#include "modbus_rtu.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SMARTLOAD_CMD_REQUEST_ID      = 0x01,
    SMARTLOAD_CMD_DISPLAY         = 0x02,
    SMARTLOAD_CMD_CONFIG_RECIPE   = 0x03,
    SMARTLOAD_CMD_AUTHORIZE       = 0x04,
    SMARTLOAD_CMD_AUTH_BATCH      = 0x05,
    SMARTLOAD_CMD_START_BATCH     = 0x06,
    SMARTLOAD_CMD_STOP_BATCH      = 0x07,
    SMARTLOAD_CMD_TIMEOUT         = 0x08,
    SMARTLOAD_CMD_END_TRANSACTION = 0x09,
    SMARTLOAD_CMD_CLEAR_STATUS    = 0x0A,
    SMARTLOAD_CMD_CLEAR_ALARMS    = 0x0B,
} smartload_command_t;

#define SMARTLOAD_STATUS_READY         0x0001
#define SMARTLOAD_STATUS_AUTHORIZED    0x0002
#define SMARTLOAD_STATUS_DELIVERING    0x0004
#define SMARTLOAD_STATUS_BATCH_ACTIVE  0x0008
#define SMARTLOAD_STATUS_ALARM_PRI     0x0100
#define SMARTLOAD_STATUS_ALARM_SEC     0x0200

typedef struct {
    uint16_t status_word;
    uint32_t delivery_volume_x100;
    uint32_t delivery_amount_x100;
    uint32_t unit_price_x10000;
    uint16_t transaction_num;
    int16_t temperature_x100;
    uint32_t preset_volume_x100;
    uint32_t total_volume_x100;
    uint16_t flow_rate_x100;
    uint16_t hose_number;
    uint16_t product_code;
    uint16_t alarm_register;
} smartload_status_t;

typedef struct {
    modbus_rtu_handle_t modbus;
    uint8_t slave_addr;
} smartload_protocol_t;

esp_err_t smartload_protocol_init(smartload_protocol_t *ctx, modbus_rtu_handle_t modbus, uint8_t slave_addr);
esp_err_t smartload_read_status(smartload_protocol_t *ctx, smartload_status_t *out_status);
esp_err_t smartload_read_status_word(smartload_protocol_t *ctx, uint16_t *out_status);
esp_err_t smartload_read_device_info(smartload_protocol_t *ctx, uint16_t *out_device_id, uint16_t *out_firmware_version);
esp_err_t smartload_send_command(smartload_protocol_t *ctx, smartload_command_t cmd);
esp_err_t smartload_authorize(smartload_protocol_t *ctx, uint16_t recipe, uint16_t additive_config, uint16_t side_nozzle);
esp_err_t smartload_write_preset_volume(smartload_protocol_t *ctx, uint32_t preset_volume_x100);
esp_err_t smartload_write_components(smartload_protocol_t *ctx, uint16_t components);
esp_err_t smartload_auth_batch(smartload_protocol_t *ctx, uint32_t preset_volume_x100, uint16_t components);
esp_err_t smartload_start_batch(smartload_protocol_t *ctx);
esp_err_t smartload_stop_batch(smartload_protocol_t *ctx);
esp_err_t smartload_end_transaction(smartload_protocol_t *ctx);
esp_err_t smartload_clear_status(smartload_protocol_t *ctx);
esp_err_t smartload_clear_alarms(smartload_protocol_t *ctx);
float smartload_volume_liters(uint32_t volume_x100);
float smartload_amount(uint32_t amount_x100);
float smartload_price(uint32_t price_x10000);
float smartload_temp_c(int16_t temp_x100);
float smartload_flow_lpm(uint16_t flow_x100);
