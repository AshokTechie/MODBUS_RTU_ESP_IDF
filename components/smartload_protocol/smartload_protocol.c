#include "smartload_protocol.h"

#include "esp_check.h"

esp_err_t smartload_protocol_init(smartload_protocol_t *ctx, modbus_rtu_handle_t modbus, uint8_t slave_addr)
{
    ESP_RETURN_ON_FALSE(ctx && modbus, ESP_ERR_INVALID_ARG, "smartload_protocol", "invalid args");
    ctx->modbus = modbus;
    ctx->slave_addr = slave_addr;
    return ESP_OK;
}

esp_err_t smartload_read_status(smartload_protocol_t *ctx, smartload_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(ctx && out_status, ESP_ERR_INVALID_ARG, "smartload_protocol", "invalid args");

    uint16_t regs[17] = {0};
    ESP_RETURN_ON_ERROR(
        modbus_rtu_read_holding_registers(ctx->modbus, ctx->slave_addr, 0x0000, 17, regs),
        "smartload_protocol",
        "read status failed");

    out_status->status_word = regs[0x00];
    out_status->delivery_volume_x100 = ((uint32_t)regs[0x01] << 16) | regs[0x03];
    out_status->delivery_amount_x100 = ((uint32_t)regs[0x04] << 16) | regs[0x05];
    out_status->unit_price_x10000 = ((uint32_t)regs[0x06] << 16) | regs[0x07];
    out_status->transaction_num = regs[0x08];
    out_status->temperature_x100 = (int16_t)regs[0x09];
    out_status->preset_volume_x100 = ((uint32_t)regs[0x0A] << 16) | regs[0x0B];
    out_status->total_volume_x100 = ((uint32_t)regs[0x0C] << 16) | regs[0x0D];
    out_status->hose_number = regs[0x0E];
    out_status->product_code = regs[0x0F];
    out_status->alarm_register = regs[0x10];

    uint16_t flow = 0;
    if (modbus_rtu_read_holding_registers(ctx->modbus, ctx->slave_addr, 0x0021, 1, &flow) == ESP_OK) {
        out_status->flow_rate_x100 = flow;
    }

    return ESP_OK;
}

esp_err_t smartload_read_status_word(smartload_protocol_t *ctx, uint16_t *out_status)
{
    return modbus_rtu_read_holding_registers(ctx->modbus, ctx->slave_addr, 0x0000, 1, out_status);
}

esp_err_t smartload_read_device_info(smartload_protocol_t *ctx, uint16_t *out_device_id, uint16_t *out_firmware_version)
{
    uint16_t regs[2] = {0};
    ESP_RETURN_ON_ERROR(
        modbus_rtu_read_holding_registers(ctx->modbus, ctx->slave_addr, 0x0064, 2, regs),
        "smartload_protocol",
        "read device info failed");
    *out_device_id = regs[0];
    *out_firmware_version = regs[1];
    return ESP_OK;
}

esp_err_t smartload_send_command(smartload_protocol_t *ctx, smartload_command_t cmd)
{
    return modbus_rtu_write_single_register(ctx->modbus, ctx->slave_addr, 0x0000, (uint16_t)cmd);
}

esp_err_t smartload_authorize(smartload_protocol_t *ctx, uint16_t recipe, uint16_t additive_config, uint16_t side_nozzle)
{
    ESP_RETURN_ON_ERROR(modbus_rtu_write_single_register(ctx->modbus, ctx->slave_addr, 0x0001, recipe), "smartload_protocol", "recipe failed");
    ESP_RETURN_ON_ERROR(modbus_rtu_write_single_register(ctx->modbus, ctx->slave_addr, 0x0002, additive_config), "smartload_protocol", "additive failed");
    ESP_RETURN_ON_ERROR(modbus_rtu_write_single_register(ctx->modbus, ctx->slave_addr, 0x0003, side_nozzle), "smartload_protocol", "side/nozzle failed");
    return smartload_send_command(ctx, SMARTLOAD_CMD_AUTHORIZE);
}

esp_err_t smartload_write_preset_volume(smartload_protocol_t *ctx, uint32_t preset_volume_x100)
{
    uint16_t regs[2] = {
        (uint16_t)((preset_volume_x100 >> 16) & 0xFFFF),
        (uint16_t)(preset_volume_x100 & 0xFFFF),
    };
    return modbus_rtu_write_multiple_registers(ctx->modbus, ctx->slave_addr, 0x0039, 2, regs);
}

esp_err_t smartload_write_components(smartload_protocol_t *ctx, uint16_t components)
{
    return modbus_rtu_write_single_register(ctx->modbus, ctx->slave_addr, 0x0041, components);
}

esp_err_t smartload_auth_batch(smartload_protocol_t *ctx, uint32_t preset_volume_x100, uint16_t components)
{
    ESP_RETURN_ON_ERROR(smartload_write_preset_volume(ctx, preset_volume_x100), "smartload_protocol", "preset volume failed");
    ESP_RETURN_ON_ERROR(smartload_write_components(ctx, components), "smartload_protocol", "components failed");
    return smartload_send_command(ctx, SMARTLOAD_CMD_AUTH_BATCH);
}

esp_err_t smartload_start_batch(smartload_protocol_t *ctx)
{
    return smartload_send_command(ctx, SMARTLOAD_CMD_START_BATCH);
}

esp_err_t smartload_stop_batch(smartload_protocol_t *ctx)
{
    return smartload_send_command(ctx, SMARTLOAD_CMD_STOP_BATCH);
}

esp_err_t smartload_end_transaction(smartload_protocol_t *ctx)
{
    return smartload_send_command(ctx, SMARTLOAD_CMD_END_TRANSACTION);
}

esp_err_t smartload_clear_status(smartload_protocol_t *ctx)
{
    return smartload_send_command(ctx, SMARTLOAD_CMD_CLEAR_STATUS);
}

esp_err_t smartload_clear_alarms(smartload_protocol_t *ctx)
{
    return smartload_send_command(ctx, SMARTLOAD_CMD_CLEAR_ALARMS);
}

float smartload_volume_liters(uint32_t volume_x100)
{
    return volume_x100 / 100.0f;
}

float smartload_amount(uint32_t amount_x100)
{
    return amount_x100 / 100.0f;
}

float smartload_price(uint32_t price_x10000)
{
    return price_x10000 / 10000.0f;
}

float smartload_temp_c(int16_t temp_x100)
{
    return temp_x100 / 100.0f;
}

float smartload_flow_lpm(uint16_t flow_x100)
{
    return flow_x100 / 100.0f;
}
