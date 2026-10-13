/* Pin map and hardware constants for the KVM switch (rev A). */
#ifndef BOARD_H
#define BOARD_H

#include "stm32c0xx.h"

#define SYSCLK_HZ       48000000u

/* All MCU signals live on GPIOA. */
#define PIN_UART_TX     0   /* USART1_TX, AF4, test point TX           */
#define PIN_UART_RX     1   /* USART1_RX, AF4, test point RX           */
#define PIN_LED         2   /* SPI1_MOSI, AF0, SK6812 data in          */
#define PIN_BTN         3   /* push button, active low, RC debounced   */
#define PIN_DET_USB_A   4   /* USB-C host A VBUS via 10k               */
#define PIN_DET_HDMI_A  5   /* HDMI source A +5V via 10k               */
#define PIN_HDMI_SEL    6   /* HD3SS215 Dx_SEL, 10k pull-down          */
#define PIN_USB_SEL     7   /* TS3USB221 S, 10k pull-down              */
#define PIN_UP_DET      8   /* USB2514B VBUS_DET, 10k pull-up to 3V3   */
#define PIN_DET_HDMI_B  11  /* HDMI source B +5V via 10k               */
#define PIN_DET_USB_B   12  /* USB-C host B VBUS via 10k               */

/* Level on the select pin that routes host B. TS3USB221: S=H -> 2D (J6).
 * HD3SS215: check Dx_SEL polarity against the datasheet / first bring-up. */
#define HDMI_SEL_B_LEVEL 1
#define USB_SEL_B_LEVEL  1

/* SK6812MINI-E chain D2..D6. Index 0 (D2) sits nearest the host A
 * connectors, index 4 (D6) nearest host B. */
#define LED_COUNT       5

/* Last 2 KB flash page holds settings (linker script keeps code out). */
#define SETTINGS_PAGE       15u
#define SETTINGS_ADDR       (FLASH_BASE + SETTINGS_PAGE * 2048u)
#define SETTINGS_PAGE_SIZE  2048u

/* GPIOA helpers */
enum { MODE_IN = 0, MODE_OUT = 1, MODE_AF = 2, MODE_ANALOG = 3 };
enum { PULL_NONE = 0, PULL_UP = 1, PULL_DOWN = 2 };

static inline void pa_mode(uint32_t pin, uint32_t mode)
{
    GPIOA->MODER = (GPIOA->MODER & ~(3u << (pin * 2))) | (mode << (pin * 2));
}

static inline void pa_pull(uint32_t pin, uint32_t pull)
{
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << (pin * 2))) | (pull << (pin * 2));
}

static inline void pa_af(uint32_t pin, uint32_t af)
{
    uint32_t i = pin >> 3, s = (pin & 7) * 4;
    GPIOA->AFR[i] = (GPIOA->AFR[i] & ~(0xFu << s)) | (af << s);
    pa_mode(pin, MODE_AF);
}

static inline void pa_write(uint32_t pin, uint32_t level)
{
    GPIOA->BSRR = level ? (1u << pin) : (1u << (pin + 16));
}

#endif
