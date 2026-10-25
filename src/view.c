/*
 * view.c — tela de osciloscopio no LCD, no mesmo layout do firmware original:
 * area do sinal de 300x200 px em (10,20), 12x8 divisoes de 25 px (25 amostras
 * por divisao), uma linha de texto em cima (V/div, t/div, trigger, RUN) e
 * outra embaixo (Vpp, frequencia, posicao, AC/DC e um ponto de USB).
 *
 * Escala vertical pela calibracao de include/gain_cal.h, nos V/div do original
 * (10 mV .. 20 V por divisao), e zero de cada faixa por zerocal (auto-
 * calibracao gravada na flash, ou o de gain_cal.h). O 0 V fica pos_px acima da linha do meio, com um
 * marcador na margem esquerda; o nivel de trigger fica trig_px acima do 0 V,
 * com um marcador na margem direita.
 *
 * O desenho e incremental (algumas colunas por chamada) para nao travar o
 * USB, e cada coluna so reescreve as linhas que o traco antigo ou o novo
 * ocupam: PC13..PC15 (RS/WR/RD) sao pinos lentos, entao pixel economizado
 * e tempo economizado.
 */
#include <stdint.h>
#include "gain_cal.h"
#include "lcd.h"
#include "view.h"
#include "zerocal.h"

#define X0 10u
#define Y0 20u
#define W  VIEW_COLS
#define H  200u
#define MID       ((int)H / 2)
#define DIV       25u      /* pixels (e amostras) por divisao */
#define TRIG_POS  DIV      /* trigger a 1 divisao da borda esquerda */
#define STEP_COLS 10u
#define NONE8     0xFFu

#define C_BG     0x0000u
#define C_GRID   RGB565(64, 64, 64)
#define C_BORDER RGB565(128, 128, 128)
#define C_WAVE   RGB565(255, 255, 0)
#define C_TEXT   RGB565(255, 255, 255)
#define C_CYAN   RGB565(0, 255, 255)
#define C_GREEN  RGB565(0, 255, 0)
#define C_RED    RGB565(255, 0, 0)
#define C_ORANGE RGB565(255, 140, 0)
#define C_DIM    RGB565(90, 90, 90)

/* traco desenhado em cada coluna (linhas a..b), para apagar depois */
static uint8_t old_a[W], old_b[W];

/* bloco sendo desenhado e a escala dele */
static const uint16_t *w_s;
static uint32_t w_n, w_start, w_col;
static int      w_busy;
static int32_t  w_off, w_px_q16, w_pos;

/* ultimo bloco mostrado (para redesenhar parado) e ultimo analisado */
static const uint16_t *last_s;
static uint32_t last_n, last_start;
static unsigned last_gain;
static int      last_ac;
static int32_t  mid_px;

/* marcadores desenhados (linha da area, -1 = nenhum) e ponto de USB */
static int zero_row = -1, trig_row = -1, usb_shown = -1;

/* --------------------------- campos de texto --------------------------- */
enum { F_VDIV, F_TDIV, F_TRIG, F_RUN, F_VPP, F_FREQ, F_POS, F_CPL, F_COUNT };
static const struct { uint16_t x, y; uint8_t width; } FIELD[F_COUNT] = {
    [F_VDIV] = {   2,   2, 5 },
    [F_TDIV] = {  70,   2, 6 },
    [F_TRIG] = { 150,   2, 8 },
    [F_RUN]  = { 262,   2, 4 },
    [F_VPP]  = {   2, 222, 8 },
    [F_FREQ] = { 106, 222, 8 },
    [F_POS]  = { 208, 222, 6 },
    [F_CPL]  = { 286, 222, 2 },
};
static char     shown[F_COUNT][10];
static uint16_t shown_color[F_COUNT];
static uint8_t  shown_inv[F_COUNT];

