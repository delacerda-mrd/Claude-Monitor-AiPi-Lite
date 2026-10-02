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
#define PIN_BTN_PWR      1      /* left "power" button (xiaozhi POWER_BUTTON_GPIO) -- probing */
#define PIN_CHG_DET      8      /* charger status, high = charging (xiaozhi) -- probing */
#define PIN_PROBE_21    21      /* Lipe: "ES8311 charge counter" -- unconfirmed, probing */
#define PIN_LED         46      /* WS2812, 1 pixel (strapping pin, OK post-boot) */
#define PIN_PWR_HOLD    10      /* drive HIGH to stay powered on battery */
/* Battery sense: ADC1 channel 1 = GPIO 2 (see power.c) */
/* Audio pins live in audio.c (I2C SDA5/SCL4, I2S MCLK6/BCLK14/WS12/DOUT11, PA9) */

#define LCD_W           128
#define LCD_H           128
