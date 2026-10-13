/* Shared state and module interfaces. */
#ifndef KVM_H
#define KVM_H

#include <stdbool.h>
#include <stdint.h>

#define CH_A 0
#define CH_B 1

typedef struct { uint8_t r, g, b; } rgb_t;

typedef struct {
    uint8_t  channel;      /* channel restored at power-up         */
    uint8_t  brightness;   /* global LED brightness 0..255         */
    uint8_t  auto_switch;  /* follow the other host if active leaves */
    uint8_t  anim;         /* wave animation on switch             */
    uint16_t anim_ms;      /* wave duration                        */
    rgb_t    color[2];     /* channel colors A, B                  */
} settings_t;

extern settings_t g_set;
extern volatile uint32_t g_ms;

/* main.c */
uint8_t kvm_hdmi(void);
uint8_t kvm_usb(void);
void    kvm_select(uint8_t hdmi, uint8_t usb);
bool    det_hdmi(uint8_t ch);
bool    det_usb(uint8_t ch);
bool    host_present(uint8_t ch);
void    settings_touch(void);

/* settings.c */
void settings_defaults(void);
void settings_load(void);
bool settings_save(void);

/* leds.c */
void leds_init(void);
void leds_switch_anim(uint8_t from_ch, uint8_t to_ch);
void leds_task(void);

/* uart.c */
void uart_init(void);
int  uart_getc(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_putu(uint32_t v);
void uart_puthex(uint32_t v, uint8_t digits);

/* cli.c */
void cli_task(void);
void cli_log(const char *msg);

#endif
