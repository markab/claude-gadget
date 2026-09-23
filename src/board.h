#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-1.75C pin map (from Waveshare's pin_config.h)

// CO5300 AMOLED, QSPI
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    38
#define LCD_CS      12
#define LCD_RESET   1
#define LCD_WIDTH   466
#define LCD_HEIGHT  466
#define LCD_COL_OFFSET 6

// Shared I2C bus: CST9217 touch, AXP2101 PMU, QMI8658 IMU
#define I2C_SDA     15
#define I2C_SCL     14
#define TP_INT      11
#define TP_RST      2
#define TP_ADDR     0x5A

// BOOT button (active low). PWR button is read through the AXP2101.
#define BTN_BOOT    0