/* texto num campo de largura fixa; inv = destacado (fundo na cor do texto) */
static void field(int f, const char *text, uint16_t color, int inv)
{
    char buf[10];
    unsigned i = 0;
    for (; text[i] && i < FIELD[f].width; i++) buf[i] = text[i];
    for (; i < FIELD[f].width; i++) buf[i] = ' ';
    buf[i] = 0;

    int same = (shown_color[f] == color) && (shown_inv[f] == (uint8_t)inv);
    for (i = 0; same && i <= FIELD[f].width; i++) same = (shown[f][i] == buf[i]);
    if (same) return;

    for (i = 0; i <= FIELD[f].width; i++) shown[f][i] = buf[i];
    shown_color[f] = color;
    shown_inv[f] = (uint8_t)inv;
    if (inv) lcd_text(FIELD[f].x, FIELD[f].y, buf, C_BG, color, 2);
    else     lcd_text(FIELD[f].x, FIELD[f].y, buf, color, C_BG, 2);
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static char *put_uint(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

/* inteiro + fracao com 'digits' casas, sem zeros a direita */
static char *put_fixed(char *p, uint32_t whole, uint32_t frac, unsigned digits)
{
    char d[3];
    p = put_uint(p, whole);
    for (int i = (int)digits - 1; i >= 0; i--) { d[i] = (char)('0' + frac % 10u); frac /= 10u; }
    while (digits && d[digits - 1u] == '0') digits--;
    if (digits) {
        *p++ = '.';
        for (unsigned i = 0; i < digits; i++) *p++ = d[i];
    }
    return p;
}

/* whole.milli com 3 algarismos significativos */
static char *put_3sig(char *p, uint32_t whole, uint32_t milli)
{
    if (whole >= 100u) return put_uint(p, whole);
    if (whole >= 10u)  return put_fixed(p, whole, milli / 100u, 1);
    return put_fixed(p, whole, milli / 10u, 2);
}

/* valor em uV como "12.3V" / "500mV" / "20mV" */
static char *put_volts(char *p, uint32_t uv)
{
    if (uv >= 1000000u) return put_str(put_3sig(p, uv / 1000000u, (uv % 1000000u) / 1000u), "V");
    return put_str(put_3sig(p, uv / 1000u, uv % 1000u), "mV");
}

/* com sinal; plus = escreve '+' nos positivos */
static char *put_svolts(char *p, int32_t uv, int plus)
{
    if (uv == 0) return put_str(p, "0V");
    if (uv < 0)   { *p++ = '-'; uv = -uv; }
    else if (plus) *p++ = '+';
    return put_volts(p, (uint32_t)uv);
}

/* ------------------------------- grade -------------------------------- */
static uint16_t grid_px(unsigned c, unsigned y)
{
    if (c == 0u || c == W - 1u || y == 0u || y == H - 1u) return C_BORDER;
    if (c % DIV == 0u) return (y % 5u == 0u) ? C_GRID : C_BG;
    if (y % DIV == 0u) return (c % 5u == 0u) ? C_GRID : C_BG;
    return C_BG;
}

/* reescreve as linhas r0..r1 da coluna c; o traco ocupa wa..wb (ou nada) */
static void draw_col(unsigned c, int r0, int r1, int wa, int wb)
{
    lcd_window(X0 + c, Y0 + (unsigned)r0, X0 + c, Y0 + (unsigned)r1);
    for (int y = r1; y >= r0; y--) {           /* de baixo para cima (ver lcd.h) */
        lcd_put((y >= wa && y <= wb) ? C_WAVE : grid_px(c, (unsigned)y));
    }
}

static int clamp_row(int y)
{
    return y < 0 ? 0 : (y > (int)H - 1 ? (int)H - 1 : y);
}

/* contagem do ADC -> linha na tela, com a escala e a posicao do bloco */
static int ypix(uint16_t v)
{
    return clamp_row(MID - w_pos - (int)(((int32_t)v - w_off) * w_px_q16 / 65536));
}

void view_init(void)
{
    for (unsigned c = 0; c < W; c++) {
        draw_col(c, 0, (int)H - 1, -1, -1);
        old_a[c] = NONE8;
        old_b[c] = 0;
    }
    for (int f = 0; f < F_COUNT; f++) shown_color[f] = 0xFFFFu - 1u;   /* forca redesenho */
    field(F_FREQ, "---", C_WAVE, 0);
    w_busy = 0;
    last_s = 0;
}

/* --------------------- painel de texto na area do sinal --------------------- */
void view_panel_clear(void)
{
    lcd_fill(X0, Y0, W, H, C_BG);
    for (unsigned c = 0; c < W; c++) {
        old_a[c] = NONE8;
        old_b[c] = 0;
    }
    w_busy = 0;
    last_s = 0;
}

void view_panel_line(unsigned line, const char *text, uint16_t color)
{
    char buf[25];
    unsigned i = 0;
    for (; text[i] && i < 24u; i++) buf[i] = text[i];
    for (; i < 24u; i++) buf[i] = ' ';
    buf[24] = 0;
    lcd_text(X0 + 6u, Y0 + 12u + line * 22u, buf, color, C_BG, 2);
}

/* ------------------------------- traco -------------------------------- */
static void start_draw(const uint16_t *s, uint32_t n, uint32_t start, unsigned gain, int ac, int pos)
{
    w_off = zero_offset(gain, ac);
    w_px_q16 = (int32_t)GAIN_CAL[gain].px_q16;
    w_pos = pos;
    w_s = s;
    w_n = n;
    w_start = start;
    w_col = 0;
    w_busy = 1;
    last_s = s;
    last_n = n;
    last_start = start;
    last_gain = gain;
    last_ac = ac;
}

int view_wave(const uint16_t *s, uint16_t n, uint32_t rate, const view_cfg_t *cfg)
{
    unsigned gain = cfg->gain > 10u ? 10u : cfg->gain;
    int32_t off = zero_offset(gain, cfg->ac);
    int32_t q = (int32_t)GAIN_CAL[gain].px_q16;

    int lo = 4095, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < lo) lo = s[i];
        if (s[i] > hi) hi = s[i];
    }
    mid_px = ((lo + hi) / 2 - off) * q / 65536;

    /* trigger no nivel pedido, com ~1 px de histerese */
    int32_t level = off + (int32_t)cfg->trig_px * 65536 / q;
    int32_t hyst = 65536 / q;
    if (hyst < 2) hyst = 2;
    int armed = 0, trig = -1;
    for (uint32_t i = 0; i < n && trig < 0; i++) {
        int32_t v = s[i];
        if (!cfg->falling) {
            if (v < level - hyst) armed = 1;
            else if (armed && v >= level) {
                if (i >= TRIG_POS && i + (W - TRIG_POS) <= n) trig = (int)i;
                armed = 0;
            }
        } else {
            if (v > level + hyst) armed = 1;
            else if (armed && v <= level) {
                if (i >= TRIG_POS && i + (W - TRIG_POS) <= n) trig = (int)i;
                armed = 0;
            }
        }
    }

    /* frequencia: bordas de subida pelo meio da amplitude, com histerese */
    int have_freq = 0;
    uint32_t f_int = 0, f_milli = 0;
    if (hi - lo >= 32) {
        int fl = (lo + hi) / 2, fh = (hi - lo) / 8;
        int high = s[0] > fl;
        uint32_t first = 0, last = 0, count = 0;
        for (uint32_t i = 1; i < n; i++) {
            if (high) {
                if (s[i] < fl - fh) high = 0;
            } else if (s[i] > fl + fh) {
                high = 1;
                if (count == 0u) first = i;
                last = i;
                count++;
            }
        }
        if (count >= 2u) {
            uint32_t num = (count - 1u) * rate, span = last - first;
            f_int = num / span;
            f_milli = (num % span) * 1000u / span;
            have_freq = 1;
        }
    }

    char txt[10], *p = txt;
    if (!have_freq)          p = put_str(p, "---");
    else if (f_int >= 1000u) p = put_str(put_3sig(p, f_int / 1000u, f_int % 1000u), "kHz");
    else                     p = put_str(put_3sig(p, f_int, f_milli), "Hz");
    *p = 0;
    field(F_FREQ, txt, C_WAVE, 0);

    p = put_str(put_volts(txt, (uint32_t)(hi - lo) * GAIN_CAL[gain].uv_count), "pp");
    *p = 0;
    field(F_VPP, txt, C_WAVE, 0);

    if (trig >= 0 || cfg->mode == TRIG_AUTO) {
        start_draw(s, n, trig >= 0 ? (uint32_t)trig - TRIG_POS : 0u, gain, cfg->ac, cfg->pos_px);
    }
    return trig >= 0;
}

