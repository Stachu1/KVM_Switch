/* SK6812MINI-E chain driven by SPI1 MOSI, plus status / wave rendering.
 *
 * SPI runs at 48 MHz / 16 = 3 MHz, so one SPI bit is 333 ns and each LED bit
 * is sent as four SPI bits: 0 -> 1000 (333 ns high), 1 -> 1100 (667 ns high).
 * Every pattern ends low, so a short FIFO underrun (an interrupt) only
 * stretches a low phase and does not corrupt the data. */
#include "board.h"
#include "kvm.h"

#define FRAME_MS   10u
#define MID_LEVEL  140u   /* middle LEDs relative to the active end LED */
#define FAR_LEVEL  70u    /* the other host's end LED when it is present */
#define BREATH_MS  2400u

static rgb_t shown[LED_COUNT];    /* last rendered frame, before brightness */
static rgb_t from[LED_COUNT];     /* frame at the moment a switch started   */
static uint32_t anim_t0;
static int8_t anim_dir;           /* +1: wave runs A->B, -1: B->A, 0: idle  */
static uint32_t last_frame;

static void spi_byte(uint8_t b)
{
    while (!(SPI1->SR & SPI_SR_TXE))
        ;
    *(volatile uint8_t *)&SPI1->DR = b;
}

static void show(const rgb_t *px)
{
    static const uint8_t pat[4] = { 0x88, 0x8C, 0xC8, 0xCC };
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
    /* Line idles low afterwards; the next frame is >80 us away (latch). */
}

static uint8_t lerp8(uint8_t a, uint8_t b, uint32_t t)  /* t = 0..256 */
{
    return (uint8_t)(a + (((int32_t)b - a) * (int32_t)t >> 8));
}

static rgb_t lerp(rgb_t a, rgb_t b, uint32_t t)
{
    return (rgb_t){ lerp8(a.r, b.r, t), lerp8(a.g, b.g, t), lerp8(a.b, b.b, t) };
}

static rgb_t scale(rgb_t c, uint32_t s)  /* s = 0..256 */
{
    return lerp((rgb_t){ 0, 0, 0 }, c, s);
}

/* Steady status picture for the current selection. */
static void render_status(rgb_t *out)
{
    uint8_t ch = kvm_hdmi();
    uint32_t near = ch == CH_A ? 0 : LED_COUNT - 1;
    uint32_t far = LED_COUNT - 1 - near;
    rgb_t c = g_set.color[ch];

    /* Breathe while the selected host has nothing connected. */
    uint32_t level = 256;
    if (!host_present(ch)) {
        uint32_t p = g_ms % BREATH_MS;
        uint32_t tri = p < BREATH_MS / 2 ? p : BREATH_MS - p;  /* 0..1200 */
        level = 48 + tri * 208 / (BREATH_MS / 2);
    }

    for (uint32_t i = 0; i < LED_COUNT; i++)
        out[i] = scale(c, (i == near ? 256 : MID_LEVEL) * level >> 8);

    /* USB routed to the other host (set from the CLI): mark the centre. */
    if (kvm_usb() != ch)
        out[LED_COUNT / 2] = scale(g_set.color[!ch], MID_LEVEL);

    out[far] = host_present(!ch) ? scale(g_set.color[!ch], FAR_LEVEL)
                                 : (rgb_t){ 0, 0, 0 };
}

/* Wave: a bright head runs from the old host's end to the new host's end,
 * the old picture crossfades into the new one behind it. */
static bool render_wave(rgb_t *out)
{
    uint32_t t = g_ms - anim_t0;
    if (t >= g_set.anim_ms)
        return false;

    rgb_t to[LED_COUNT];
    render_status(to);
    rgb_t head = g_set.color[kvm_hdmi()];
    head = lerp(head, (rgb_t){ 255, 255, 255 }, 80);

    /* Head position in 1/256 LED steps, from one LED before the first to
     * one LED past the last so it enters and leaves the bar smoothly. */
    int32_t span = (LED_COUNT + 1) * 256;
    int32_t pos = (int32_t)(t * (uint32_t)span / g_set.anim_ms) - 256;

    for (uint32_t i = 0; i < LED_COUNT; i++) {
        int32_t d = anim_dir > 0 ? (int32_t)i : (int32_t)(LED_COUNT - 1 - i);
        int32_t diff = pos - d * 256;
        uint32_t mix = diff <= 0 ? 0 : diff >= 256 ? 256 : (uint32_t)diff;
        rgb_t px = lerp(from[i], to[i], mix);

        int32_t ad = diff < 0 ? -diff : diff;
        if (ad < 384)
            px = lerp(px, head, (uint32_t)(384 - ad) * 256 / 384);
        out[i] = px;
    }
    return true;
}

void leds_switch_anim(uint8_t from_ch, uint8_t to_ch)
{
    if (!g_set.anim || from_ch == to_ch)
        return;
    for (uint32_t i = 0; i < LED_COUNT; i++)
        from[i] = shown[i];
    anim_dir = to_ch == CH_B ? 1 : -1;
    anim_t0 = g_ms;
}

void leds_task(void)
{
    if (g_ms - last_frame < FRAME_MS)
        return;
    last_frame = g_ms;

    if (anim_dir == 0 || !render_wave(shown)) {
        anim_dir = 0;
        render_status(shown);
    }

    /* Gamma ~2 then global brightness. */
    rgb_t out[LED_COUNT];
    uint32_t br = g_set.brightness + 1u;
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        out[i].r = (uint8_t)((((uint32_t)shown[i].r * shown[i].r + 255) >> 8) * br >> 8);
        out[i].g = (uint8_t)((((uint32_t)shown[i].g * shown[i].g + 255) >> 8) * br >> 8);
        out[i].b = (uint8_t)((((uint32_t)shown[i].b * shown[i].b + 255) >> 8) * br >> 8);
    }
    show(out);
}

void leds_init(void)
{
    RCC->APBENR2 |= RCC_APBENR2_SPI1EN;
    pa_af(PIN_LED, 0);

    /* Master, transmit-only (bidirectional output), 8-bit, fPCLK/16. */
    SPI1->CR1 = SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE | SPI_CR1_MSTR |
                SPI_CR1_SSM | SPI_CR1_SSI | (3u << SPI_CR1_BR_Pos);
    SPI1->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
    SPI1->CR1 |= SPI_CR1_SPE;

    /* Power-up wave from the A end into the restored channel. */
    for (uint32_t i = 0; i < LED_COUNT; i++)
        shown[i] = (rgb_t){ 0, 0, 0 };
    show(shown);
    if (g_set.anim) {
        anim_dir = kvm_hdmi() == CH_B ? 1 : -1;
        anim_t0 = g_ms;
    }
}
