/* KVM switch firmware - STM32C031G6, bare metal on CMSIS headers.
 *
 * Main loop: debounce inputs every millisecond, handle the button, run the
 * USB re-enumeration sequence, render LEDs at 100 Hz, serve the UART CLI and
 * write settings to flash a few seconds after they change. */
#include "board.h"
#include "kvm.h"

#define BTN_LONG_MS        800u
#define DET_DEBOUNCE_MS    50u
#define BTN_DEBOUNCE_MS    20u
#define USB_DETACH_MS      30u   /* hub sees VBUS gone before the mux moves */
#define USB_SETTLE_MS      50u   /* mux settled before hub reconnects       */
#define SETTINGS_DELAY_MS  3000u /* coalesce changes before a flash write   */

volatile uint32_t g_ms;

void SysTick_Handler(void)
{
    g_ms++;
}

/* ---- inputs ------------------------------------------------------------ */

enum { IN_BTN, IN_HDMI_A, IN_HDMI_B, IN_USB_A, IN_USB_B, IN_COUNT };

typedef struct {
    uint8_t pin, active_low, debounce, state, cnt;
    const char *name;
} input_t;

static input_t inputs[IN_COUNT] = {
    [IN_BTN]    = { PIN_BTN,        1, BTN_DEBOUNCE_MS, 0, 0, "button" },
    [IN_HDMI_A] = { PIN_DET_HDMI_A, 0, DET_DEBOUNCE_MS, 0, 0, "HDMI A" },
    [IN_HDMI_B] = { PIN_DET_HDMI_B, 0, DET_DEBOUNCE_MS, 0, 0, "HDMI B" },
    [IN_USB_A]  = { PIN_DET_USB_A,  0, DET_DEBOUNCE_MS, 0, 0, "USB A" },
    [IN_USB_B]  = { PIN_DET_USB_B,  0, DET_DEBOUNCE_MS, 0, 0, "USB B" },
};

/* Returns a bitmask of inputs whose debounced state changed. */
static uint32_t inputs_poll(void)
{
    uint32_t idr = GPIOA->IDR, changed = 0;
    for (uint32_t i = 0; i < IN_COUNT; i++) {
        input_t *in = &inputs[i];
        uint8_t raw = ((idr >> in->pin) & 1u) ^ in->active_low;
        if (raw == in->state) {
            in->cnt = 0;
        } else if (++in->cnt >= in->debounce) {
            in->state = raw;
            in->cnt = 0;
            changed |= 1u << i;
        }
    }
    return changed;
}

bool det_hdmi(uint8_t ch)     { return inputs[ch ? IN_HDMI_B : IN_HDMI_A].state; }
bool det_usb(uint8_t ch)      { return inputs[ch ? IN_USB_B : IN_USB_A].state; }
bool host_present(uint8_t ch) { return det_hdmi(ch) || det_usb(ch); }

/* ---- switching --------------------------------------------------------- */

enum { USB_IDLE, USB_DETACH, USB_SETTLE };

static uint8_t hdmi_ch;
static uint8_t usb_target, usb_ch;
static uint8_t usb_state;
static uint32_t usb_t;

uint8_t kvm_hdmi(void) { return hdmi_ch; }
uint8_t kvm_usb(void)  { return usb_target; }

/* UP_DET is open drain against the hub's pull-up: release = high. */
static void up_det(bool high) { pa_write(PIN_UP_DET, high); }

void kvm_select(uint8_t hdmi, uint8_t usb)
{
    if (hdmi != hdmi_ch) {
        pa_write(PIN_HDMI_SEL, hdmi ? HDMI_SEL_B_LEVEL : !HDMI_SEL_B_LEVEL);
        leds_switch_anim(hdmi_ch, hdmi);
        hdmi_ch = hdmi;
    }
    if (usb != usb_target) {
        /* Detach the hub from the old host first so both hosts see a clean
         * unplug / plug instead of a hub that silently changed upstream. */
        usb_target = usb;
        usb_state = USB_DETACH;
        usb_t = g_ms;
        up_det(false);
    }
    if (hdmi == usb && g_set.channel != hdmi) {
        g_set.channel = hdmi;
        settings_touch();
    }
}

static void usb_task(void)
{
    switch (usb_state) {
    case USB_DETACH:
        if (g_ms - usb_t >= USB_DETACH_MS) {
            usb_ch = usb_target;
            pa_write(PIN_USB_SEL, usb_ch ? USB_SEL_B_LEVEL : !USB_SEL_B_LEVEL);
            usb_state = USB_SETTLE;
            usb_t = g_ms;
        }
        break;
    case USB_SETTLE:
        if (g_ms - usb_t >= USB_SETTLE_MS)
            usb_state = USB_IDLE;
        break;
    default:
        /* Hub only connects upstream while the selected host supplies VBUS. */
        up_det(det_usb(usb_ch));
        break;
    }
}

static void switch_to(uint8_t ch)
{
    kvm_select(ch, ch);
}