void view_redraw(const view_cfg_t *cfg)
{
    if (last_s) start_draw(last_s, last_n, last_start, last_gain, last_ac, cfg->pos_px);
}

int view_busy(void)
{
    return w_busy;
}

int view_mid_px(void)
{
    return (int)mid_px;
}

void view_step(void)
{
    if (!w_busy) return;
    for (unsigned k = 0; k < STEP_COLS && w_col < W; k++, w_col++) {
        unsigned c = w_col;
        uint32_t i = w_start + c;
        int na = -1, nb = -1;
        if (i < w_n) {
            int y = ypix(w_s[i]);
            int yp = (c > 0u) ? ypix(w_s[i - 1u]) : y;
            na = y < yp ? y : yp;
            nb = y < yp ? yp : y;
        }

        int r0, r1;
        if (old_a[c] == NONE8) {
            if (na < 0) continue;              /* nada antes, nada agora */
            r0 = na;
            r1 = nb;
        } else {
            r0 = old_a[c];
            r1 = old_b[c];
            if (na >= 0) {
                if (na < r0) r0 = na;
                if (nb > r1) r1 = nb;
            }
        }
        draw_col(c, r0, r1, na, nb);
        old_a[c] = (na < 0) ? NONE8 : (uint8_t)na;
        old_b[c] = (uint8_t)(nb < 0 ? 0 : nb);
    }
    if (w_col >= W) w_busy = 0;
}

