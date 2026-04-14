#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uart_port_t uart_num;
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    gpio_num_t de_pin;
    gpio_num_t re_pin;
    uint32_t baudrate;
    uint32_t response_timeout_ms;
    bool debug;
} modbus_rtu_config_t;

typedef struct modbus_rtu_context *modbus_rtu_handle_t;

esp_err_t modbus_rtu_init(const modbus_rtu_config_t *cfg, modbus_rtu_handle_t *out_handle);
void modbus_rtu_deinit(modbus_rtu_handle_t handle);
void modbus_rtu_set_debug(modbus_rtu_handle_t handle, bool enable);
void modbus_rtu_set_timeout(modbus_rtu_handle_t handle, uint32_t timeout_ms);

esp_err_t modbus_rtu_read_holding_registers(modbus_rtu_handle_t handle,
                                            uint8_t slave_addr,
                                            uint16_t start_addr,
                                            uint16_t count,
                                            uint16_t *out_regs);

esp_err_t modbus_rtu_write_multiple_registers(modbus_rtu_handle_t handle,
                                              uint8_t slave_addr,
                                              uint16_t start_addr,
                                              uint16_t count,
                                              const uint16_t *regs);

static inline esp_err_t modbus_rtu_write_single_register(modbus_rtu_handle_t handle,
                                                         uint8_t slave_addr,
                                                         uint16_t addr,
                                                         uint16_t value)
{
    return modbus_rtu_write_multiple_registers(handle, slave_addr, addr, 1, &value);
}
