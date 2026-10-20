// SK6812MINI-E chain driven by SPI1 MOSI, plus event animations.
//
// SPI runs at 48 MHz / 16 = 3 MHz, so one SPI bit is 333 ns and each LED bit
// is sent as four SPI bits: 0 -> 1000 (333 ns high), 1 -> 1100 (667 ns high).
// Between frames the pin is a GPIO held low (see show()).
//
// The bar is dark at rest. It only lights for events, and every effect fades
// out on its own:
//  - switch:     a head runs from the old host's end to the new host's end,
//                filling the bar with the new color, then the bar fades out
//  - connect:    a soft pulse at the host's end when its HDMI or USB appears
//  - brightness: the whole bar flashes at the new level and fades
// Effects can overlap; the brightest value per channel wins.
#include "board.h"
#include "kvm.h"

#define FRAME_MS      10u
#define TRAIL_LEVEL   110u   // bar behind the travelling head
#define HEAD_WIDTH    384    // head glow half-width, 1/256 LED steps
#define FADE_MS       900u   // switch: fade-out after the head arrives
#define CONNECT_RISE  120u   // connect: fade-in
#define CONNECT_MS    1400u  // connect: total length
#define FLASH_MS      800u   // brightness flash

static const rgb_t BLACK = { 0, 0, 0 };
static const rgb_t WHITE = { 255, 255, 255 };

static uint32_t last_frame;

static bool sw_on;
static uint32_t sw_t0;
static uint8_t sw_to;
static rgb_t sw_from;            // color the head starts with

static bool conn_on[2];
static uint32_t conn_t0[2];

static bool flash_on;
static uint32_t flash_t0;

static void spi_byte(uint8_t b)
{
    while (!(SPI1->SR & SPI_SR_TXE))
        ;
    *(volatile uint8_t *)&SPI1->DR = b;
}

// MOSI is not guaranteed to idle low between transfers, and the first LED
// only restarts on a >80 us low. So PA2 is a GPIO driven low except while a
// frame is being sent. Each frame starts with a 0x00 byte: the pin is handed
// to the SPI while that byte is shifting out, so the switch happens on a low
// level. Interrupts are held off for the frame (~160 us) so a FIFO underrun
// can't leave the line in an undefined state mid-frame.
static void show(const rgb_t *px)
{
    static const uint8_t pat[4] = { 0x88, 0x8C, 0xC8, 0xCC };

    __disable_irq();
    spi_byte(0x00);
    pa_mode(PIN_LED, MODE_AF);
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        const uint8_t grb[3] = { px[i].g, px[i].r, px[i].b };
        for (uint32_t c = 0; c < 3; c++) {
            uint8_t v = grb[c];
            spi_byte(pat[v >> 6]);
            spi_byte(pat[(v >> 4) & 3]);
            spi_byte(pat[(v >> 2) & 3]);
            spi_byte(pat[v & 3]);
        }
    }
    while ((SPI1->SR & SPI_SR_FTLVL) || (SPI1->SR & SPI_SR_BSY))
        ;
    pa_mode(PIN_LED, MODE_OUT);     // ODR is 0: hold low -> LEDs latch
    __enable_irq();
}

static uint8_t lerp8(uint8_t a, uint8_t b, uint32_t t)  // t = 0..256
{
    return (uint8_t)(a + (((int32_t)b - a) * (int32_t)t >> 8));
}

static rgb_t lerp(rgb_t a, rgb_t b, uint32_t t)
{
    return (rgb_t){ lerp8(a.r, b.r, t), lerp8(a.g, b.g, t), lerp8(a.b, b.b, t) };
}

static rgb_t scale(rgb_t c, uint32_t s)  // s = 0..256
{
    return lerp(BLACK, c, s);
}

static void add(rgb_t *dst, rgb_t c)     // per-channel max
{
    if (c.r > dst->r) dst->r = c.r;
    if (c.g > dst->g) dst->g = c.g;
    if (c.b > dst->b) dst->b = c.b;
}

// Distance of LED i from host ch's end of the bar (D2 = A end, D6 = B end).
static uint32_t dist_from(uint8_t ch, uint32_t i)
{
    return ch == CH_A ? i : LED_COUNT - 1 - i;
}