/* ----------------------------- marcadores ----------------------------- */
/* triangulo de 7 linhas na margem (esquerda aponta para a direita e vice-versa) */
static void marker(int *drawn, int row, int left, uint16_t color)
{
    if (row < 3) row = 3;
    if (row > (int)H - 4) row = (int)H - 4;
    if (*drawn == row) return;
    unsigned mx = left ? 0u : X0 + W;          /* margem de 10 px */
    if (*drawn >= 0) lcd_fill(mx, Y0 + (unsigned)*drawn - 3u, X0, 7, C_BG);
    for (int dy = -3; dy <= 3; dy++) {
        unsigned len = 7u - 2u * (unsigned)(dy < 0 ? -dy : dy);
        unsigned x = left ? 1u : X0 + W + 9u - len;
        lcd_fill(x, Y0 + (unsigned)(row + dy), len, 1, color);
    }
    *drawn = row;
}

/* ------------------------------- estado ------------------------------- */
void view_status(const view_cfg_t *cfg, uint32_t rate, int run, int usb, int sel)
{
    char txt[10], *p;
    unsigned gain = cfg->gain > 10u ? 10u : cfg->gain;
    int32_t uv_px = (int32_t)(GAIN_CAL[gain].vdiv_uv / DIV);    /* uV por pixel */

    p = put_volts(txt, GAIN_CAL[gain].vdiv_uv);
    *p = 0;
    field(F_VDIV, txt, C_TEXT, sel == SEL_VDIV);

    /* base de tempo = 25 amostras por divisao; v em decimos de us */
    uint32_t v = 250000000u / rate;
    if (v < 10000u)         p = put_str(put_fixed(txt, v / 10u, v % 10u, 1), "us");
    else if (v < 10000000u) p = put_str(put_fixed(txt, v / 10000u, (v % 10000u) / 100u, 2), "ms");
    else                    p = put_str(put_fixed(txt, v / 10000000u, (v % 10000000u) / 100000u, 2), "s");
    *p = 0;
    field(F_TDIV, txt, C_CYAN, sel == SEL_TDIV);

    p = txt;
    *p++ = cfg->mode == TRIG_NORMAL ? 'N' : (cfg->mode == TRIG_SINGLE ? 'S' : 'A');
    *p++ = cfg->falling ? '\\' : '/';
    p = put_svolts(p, cfg->trig_px * uv_px, 0);
    *p = 0;
    field(F_TRIG, txt, C_ORANGE, sel == SEL_TRIG);

    field(F_RUN, run == RUN_RUN ? "RUN" : (run == RUN_WAIT ? "WAIT" : "STOP"),
          run == RUN_RUN ? C_GREEN : (run == RUN_WAIT ? C_WAVE : C_RED), 0);

    p = put_svolts(txt, cfg->pos_px * uv_px, 1);
    *p = 0;
    field(F_POS, txt, C_CYAN, sel == SEL_POS);

    field(F_CPL, cfg->ac ? "AC" : "DC", C_TEXT, 0);

    if (usb != usb_shown) {                     /* ponto de USB no canto */
        lcd_fill(313, 227, 6, 6, usb == 2 ? C_GREEN : (usb == 1 ? C_TEXT : C_DIM));
        usb_shown = usb;
    }

    marker(&zero_row, MID - cfg->pos_px, 1, C_CYAN);
    marker(&trig_row, MID - cfg->pos_px - cfg->trig_px, 0, C_ORANGE);
}

void view_message(const char *msg)
{
    lcd_fill(0, 220, LCD_W, 20, C_BG);
    lcd_text(4, 222, msg, C_RED, C_BG, 2);
    for (int f = F_VPP; f < F_COUNT; f++) shown_color[f] = 0xFFFFu - 1u;
    usb_shown = -1;
}