/* If the active host went away and the other one is there, follow it. */
static void auto_switch(void)
{
    uint8_t ch = hdmi_ch;
    if (g_set.auto_switch && !host_present(ch) && host_present(!ch)) {
        cli_log(ch ? "auto: switching to A" : "auto: switching to B");
        switch_to(!ch);
    }
}

/* ---- button ------------------------------------------------------------ */

static void cycle_brightness(void)
{
    static const uint8_t levels[] = { 8, 32, 96, 255 };
    uint8_t next = levels[0];
    for (uint32_t i = 0; i < sizeof levels; i++) {
        if (levels[i] > g_set.brightness) {
            next = levels[i];
            break;
        }
    }
    g_set.brightness = next;
    settings_touch();
}

static void button_task(void)
{
    static bool was_down, long_done;
    static uint32_t t_down;
    bool down = inputs[IN_BTN].state;

    if (down && !was_down) {
        t_down = g_ms;
        long_done = false;
    } else if (down && !long_done && g_ms - t_down >= BTN_LONG_MS) {
        long_done = true;
        cycle_brightness();
    } else if (!down && was_down && !long_done) {
        switch_to(!hdmi_ch);
    }
    was_down = down;
}

/* ---- settings persistence --------------------------------------------- */

static bool set_dirty;
static uint32_t set_t;

void settings_touch(void)
{
    set_dirty = true;
    set_t = g_ms;
}

static void settings_task(void)
{
    if (set_dirty && g_ms - set_t >= SETTINGS_DELAY_MS) {
        set_dirty = false;
        if (!settings_save())
            cli_log("settings: flash write failed");
    }
}

/* ---- init -------------------------------------------------------------- */

static void clock_init(void)
{
    /* 48 MHz needs one flash wait state; set it before speeding up. */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_0 |
                 FLASH_ACR_PRFTEN | FLASH_ACR_ICEN;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_0)
        ;
    RCC->CR &= ~RCC_CR_HSIDIV;          /* HSISYS = HSI48 / 1 */
    SystemCoreClock = SYSCLK_HZ;
    SysTick_Config(SYSCLK_HZ / 1000u);
}

static void gpio_init(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;

    pa_mode(PIN_BTN, MODE_IN);          /* external pull-up + RC */

    static const uint8_t det_pins[] = {
        PIN_DET_HDMI_A, PIN_DET_HDMI_B, PIN_DET_USB_A, PIN_DET_USB_B
    };
    for (uint32_t i = 0; i < sizeof det_pins; i++) {
        /* No external pull-down: keep the pin defined with nothing plugged. */
        pa_pull(det_pins[i], PULL_DOWN);
        pa_mode(det_pins[i], MODE_IN);
    }

    uint8_t ch = g_set.channel;
    pa_write(PIN_HDMI_SEL, ch ? HDMI_SEL_B_LEVEL : !HDMI_SEL_B_LEVEL);
    pa_write(PIN_USB_SEL, ch ? USB_SEL_B_LEVEL : !USB_SEL_B_LEVEL);
    pa_mode(PIN_HDMI_SEL, MODE_OUT);
    pa_mode(PIN_USB_SEL, MODE_OUT);
    hdmi_ch = usb_ch = usb_target = ch;

    /* Hold the hub detached until the VBUS input has been debounced. */
    GPIOA->OTYPER |= 1u << PIN_UP_DET;
    up_det(false);
    pa_mode(PIN_UP_DET, MODE_OUT);
    usb_state = USB_SETTLE;
    usb_t = 0;
}

static void iwdg_init(void)
{
    RCC->APBENR1 |= RCC_APBENR1_DBGEN;
    DBG->APBFZ1 |= DBG_APB_FZ1_DBG_IWDG_STOP;  /* freeze while halted in debugger */

    IWDG->KR = 0xCCCC;      /* start */
    IWDG->KR = 0x5555;      /* unlock PR/RLR */
    IWDG->PR = 3;           /* LSI 32 kHz / 32 = 1 kHz */
    IWDG->RLR = 1000;       /* ~1 s */
    while (IWDG->SR)
        ;
    IWDG->KR = 0xAAAA;
}

int main(void)
{
    clock_init();
    settings_load();
    gpio_init();
    uart_init();
    leds_init();
    iwdg_init();

    uart_puts("\r\nKVM switch ready, type 'help'\r\n");

    uint32_t last_ms = g_ms;
    for (;;) {
        IWDG->KR = 0xAAAA;

        if (g_ms != last_ms) {
            last_ms = g_ms;
            uint32_t changed = inputs_poll();
            for (uint32_t i = IN_HDMI_A; i < IN_COUNT; i++) {
                if (changed & (1u << i)) {
                    uart_puts(inputs[i].name);
                    uart_puts(inputs[i].state ? ": connected\r\n" : ": disconnected\r\n");
                }
            }
            if (changed & ~(1u << IN_BTN))
                auto_switch();
            button_task();
            usb_task();
            settings_task();
            leds_task();
        }

        cli_task();
        __WFI();            /* SysTick or UART RX wakes us */
    }
}

/* Called by __libc_init_array from the startup code; linked with
 * -nostartfiles so there is no crti.o to provide it. */
void _init(void) {}
