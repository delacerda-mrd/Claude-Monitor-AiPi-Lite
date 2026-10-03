/*
 * board.h  --  AIPI-Lite (ESP32-S3) pin map.
 * Authoritative source: docs/BOARD_REFERENCE.md §10. Never guess a pin.
 */
#pragma once

/* Display: ST7735 128x128, SPI */
#define PIN_LCD_CLK     16
#define PIN_LCD_MOSI    17
#define PIN_LCD_DC       7
#define PIN_LCD_CS      15
#define PIN_LCD_RST     18
#define PIN_LCD_BL       3

#define PIN_BTN         42      /* right button, active-low */
#define PIN_BTN_PWR      1      /* left button, active-low (verified 2026-10-02) */
#define PIN_VBUS         8      /* USB power present, 1 = plugged (verified 2026-10-02) */
#define PIN_CHRG        21      /* charger CHRG, 0 = charging (probable, 2026-10-02) */
#define PIN_LED         46      /* WS2812, 1 pixel (strapping pin, OK post-boot) */
#define PIN_PWR_HOLD    10      /* drive HIGH to stay powered on battery */
/* Battery sense: ADC1 channel 1 = GPIO 2 (see power.c) */
/* Audio pins live in audio.c (I2C SDA5/SCL4, I2S MCLK6/BCLK14/WS12/DOUT11, PA9) */

#define LCD_W           128
#define LCD_H           128

/* Audio (meter_core audio.c): ES8311 on I2C1, I2S0, NS4150-style amp.
 * From xiaozhi-esp32 boards/aipi-lite/config.h; verified in use since v1. */
#define BOARD_AUDIO_I2C_PORT     I2C_NUM_1
#define BOARD_AUDIO_I2C_SDA      GPIO_NUM_5
#define BOARD_AUDIO_I2C_SCL      GPIO_NUM_4
#define BOARD_AUDIO_I2S_MCLK     GPIO_NUM_6
#define BOARD_AUDIO_I2S_BCLK     GPIO_NUM_14
#define BOARD_AUDIO_I2S_WS       GPIO_NUM_12
#define BOARD_AUDIO_I2S_DOUT     GPIO_NUM_11
#define BOARD_AUDIO_I2S_DIN      GPIO_NUM_13
#define BOARD_AUDIO_PA_PIN       GPIO_NUM_9
#define BOARD_AUDIO_PA_ON_LEVEL  1          /* amp enable is active-high */
