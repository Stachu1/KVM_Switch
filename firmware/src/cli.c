// Line-based command interface on the UART test points.
#include <string.h>
#include "board.h"
#include "kvm.h"

#define LINE_MAX  48u
#define ARGS_MAX  4u

static char line[LINE_MAX];
static uint32_t len;

void cli_log(const char *msg)
{
    uart_puts(msg);
    uart_puts("\r\n");
}

static void put_ch(uint8_t ch) { uart_putc(ch ? 'B' : 'A'); }

static void put_onoff(const char *name, bool on)
{
    uart_puts(name);
    uart_puts(on ? "on\r\n" : "off\r\n");
}

static void put_color(rgb_t c)
{
    uart_puthex(c.r, 2);
    uart_puthex(c.g, 2);
    uart_puthex(c.b, 2);
}

static void cmd_status(void)
{
    uart_puts("HDMI -> ");  put_ch(kvm_hdmi());
    uart_puts("   USB -> "); put_ch(kvm_usb());
    uart_puts("\r\nhost A: HDMI ");
    uart_puts(det_hdmi(CH_A) ? "yes" : "no ");
    uart_puts("  USB ");
    uart_puts(det_usb(CH_A) ? "yes" : "no");
    uart_puts("\r\nhost B: HDMI ");
    uart_puts(det_hdmi(CH_B) ? "yes" : "no ");
    uart_puts("  USB ");
    uart_puts(det_usb(CH_B) ? "yes" : "no");
    uart_puts("\r\nbright ");  uart_putu(g_set.brightness);
    uart_puts("\r\n");
    put_onoff("auto   ", g_set.auto_switch);
    put_onoff("anim   ", g_set.anim);
    uart_puts("animms ");  uart_putu(g_set.anim_ms);
    uart_puts("\r\ncolor A ");  put_color(g_set.color[CH_A]);
    uart_puts("  color B ");    put_color(g_set.color[CH_B]);
    uart_puts("\r\n");
}

static const char help[] =
    "status              show selection, detected hosts, settings\r\n"
    "a | b | toggle      switch HDMI and USB together\r\n"
    "hdmi a|b, usb a|b   switch one path only\r\n"
    "bright 0-255        LED brightness\r\n"
    "color a|b RRGGBB    channel color\r\n"
    "auto on|off         follow the other host when the active one leaves\r\n"
    "anim on|off         wave animation on switch\r\n"
    "animms 100-5000     wave duration\r\n"
    "defaults            restore default settings\r\n"
    "reboot\r\n"
    "Settings are saved to flash automatically.\r\n";

static bool parse_u32(const char *s, uint32_t *out, uint32_t base)
{
    uint32_t v = 0;
    if (!s || !*s)
        return false;
    for (; *s; s++) {
        uint32_t d;
        if (*s >= '0' && *s <= '9')       d = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
        else return false;
        if (d >= base)
            return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}

// "a"/"b" -> 0/1, -1 if neither
static int parse_ch(const char *s)
{
    if (s && (!strcmp(s, "a") || !strcmp(s, "A"))) return CH_A;
    if (s && (!strcmp(s, "b") || !strcmp(s, "B"))) return CH_B;
    return -1;
}

// "on"/"off" -> 1/0, -1 if neither
static int parse_onoff(const char *s)
{
    if (s && !strcmp(s, "on"))  return 1;
    if (s && !strcmp(s, "off")) return 0;
    return -1;
}

static bool execute(int argc, char **argv)
{
    const char *cmd = argv[0], *a1 = argc > 1 ? argv[1] : 0, *a2 = argc > 2 ? argv[2] : 0;
    uint32_t v;
    int ch, on;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        uart_puts(help);
    } else if (!strcmp(cmd, "status")) {
        cmd_status();
    } else if ((ch = parse_ch(cmd)) >= 0 && argc == 1) {
        kvm_select((uint8_t)ch, (uint8_t)ch);
    } else if (!strcmp(cmd, "toggle")) {
        uint8_t n = !kvm_hdmi();
        kvm_select(n, n);
    } else if (!strcmp(cmd, "hdmi") && (ch = parse_ch(a1)) >= 0) {
        kvm_select((uint8_t)ch, kvm_usb());
    } else if (!strcmp(cmd, "usb") && (ch = parse_ch(a1)) >= 0) {
        kvm_select(kvm_hdmi(), (uint8_t)ch);
    } else if (!strcmp(cmd, "bright") && parse_u32(a1, &v, 10) && v <= 255) {
        g_set.brightness = (uint8_t)v;
        leds_flash();
    } else if (!strcmp(cmd, "color") && (ch = parse_ch(a1)) >= 0 &&
               a2 && strlen(a2) == 6 && parse_u32(a2, &v, 16)) {
        g_set.color[ch] = (rgb_t){ (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        leds_connect((uint8_t)ch);      // preview at that host's end
    } else if (!strcmp(cmd, "auto") && (on = parse_onoff(a1)) >= 0) {
        g_set.auto_switch = (uint8_t)on;
    } else if (!strcmp(cmd, "anim") && (on = parse_onoff(a1)) >= 0) {
        g_set.anim = (uint8_t)on;
    } else if (!strcmp(cmd, "animms") && parse_u32(a1, &v, 10) && v >= 100 && v <= 5000) {
        g_set.anim_ms = (uint16_t)v;
    } else if (!strcmp(cmd, "defaults")) {
        uint8_t keep = g_set.channel;
        settings_defaults();
        g_set.channel = keep;
    } else if (!strcmp(cmd, "reboot")) {
        if (!settings_save())
            cli_log("settings: flash write failed");
        NVIC_SystemReset();
    } else {
        return false;
    }
    settings_touch();
    return true;
}

static void process_line(void)
{
    char *argv[ARGS_MAX];
    int argc = 0;
    char *p = line;

    while (*p && argc < (int)ARGS_MAX) {
        while (*p == ' ')
            *p++ = 0;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }
    if (argc == 0)
        return;
    if (execute(argc, argv))
        cli_log("ok");
    else
        cli_log("? (type 'help')");
}

void cli_task(void)
{
    int c;
    while ((c = uart_getc()) >= 0) {
        if (c == '\r' || c == '\n') {
            if (len == 0 && c == '\n')
                continue;               // second half of CRLF
            uart_puts("\r\n");
            line[len] = 0;
            process_line();
            len = 0;
            uart_puts("> ");
        } else if (c == 0x08 || c == 0x7F) {
            if (len) {
                len--;
                uart_puts("\b \b");
            }
        } else if (c >= ' ' && len < LINE_MAX - 1) {
            line[len++] = (char)c;
            uart_putc((char)c);
        }
    }
}
