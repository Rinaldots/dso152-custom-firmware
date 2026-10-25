/*
 * zerocal.c — auto-calibracao do zero, como a do firmware original: com a
 * ponta em curto (0 V), mede a media do ADC em cada faixa, em DC e em AC, e
 * grava na flash.
 *
 * A pagina da flash e so nossa (0x0800F000, 1 KB), fora do app (o linker para
 * antes dela) e longe da area de configuracoes do original (0x0800FE00), para
 * nao estragar nada caso se volte a ele. Apagar e programar seguem o StdPeriph
 * da WCH que o original usa, inclusive a leitura de endereco ^ 0x1000 gravada
 * em FLASH+0x34 depois de cada operacao.
 */
#include <stdint.h>
#include "ch32f103.h"
#include "gain_cal.h"
#include "scope.h"
#include "zerocal.h"

uint32_t sys_ms(void);

#define ZC_ADDR    0x0800F000u
#define ZC_MAGIC   0x5A43414Cu        /* "LACZ" */
#define N_GAIN     11u
#define RATE       50000u             /* 1024 amostras = 20 ms */
#define SETTLE_DC  100u               /* ms depois de trocar faixa (rele) */
#define SETTLE_AC  300u               /* ms em AC: capacitor de acoplamento */
#define MAX_PP     300                /* mais que isso na ponta em curto = sinal */
#define ZERO_MIN   1024
#define ZERO_MAX   3072

typedef struct {
    uint32_t magic;
    int16_t  dc[N_GAIN];
    int16_t  ac[N_GAIN];
    uint16_t check;
    uint16_t pad;
} zc_flash_t;

static int16_t z_dc[N_GAIN], z_ac[N_GAIN];
static int     from_flash;

static uint16_t checksum(const int16_t *dc, const int16_t *ac)
{
    uint16_t s = 0x5A5Au;
    for (unsigned i = 0; i < N_GAIN; i++) s = (uint16_t)(s + (uint16_t)dc[i] * 3u + (uint16_t)ac[i]);
    return s;
}

static int plausible(const int16_t *z)
{
    for (unsigned i = 0; i < N_GAIN; i++) {
        if (z[i] < ZERO_MIN || z[i] > ZERO_MAX) return 0;
    }
    return 1;
}

void zerocal_load(void)
{
    for (unsigned i = 0; i < N_GAIN; i++) {
        z_dc[i] = GAIN_CAL[i].off_dc;
        z_ac[i] = GAIN_CAL[i].off_ac;
    }
    const zc_flash_t *f = (const zc_flash_t *)ZC_ADDR;
    from_flash = 0;
    if (f->magic != ZC_MAGIC || f->check != checksum(f->dc, f->ac)) return;
    if (!plausible(f->dc) || !plausible(f->ac)) return;
    for (unsigned i = 0; i < N_GAIN; i++) {
        z_dc[i] = f->dc[i];
        z_ac[i] = f->ac[i];
    }
    from_flash = 1;
}

int zerocal_from_flash(void)
{
    return from_flash;
}

int zero_offset(unsigned gain, int ac)
{
    if (gain >= N_GAIN) gain = N_GAIN - 1u;
    return ac ? z_ac[gain] : z_dc[gain];
}

/* ------------------------------- flash -------------------------------- */
static int flash_wait(void)
{
    for (uint32_t t = 0; FLASH->SR & FLASH_SR_BSY; t++) {
        if (t > 0x200000u) return 0;
    }
    return (FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) == 0u;
}

static void flash_ch32_sync(uint32_t addr)
{
    FLASH_CH32_REG34 = *(volatile uint32_t *)(addr ^ 0x1000u);
}

static int flash_save(const zc_flash_t *z)
{
    int ok = 1;
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;   /* limpa (rc_w1) */

    /* apaga a pagina */
    if (flash_wait()) {
        FLASH->CR |= FLASH_CR_PER;
        FLASH->AR = ZC_ADDR;
        FLASH->CR |= FLASH_CR_STRT;
        ok = flash_wait();
        FLASH->CR &= ~FLASH_CR_PER;
    } else {
        ok = 0;
    }
    flash_ch32_sync(ZC_ADDR);

    /* programa de meia em meia palavra */
    const uint16_t *src = (const uint16_t *)z;
    volatile uint16_t *dst = (volatile uint16_t *)ZC_ADDR;
    for (unsigned i = 0; ok && i < sizeof *z / 2u; i++) {
        FLASH->CR |= FLASH_CR_PG;
        dst[i] = src[i];
        ok = flash_wait();
        FLASH->CR &= ~FLASH_CR_PG;
        flash_ch32_sync((uint32_t)&dst[i]);
    }
    FLASH->CR |= FLASH_CR_LOCK;

    /* confere o que ficou gravado */
    for (unsigned i = 0; ok && i < sizeof *z / 2u; i++) {
        if (dst[i] != src[i]) ok = 0;
    }
    return ok;
}

/* ------------------------------ medicao ------------------------------- */
static void wait_ms(uint32_t ms, void (*idle)(void))
{
    uint32_t t0 = sys_ms();
    while ((sys_ms() - t0) < ms) idle();
}

/* media e pico a pico de um bloco */
static int measure(uint16_t *buf, int *mean, int *pp, void (*idle)(void))
{
    scope_set_rate(RATE);
    scope_start(buf, ZC_SAMPLES);
    uint32_t t0 = sys_ms();
    while (!scope_done()) {
        idle();
        if ((sys_ms() - t0) > 200u) { scope_stop(); return 0; }
    }
    scope_stop();
    uint32_t sum = 0;
    int lo = 4095, hi = 0;
    for (unsigned i = 0; i < ZC_SAMPLES; i++) {
        sum += buf[i];
        if (buf[i] < lo) lo = buf[i];
        if (buf[i] > hi) hi = buf[i];
    }
    *mean = (int)((sum + ZC_SAMPLES / 2u) / ZC_SAMPLES);
    *pp = hi - lo;
    return 1;
}

int zerocal_run(uint16_t *buf, void (*progress)(unsigned gain, int ac), void (*idle)(void))
{
    zc_flash_t z;
    z.magic = ZC_MAGIC;
    z.pad = 0xFFFFu;

    for (int ac = 0; ac <= 1; ac++) {
        scope_set_coupling(ac ? !GAIN_CAL_DC_LEVEL : GAIN_CAL_DC_LEVEL);
        for (unsigned g = 0; g < N_GAIN; g++) {
            progress(g, ac);
            scope_set_gain(g);
            wait_ms(ac ? SETTLE_AC : SETTLE_DC, idle);
            int mean, pp;
            if (!measure(buf, &mean, &pp, idle)) return ZC_ADC;
            if (pp > MAX_PP) return ZC_SIGNAL;
            if (mean < ZERO_MIN || mean > ZERO_MAX) return ZC_RANGE;
            if (ac) z.ac[g] = (int16_t)mean;
            else    z.dc[g] = (int16_t)mean;
        }
    }
    z.check = checksum(z.dc, z.ac);
    if (!flash_save(&z)) return ZC_FLASH;
    zerocal_load();
    return zerocal_from_flash() ? ZC_OK : ZC_FLASH;
}
