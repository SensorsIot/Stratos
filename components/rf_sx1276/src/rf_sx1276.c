#include "rf_sx1276.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"

#include "board_profile.h"
#include "sx1276_regs.h"

static const char *TAG = "rf";

#define FXOSC_HZ 32000000ULL
#define BYTE_QUEUE_DEPTH 1024

static spi_device_handle_t s_spi;
static QueueHandle_t       s_bytes;
static SemaphoreHandle_t   s_irq_sem;
static int16_t             s_rssi_dbm = 0;
static rf_profile_t        s_active_profile;

/* RS41 sync word baseline below is from the rs1729 reference. Phase 2 bench
   tuning will confirm/refine — see FSD §6.3.4 and §5 R-2. */

static const rf_profile_t s_profiles[] = {
    {
        /* Sync word matches dl9rdz/rdz_ttgo_sonde RS41.cpp (known-working
           SX1276/SX1278 RS41 receiver). The earlier "0x10,0xB6,0xCA,..."
           value is commented out in their source as an obsolete baseline. */
        .type        = SONDE_TYPE_RS41,
        .bitrate_bps = 4800,
        .freq_dev_hz = 4800,
        .rxbw_hz     = 12500,    /* rxbw_idx 4 = 12.5 kHz */
        .sync_word   = {0x08, 0x6D, 0x53, 0x88, 0x44, 0x69, 0x48, 0x1F},
        .sync_len    = 8,
    },
    {
        .type        = SONDE_TYPE_M20,
        .bitrate_bps = 9600,
        .freq_dev_hz = 9600,
        .rxbw_hz     = 25000,
        .sync_word   = {0x99, 0x9A},
        .sync_len    = 2,
    },
    {
        .type        = SONDE_TYPE_M10,
        .bitrate_bps = 9600,
        .freq_dev_hz = 9600,
        .rxbw_hz     = 25000,
        .sync_word   = {0x66, 0x65},
        .sync_len    = 2,
    },
    {
        .type        = SONDE_TYPE_DFM,
        .bitrate_bps = 2500,
        .freq_dev_hz = 2500,
        .rxbw_hz     = 10000,
        .sync_word   = {0x45, 0xCF, 0x9A, 0x90},
        .sync_len    = 4,
    },
};

const rf_profile_t *st_rf_profile_for(sonde_type_t t)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if (s_profiles[i].type == t) return &s_profiles[i];
    }
    return NULL;
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(reg | 0x80), val };
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), 0x00 };
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK) *val = rx[1];
    return err;
}

static esp_err_t set_mode(uint8_t mode)
{
    if ((mode & 0x07) == MODE_FORBIDDEN_TX) {
        ESP_LOGE(TAG, "BUG: TX mode requested, refusing");
        return ESP_ERR_INVALID_ARG;
    }
    /* keep LongRangeMode=0 (FSK) */
    return reg_write(REG_OP_MODE, mode & 0x07);
}

