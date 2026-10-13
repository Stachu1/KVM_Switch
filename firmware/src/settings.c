/* Settings stored as an append-only log of 16-byte records in the last
 * flash page. The newest valid record wins; the page is erased only when it
 * is full (128 records), which keeps wear negligible even though the last
 * selected channel is saved on every switch. */
#include <string.h>
#include "board.h"
#include "kvm.h"

#define REC_MAGIC  0x4B56u   /* "KV" */
#define REC_SLOTS  (SETTINGS_PAGE_SIZE / sizeof(record_t))

typedef struct {
    uint16_t   magic;
    uint16_t   sum;
    settings_t s;
} record_t;

_Static_assert(sizeof(settings_t) == 12, "settings_t layout changed");
_Static_assert(sizeof(record_t) == 16, "record must be two flash double-words");

#define FLASH_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | \
                      FLASH_SR_PGAERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR | \
                      FLASH_SR_MISERR | FLASH_SR_FASTERR)

settings_t g_set;

static const record_t *const slots = (const record_t *)SETTINGS_ADDR;

void settings_defaults(void)
{
    g_set = (settings_t){
        .channel = CH_A,
        .brightness = 32,
        .auto_switch = 1,
        .anim = 1,
        .anim_ms = 600,
        .color = { { 0, 140, 255 }, { 255, 70, 0 } },
    };
}

static uint16_t checksum(const settings_t *s)
{
    /* Fletcher-16 */
    const uint8_t *p = (const uint8_t *)s;
    uint16_t a = 0, b = 0;
    for (uint32_t i = 0; i < sizeof *s; i++) {
        a = (a + p[i]) % 255;
        b = (b + a) % 255;
    }
    return (uint16_t)(b << 8 | a);
}

static bool valid(const record_t *r)
{
    return r->magic == REC_MAGIC && r->sum == checksum(&r->s) &&
           r->s.channel <= CH_B && r->s.anim_ms >= 100 && r->s.anim_ms <= 5000;
}

static bool erased(const record_t *r)
{
    const uint32_t *w = (const uint32_t *)r;
    return (w[0] & w[1] & w[2] & w[3]) == 0xFFFFFFFFu;
}

static const record_t *latest(void)
{
    const record_t *last = 0;
    for (uint32_t i = 0; i < REC_SLOTS && !erased(&slots[i]); i++)
        if (valid(&slots[i]))
            last = &slots[i];
    return last;
}

void settings_load(void)
{
    const record_t *r = latest();
    if (r)
        g_set = r->s;
    else
        settings_defaults();
}

/* ---- flash primitives -------------------------------------------------- */

static bool flash_wait(void)
{
    while (FLASH->SR & (FLASH_SR_BSY1 | FLASH_SR_CFGBSY))
        ;
    uint32_t err = FLASH->SR & FLASH_ERRORS;
    FLASH->SR = err | FLASH_SR_EOP;
    return err == 0;
}

static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static bool flash_erase_page(uint32_t page)
{
    flash_wait();
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB) | (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER;
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = flash_wait();
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
    return ok;
}

static bool flash_write(uint32_t addr, const uint32_t *w, uint32_t dwords)
{
    bool ok = flash_wait();
    FLASH->CR |= FLASH_CR_PG;
    for (uint32_t i = 0; ok && i < dwords; i++, addr += 8, w += 2) {
        ((volatile uint32_t *)addr)[0] = w[0];
        ((volatile uint32_t *)addr)[1] = w[1];
        ok = flash_wait();
    }
    FLASH->CR &= ~FLASH_CR_PG;
    return ok;
}

bool settings_save(void)
{
    record_t rec;
    memset(&rec, 0xFF, sizeof rec);
    rec.magic = REC_MAGIC;
    rec.s = g_set;
    rec.sum = checksum(&rec.s);

    const record_t *last = latest();
    if (last && memcmp(last, &rec, sizeof rec) == 0)
        return true;

    uint32_t slot = 0;
    while (slot < REC_SLOTS && !erased(&slots[slot]))
        slot++;

    flash_unlock();
    bool ok = true;
    if (slot == REC_SLOTS) {
        ok = flash_erase_page(SETTINGS_PAGE);
        slot = 0;
    }
    uint32_t words[4];
    memcpy(words, &rec, sizeof words);
    if (ok)
        ok = flash_write((uint32_t)&slots[slot], words, 2);
    FLASH->CR |= FLASH_CR_LOCK;
    return ok;
}
