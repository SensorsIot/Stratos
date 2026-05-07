#include "board_profile.h"

static const board_profile_t s_profiles[] = {
    {
        .id           = 0,
        .name         = "ttgo_lora32_v21_16",
        .rf_spi_host  = SPI3_HOST,
        .rf_sck       = GPIO_NUM_5,
        .rf_miso      = GPIO_NUM_19,
        .rf_mosi      = GPIO_NUM_27,
        .rf_nss       = GPIO_NUM_18,
        .rf_rst       = GPIO_NUM_23,
        .rf_dio0      = GPIO_NUM_26,
        .rf_dio1      = GPIO_NUM_33,
        .rf_dio2      = GPIO_NUM_32,
        .oled_sda     = GPIO_NUM_21,
        .oled_scl     = GPIO_NUM_22,
        .oled_rst     = GPIO_NUM_16,
        .led          = GPIO_NUM_25,
        .button       = GPIO_NUM_0,
        .battery_adc  = GPIO_NUM_35,
    },
};

static uint8_t s_active = 0;

const board_profile_t *board_profile_get(uint8_t id)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if (s_profiles[i].id == id) return &s_profiles[i];
    }
    return NULL;
}

const board_profile_t *board_profile_active(void)
{
    const board_profile_t *p = board_profile_get(s_active);
    return p ? p : &s_profiles[0];
}

void board_profile_set_active(uint8_t id)
{
    if (board_profile_get(id)) s_active = id;
}
