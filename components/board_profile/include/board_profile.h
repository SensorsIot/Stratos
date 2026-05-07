#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t     id;
    const char *name;

    spi_host_device_t rf_spi_host;
    gpio_num_t  rf_sck;
    gpio_num_t  rf_miso;
    gpio_num_t  rf_mosi;
    gpio_num_t  rf_nss;
    gpio_num_t  rf_rst;
    gpio_num_t  rf_dio0;
    gpio_num_t  rf_dio1;
    gpio_num_t  rf_dio2;

    gpio_num_t  oled_sda;
    gpio_num_t  oled_scl;
    gpio_num_t  oled_rst;

    gpio_num_t  led;
    gpio_num_t  button;
    gpio_num_t  battery_adc;
} board_profile_t;

const board_profile_t *board_profile_get(uint8_t id);
const board_profile_t *board_profile_active(void);
void                   board_profile_set_active(uint8_t id);

#ifdef __cplusplus
}
#endif