// Head travels from the old host's end to the new one, leaving the new color
// behind it, then the whole bar fades out. With the animation turned off only
// the fade-out of the filled bar is shown.
static void render_switch(rgb_t *out)
{
    uint32_t travel = g_set.anim ? g_set.anim_ms : 0;
    uint32_t t = g_ms - sw_t0;
    if (t >= travel + FADE_MS) {
        sw_on = false;
        return;
    }

    rgb_t to = g_set.color[sw_to];
    int32_t end = (LED_COUNT - 1) * 256;
    int32_t pos = t < travel ? (int32_t)(t * (uint32_t)end / travel) : end;
    uint32_t fade = t < travel ? 256 : 256 - (t - travel) * 256 / FADE_MS;
    rgb_t head = lerp(lerp(sw_from, to, (uint32_t)pos * 256 / (uint32_t)end), WHITE, 64);

    for (uint32_t i = 0; i < LED_COUNT; i++) {
        int32_t diff = pos - (int32_t)dist_from((uint8_t)!sw_to, i) * 256;
        rgb_t px = BLACK;
        if (diff > 0)
            px = scale(to, TRAIL_LEVEL * (uint32_t)(diff > 256 ? 256 : diff) >> 8);
        int32_t ad = diff < 0 ? -diff : diff;
        if (ad < HEAD_WIDTH)
            px = lerp(px, head, (uint32_t)(HEAD_WIDTH - ad) * 256 / HEAD_WIDTH);
        add(&out[i], scale(px, fade));
    }
}

// Soft pulse at the host's end, spilling a little into the bar.
static void render_connect(rgb_t *out, uint8_t ch)
{
    static const uint16_t spread[] = { 256, 96, 24 };
    uint32_t t = g_ms - conn_t0[ch];
    if (t >= CONNECT_MS) {
        conn_on[ch] = false;
        return;
    }

    uint32_t level = t < CONNECT_RISE
        ? t * 256 / CONNECT_RISE
        : 256 - (t - CONNECT_RISE) * 256 / (CONNECT_MS - CONNECT_RISE);
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        uint32_t d = dist_from(ch, i);
        if (d < sizeof spread / sizeof spread[0])
            add(&out[i], scale(g_set.color[ch], level * spread[d] >> 8));
    }
}

// Whole bar in the active color at full level, fading out.
static void render_flash(rgb_t *out)
{
    uint32_t t = g_ms - flash_t0;
    if (t >= FLASH_MS) {
        flash_on = false;
        return;
    }
    rgb_t c = scale(g_set.color[kvm_hdmi()], 256 - t * 256 / FLASH_MS);
    for (uint32_t i = 0; i < LED_COUNT; i++)
        add(&out[i], c);
}

void leds_switch_anim(uint8_t from_ch, uint8_t to_ch)
{
    if (from_ch == to_ch)
        return;
    sw_from = g_set.color[from_ch];
    sw_to = to_ch;
    sw_t0 = g_ms;
    sw_on = true;
}

void leds_connect(uint8_t ch)
{
    conn_t0[ch] = g_ms;
    conn_on[ch] = true;
}

void leds_flash(void)
{
    flash_t0 = g_ms;
    flash_on = true;
}

void leds_task(void)
{
    if (g_ms - last_frame < FRAME_MS)
        return;
    last_frame = g_ms;

    rgb_t px[LED_COUNT];
    for (uint32_t i = 0; i < LED_COUNT; i++)
        px[i] = BLACK;

    if (sw_on)
        render_switch(px);
    for (uint8_t ch = CH_A; ch <= CH_B; ch++)
        if (conn_on[ch])
            render_connect(px, ch);
    if (flash_on)
        render_flash(px);

    // Gamma ~2 then global brightness.
    uint32_t br = g_set.brightness + 1u;
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        px[i].r = (uint8_t)((((uint32_t)px[i].r * px[i].r + 255) >> 8) * br >> 8);
        px[i].g = (uint8_t)((((uint32_t)px[i].g * px[i].g + 255) >> 8) * br >> 8);
        px[i].b = (uint8_t)((((uint32_t)px[i].b * px[i].b + 255) >> 8) * br >> 8);
    }
    show(px);
}

void leds_init(void)
{
    RCC->APBENR2 |= RCC_APBENR2_SPI1EN;
    // Keep the data line low while the SPI isn't driving it, and use the
    // fastest edges so short high pulses keep their width at the LED.
    pa_pull(PIN_LED, PULL_DOWN);
    GPIOA->OSPEEDR |= 3u << (PIN_LED * 2);
    pa_af(PIN_LED, 0);                  // select SPI1_MOSI for show()
    pa_write(PIN_LED, 0);
    pa_mode(PIN_LED, MODE_OUT);         // idle: driven low

    // Master, transmit-only (bidirectional output), 8-bit, fPCLK/16.
    SPI1->CR1 = SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE | SPI_CR1_MSTR |
                SPI_CR1_SSM | SPI_CR1_SSI | (3u << SPI_CR1_BR_Pos);
    SPI1->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
    SPI1->CR1 |= SPI_CR1_SPE;

    const rgb_t off[LED_COUNT] = { 0 };
    show(off);

    // Power-up: run the switch effect into the restored channel, starting
    // from the other end in the same color.
    uint8_t ch = kvm_hdmi();
    leds_switch_anim(!ch, ch);
    sw_from = g_set.color[ch];
}