static void hw_reset(gpio_num_t rst)
{
    gpio_config_t g = { .pin_bit_mask = 1ULL << rst, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level(rst, 0);
    ets_delay_us(200);
    gpio_set_level(rst, 1);
    ets_delay_us(5000);
}

static void IRAM_ATTR dio0_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_irq_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void rx_task(void *arg)
{
    (void)arg;
    /* On FifoLevel IRQ, drain bytes from the SX1276 FIFO (REG_FIFO).
       Quantity is at least FIFO_THRESH+1; drain until FifoEmpty asserted.
       Independent of IRQs, poll RegRssiValue every loop so the API can
       expose noise-floor RSSI even when no sync has fired yet — needed
       for diagnosing whether the RF front-end is alive at all. */
    for (;;) {
        if (xSemaphoreTake(s_irq_sem, pdMS_TO_TICKS(250)) == pdTRUE) {
            for (int i = 0; i < 64; i++) {
                uint8_t flags2;
                if (reg_read(0x3F, &flags2) != ESP_OK) break;  /* IrqFlags2 */
                if (flags2 & 0x40) break;                      /* FifoEmpty */
                uint8_t b;
                if (reg_read(REG_FIFO, &b) != ESP_OK) break;
                xQueueSend(s_bytes, &b, 0);
            }
        }
        uint8_t rssi;
        if (reg_read(REG_RSSI_VALUE, &rssi) == ESP_OK) {
            s_rssi_dbm = -((int16_t)rssi >> 1);
        }
    }
}

esp_err_t st_rf_init(void)
{
    const board_profile_t *bp = board_profile_active();

    s_bytes   = xQueueCreate(BYTE_QUEUE_DEPTH, sizeof(uint8_t));
    s_irq_sem = xSemaphoreCreateBinary();
    if (!s_bytes || !s_irq_sem) return ESP_ERR_NO_MEM;

    spi_bus_config_t bus = {
        .miso_io_num     = bp->rf_miso,
        .mosi_io_num     = bp->rf_mosi,
        .sclk_io_num     = bp->rf_sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(bp->rf_spi_host, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = bp->rf_nss,
        .queue_size     = 4,
    };
    err = spi_bus_add_device(bp->rf_spi_host, &devcfg, &s_spi);
    if (err != ESP_OK) return err;

    hw_reset(bp->rf_rst);

    uint8_t ver = 0;
    reg_read(REG_VERSION, &ver);
    ESP_LOGI(TAG, "SX1276 silicon version 0x%02X", ver);

    /* FSK mode (LongRangeMode=0), Standby */
    reg_write(REG_OP_MODE, 0x00);
    set_mode(MODE_STDBY);

    /* Disable LoRa packet engine variants by setting fixed-len FSK packet
       mode with payload length 0 (we read raw bytes via FifoLevel IRQ). */
    reg_write(REG_PACKET_CONFIG1, 0x00); /* fixed length, no CRC, no whitening */
    reg_write(REG_PACKET_CONFIG2, 0x40); /* PacketMode=1 (packet), no IO */
    reg_write(REG_PAYLOAD_LEN,    0xFF);
    reg_write(REG_FIFO_THRESH,    0x80 | 31); /* TxStartCondition=FifoNotEmpty (irrelevant for RX); FifoThreshold=31 */

    /* DIO0 = FifoLevel (FSK mode RX): RegDioMapping1 = 00 (DIO0 = PayloadReady)
       For continuous RX with FifoLevel, set DIO0 mapping = 01 (FifoLevel).  */
    reg_write(REG_DIO_MAPPING1, 0x40);

    /* IRQ pin */
    gpio_config_t gi = {
        .pin_bit_mask = 1ULL << bp->rf_dio0,
        .mode         = GPIO_MODE_INPUT,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&gi);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(bp->rf_dio0, dio0_isr, NULL);

    BaseType_t r = xTaskCreate(rx_task, "rf_rx", 4096, NULL, 18, NULL);
    if (r != pdPASS) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t st_rf_set_freq_hz(uint32_t hz)
{
    /* Frf = (freq_hz * 2^19) / 32MHz */
    uint64_t frf = ((uint64_t)hz << 19) / FXOSC_HZ;
    set_mode(MODE_STDBY);
    reg_write(REG_FRF_MSB, (frf >> 16) & 0xFF);
    reg_write(REG_FRF_MID, (frf >>  8) & 0xFF);
    reg_write(REG_FRF_LSB,  frf        & 0xFF);
    set_mode(MODE_RX_CONT);
    ESP_LOGI(TAG, "freq=%lu Hz (Frf=%lu)", (unsigned long)hz, (unsigned long)frf);
    return ESP_OK;
}

static uint8_t rxbw_encode(uint32_t bw_hz)
{
    /* RegRxBw mantissa/exp encoding per SX1276 datasheet §3.5.4. */
    static const struct { uint32_t bw; uint8_t reg; } table[] = {
        {2600,  0x17}, {3100,  0x0F}, {3900,  0x07},
        {5200,  0x16}, {6300,  0x0E}, {7800,  0x06},
        {10400, 0x15}, {12500, 0x0D}, {15600, 0x05},
        {20800, 0x14}, {25000, 0x0C}, {31300, 0x04},
        {41700, 0x13}, {50000, 0x0B}, {62500, 0x03},
        {83333, 0x12}, {100000,0x0A}, {125000,0x02},
        {166700,0x11}, {200000,0x09}, {250000,0x01},
    };
    uint8_t pick = 0x0D;
    uint32_t best = UINT32_MAX;
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        uint32_t d = (table[i].bw > bw_hz) ? (table[i].bw - bw_hz) : (bw_hz - table[i].bw);
        if (d < best) { best = d; pick = table[i].reg; }
    }
    return pick;
}

esp_err_t st_rf_apply_profile(const rf_profile_t *p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    s_active_profile = *p;
    set_mode(MODE_STDBY);

    /* LNA: max gain (G1=001) with HF boost on (LnaBoostHf=11) — the high-band
       LNA needs the boost for ~3 dB extra sensitivity at 433 MHz. */
    reg_write(REG_LNA, 0x23);

    /* RegRxConfig = 0x1E: AfcAutoOn + AgcAutoOn + RxTrigger=PreambleDetect.
       AutoRestartRx is set in RegSyncConfig (0x57) instead. Matches
       dl9rdz/rdz_ttgo_sonde's known-working setting. */
    reg_write(REG_RX_CONFIG, 0x1E);

    /* RegPreambleDetect = 0xA8: detector on, size=2 bytes, tolerance=8 chips. */
    reg_write(REG_PREAMBLE_DET, 0xA8);

    uint16_t br = (uint16_t)(FXOSC_HZ / p->bitrate_bps);
    reg_write(REG_BITRATE_MSB, (br >> 8) & 0xFF);
    reg_write(REG_BITRATE_LSB,  br       & 0xFF);

    uint16_t fdev = (uint16_t)((p->freq_dev_hz * (uint64_t)(1ULL << 19)) / FXOSC_HZ);
    reg_write(REG_FDEV_MSB, (fdev >> 8) & 0x3F);
    reg_write(REG_FDEV_LSB,  fdev       & 0xFF);

    uint8_t bw = rxbw_encode(p->rxbw_hz);
    reg_write(REG_RXBW,   bw);
    reg_write(REG_AFC_BW, bw);

    /* sync_cfg = 0x50 | (sync_len-1):
       bits 7:6 = 01  AutoRestartRx ON without PLL re-sync
       bit  4   = 1   SyncOn
       bit  3   = 0   FIFO fills on sync match (default)
       bits 2:0 = sync_len-1 (e.g. 7 → 8-byte sync). */
    uint8_t sync_cfg = 0x50 | (p->sync_len ? ((p->sync_len - 1) & 0x07) : 0);
    if (p->sync_len) {
        for (uint8_t i = 0; i < p->sync_len; i++) {
            reg_write(REG_SYNC_VALUE1 + i, p->sync_word[i]);
        }
    }
    reg_write(REG_SYNC_CONFIG, sync_cfg);

    ESP_LOGI(TAG, "profile: type=%d bps=%lu fdev=%lu Hz rxbw=%lu Hz synclen=%u",
             p->type,
             (unsigned long)p->bitrate_bps,
             (unsigned long)p->freq_dev_hz,
             (unsigned long)p->rxbw_hz,
             p->sync_len);
    return ESP_OK;
}

esp_err_t st_rf_start_rx(void)  { return set_mode(MODE_RX_CONT); }
esp_err_t st_rf_stop(void)      { return set_mode(MODE_STDBY); }
int16_t   st_rf_rssi_dbm(void)  { return s_rssi_dbm; }
QueueHandle_t st_rf_byte_queue(void) { return s_bytes; }
