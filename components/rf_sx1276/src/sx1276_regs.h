#pragma once

#define REG_FIFO          0x00
#define REG_OP_MODE       0x01
#define REG_BITRATE_MSB   0x02
#define REG_BITRATE_LSB   0x03
#define REG_FDEV_MSB      0x04
#define REG_FDEV_LSB      0x05
#define REG_FRF_MSB       0x06
#define REG_FRF_MID       0x07
#define REG_FRF_LSB       0x08
#define REG_PA_CONFIG     0x09
#define REG_LNA           0x0C
#define REG_RX_CONFIG     0x0D
#define REG_RSSI_VALUE    0x11
#define REG_RXBW          0x12
#define REG_AFC_BW        0x13
#define REG_PREAMBLE_DET  0x1F
#define REG_RX_TIMEOUT1   0x16
#define REG_PREAMBLE_MSB  0x25
#define REG_PREAMBLE_LSB  0x26
#define REG_SYNC_CONFIG   0x27
#define REG_SYNC_VALUE1   0x28
#define REG_PACKET_CONFIG1 0x30
#define REG_PACKET_CONFIG2 0x31
#define REG_PAYLOAD_LEN   0x32
#define REG_FIFO_THRESH   0x35
#define REG_DIO_MAPPING1  0x40
#define REG_DIO_MAPPING2  0x41
#define REG_VERSION       0x42
#define REG_PA_DAC        0x4D

/* RegOpMode: bit 7 = LongRangeMode (0=FSK), bits 6:5 = ModulationType
   (00=FSK), bits 2:0 = Mode. */
#define MODE_SLEEP        0x00
#define MODE_STDBY        0x01
#define MODE_FS_RX        0x04
#define MODE_RX_CONT      0x05

/* Compile-time guard: TX modes are never used by this driver. */
#define MODE_FORBIDDEN_TX 0x03
