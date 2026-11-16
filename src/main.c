/*
 * main.c — firmware do DSO152: captura o sinal pelo ADC (PA1), mostra a forma
 * de onda no LCD e envia os frames ao PC pela USB-C como porta serial virtual
 * (CDC).
 *
 * Fluxo: captura um bloco de N amostras (TIM1_CC1 -> ADC1 -> DMA1_Ch1) num
 * buffer enquanto o bloco anterior vai pelo USB e e desenhado na tela. So
 * envia com a porta aberta no PC (DTR). Frame (little-endian), igual ao
 * pc/plot.py:
 *     A5 5A | n (u16) | faixa (u8) | taxa_hz (u32) | zero (i16) | amostras u16[n] | crc8
 * faixa: bits 0..6 = indice 0..10 (G0..G10), bit 7 = 1 em AC. zero: contagem
 * do ADC com 0 V na ponta nessa faixa (a da auto-calibracao, se houver). O
 * CRC-8 (poly 0x07, init 0) cobre de n ate a ultima amostra. Todo bloco
 * capturado vai pelo USB, mesmo os que nao disparam o trigger.
 *
 * Comandos do PC (texto, um por linha, terminados em \n ou \r):
 *     G<0..10>      faixa de ganho (indice da GAIN_TABLE)
 *     R<hz>         taxa de amostragem livre (10 .. 800000)
 *     B<i>          base de tempo da tela (indice de TB_RATE, 0 = 50 us/div)
 *     N<n>          amostras por frame (16 .. 2048)
 *     C<0|1>        nivel do PA3 (1 = DC, 0 = AC)
 *     T<0|1|2>      trigger AUTO / NORMAL / SINGLE      E<0|1> borda subida/descida
 *     L<px>         nivel do trigger acima do 0 V (25 px por divisao, com sinal)
 *     P<px>         posicao do 0 V acima do meio da tela (px, com sinal)
 *     S<0|1>        parar / rodar (no SINGLE, S1 arma de novo)
 *     A             trigger no meio do sinal (como o botao AUTO)
 *     D             tudo no padrao (como o AUTO longo)
 *     Z             mede e grava os zeros (ponta no jacare!)
 *     Q             pede o quadro de estado
 * Depois de cada comando, e sempre que algo muda pelos botoes, o aparelho
 * manda um quadro de estado (A5 5B, ver status_poll()).
 *
 * Botoes, no estilo do original (pinos do esquema do Atlan):
 *     roda esq. / dir.   escolhe o parametro: V/div, t/div, posicao, trigger
 *     CIMA / BAIXO       ajusta o parametro escolhido (CIMA = aumenta)
 *     OK (roda)          alterna AC / DC
 *     MODE               curto: trigger AUTO / NORMAL / SINGLE; longo: borda
 *     RUN                congela / continua (no SINGLE, arma de novo);
 *                        segurado por 2 s: auto-calibracao do zero
 *     AUTO               curto: trigger no meio do sinal; longo: tudo no padrao
 *     POWER              solto por 0,5 s e depois segurado por 3 s: desliga
 */
#include <stdint.h>
#include "ch32f103.h"
#include "dso152_pins.h"      /* LCD_BL_PIN (PC9), PWR_HOLD_PIN (PC10), PWR_BTN_PIN (PC11) */
#include "gain_cal.h"         /* GAIN_CAL_DC_LEVEL */
#include "lcd.h"
#include "scope.h"
#include "usb_cdc.h"
#include "view.h"
#include "zerocal.h"

/* de system.c */
int      sys_clock_hse_72mhz(void);
uint32_t sys_ms(void);

/* Numeros de pino (bit) a partir das mascaras GPIO_Pin_x do header de pinos. */
#define PIN_BL   9     /* PC9  backlight, ativo em 0 (GPIO_Pin_9)  */
#define PIN_HOLD 10    /* PC10 power-hold, ativo em 1 (GPIO_Pin_10) */
#define PIN_PWR  11    /* PC11 botao POWER, ativo em 0 (GPIO_Pin_11) */

#define TICK_MS     10u
#define ARM_MS      500u     /* POWER solto por este tempo arma o desligamento */
#define HOLD_OFF_MS 3000u    /* POWER apertado por este tempo desliga */
#define LONG_TICKS  80u      /* toque longo: 800 ms */
#define CAL_TICKS   200u     /* RUN segurado por 2 s: auto-calibracao */

#define N_MIN       16u
#define N_MAX       2048u
#define FRAME_HDR   11u      /* sync(2) + n(2) + faixa(1) + taxa(4) + zero(2) */

#define GAIN_DEFAULT 6u      /* 1 V/div, o padrao do original */
#define TB_DEFAULT   3       /* 500 us/div */
#define TRIG_DEFAULT 12      /* nivel de trigger: ~0,5 divisao acima do 0 V */

/* Bases de tempo do original (1-2-5) com 25 amostras por divisao, entao
 * taxa = 25 / (t/div). As de 10 e 20 us/div pediriam 2,5 e 1,25 MS/s, acima
 * do ADC a 12 MHz; as mais lentas que 100 ms/div ficariam lentas demais por
 * bloco (o original usa modo rolagem nelas). */
static const uint32_t TB_RATE[] = {
    500000, 250000, 125000, 50000, 25000, 12500,   /* 50 us .. 2 ms/div */
    5000, 2500, 1250, 500, 250,                    /* 5 ms .. 100 ms/div */
};
#define N_TB ((int)(sizeof TB_RATE / sizeof TB_RATE[0]))

/* Dois buffers de frame: enquanto um vai pelo USB e para a tela, o outro
 * captura. O cabecalho comeca no byte 1, para as amostras (byte 12) ficarem
 * alinhadas em 16 bits, como o DMA precisa. */
#define FRAME_WORDS ((1u + FRAME_HDR + 2u * N_MAX + 1u + 1u) / 2u)
static uint16_t frame_mem[2][FRAME_WORDS];

/* configuracao atual (PC ou botoes) */
static view_cfg_t cfg = { GAIN_DEFAULT, 0, 0, TRIG_DEFAULT, 0, TRIG_AUTO };
static int      sel = SEL_VDIV;               /* parametro que CIMA/BAIXO ajustam */
static int      tb = TB_DEFAULT;              /* -1 = taxa livre pedida pelo PC */
static uint32_t rate_req = 50000u;
static uint16_t n_samples = 600u;             /* 12 divisoes na tela + folga para o trigger */
static int      running = 1;
static int      waiting;                      /* NORMAL/SINGLE sem disparo no ultimo bloco */
static int      restart;                      /* refazer a captura com a nova config */

enum { CAL_OFF, CAL_ASK, CAL_RUN };           /* pagina de auto-calibracao */
static int      cal_state;
static int8_t   cal_result = -1;              /* ultimo zerocal_run(), -1 = nenhum */

static uint8_t crc_table[256];

static void crc8_init(void)
{
    for (unsigned i = 0; i < 256u; i++) {
        uint8_t c = (uint8_t)i;
        for (int k = 0; k < 8; k++) {
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
        }
        crc_table[i] = c;
    }
}

static uint8_t crc8(const uint8_t *p, uint32_t n)
{
    uint8_t c = 0;
    while (n--) c = crc_table[c ^ *p++];
    return c;
}

static uint8_t *frame_bytes(int b)
{
    return (uint8_t *)frame_mem[b] + 1;   /* inicio do frame (sync) */
}

static uint16_t *frame_samples(int b)
{
    return &frame_mem[b][6];              /* byte 12 do buffer = byte 11 do frame */
}

/* Preenche cabecalho e CRC; devolve o tamanho do frame em bytes. */
static uint32_t frame_finish(int b, uint16_t n, uint8_t g, int is_ac, uint32_t rate)
{
    uint8_t *f = frame_bytes(b);
    int16_t zero = (int16_t)zero_offset(g, is_ac);
    f[0] = 0xA5;
    f[1] = 0x5A;
    f[2] = (uint8_t)n;
    f[3] = (uint8_t)(n >> 8);
    f[4] = (uint8_t)(g | (is_ac ? 0x80u : 0u));   /* bit 7 = AC */
    f[5] = (uint8_t)rate;
    f[6] = (uint8_t)(rate >> 8);
    f[7] = (uint8_t)(rate >> 16);
    f[8] = (uint8_t)(rate >> 24);
    f[9] = (uint8_t)zero;
    f[10] = (uint8_t)((uint16_t)zero >> 8);
    uint32_t body = 2u + 1u + 4u + 2u + 2u * n;
    f[2u + body] = crc8(f + 2, body);
    return 2u + body + 1u;
}

/* ------------------------- mudancas de config ------------------------- */
static void set_gain(unsigned g)
{
    if (g > 10u) return;
    cfg.gain = g;
    scope_set_gain(g);
    restart = 1;
}

static void set_ac(int on)
{
    cfg.ac = on;
    scope_set_coupling(on ? !GAIN_CAL_DC_LEVEL : GAIN_CAL_DC_LEVEL);
    restart = 1;
}

static void set_tb(int i)
{
    if (i < 0) i = 0;
    if (i >= N_TB) i = N_TB - 1;
    tb = i;
    rate_req = TB_RATE[i];
    restart = 1;
}

/* proxima base de tempo a partir da atual (ou da taxa livre do PC) */
static void step_tb(int dir)
{
    if (tb >= 0) {
        set_tb(tb + dir);
        return;
    }
    int i = 0;                                /* primeira base com taxa <= pedida */
    while (i < N_TB - 1 && TB_RATE[i] > rate_req) i++;
    set_tb(dir > 0 ? i : i - 1);
}

static int clamp_pos(int v)
{
    return v < -VIEW_POS_MAX ? -VIEW_POS_MAX : (v > VIEW_POS_MAX ? VIEW_POS_MAX : v);
}

static void set_defaults(void)
{
    set_gain(GAIN_DEFAULT);
    set_tb(TB_DEFAULT);
    set_ac(0);
    cfg.pos_px = 0;
    cfg.trig_px = TRIG_DEFAULT;
    cfg.falling = 0;
    cfg.mode = TRIG_AUTO;
    running = 1;
    sel = SEL_VDIV;
}

/* CIMA (+1) / BAIXO (-1) no parametro escolhido */
static void adjust(int dir, int step)
{
    switch (sel) {
    case SEL_VDIV:
        if (dir > 0 || cfg.gain > 0u) set_gain(cfg.gain + (unsigned)dir);
        break;
    case SEL_TDIV:
        step_tb(dir);
        break;
    case SEL_POS:
        cfg.pos_px = clamp_pos(cfg.pos_px + dir * step);
        if (!running || waiting) view_redraw(&cfg);   /* traco parado acompanha */
        break;
    case SEL_TRIG:
        cfg.trig_px = clamp_pos(cfg.trig_px + dir * step);
        break;
    default:
        break;
    }
}

/* ------------------------ auto-calibracao do zero ----------------------- */
#define C_WHITE  RGB565(255, 255, 255)
#define C_YELLOW RGB565(255, 255, 0)
#define C_GREEN  RGB565(0, 255, 0)
#define C_RED    RGB565(255, 0, 0)
#define C_GRAY   RGB565(140, 140, 140)

static void cal_enter(void)
{
    cal_state = CAL_ASK;
    restart = 1;
    view_panel_clear();
    view_panel_line(0, "AUTO-CALIBRACAO DO ZERO", C_WHITE);
    view_panel_line(2, "Encoste a ponta no", C_YELLOW);
    view_panel_line(3, "jacare (0 V na entrada)", C_YELLOW);
    view_panel_line(4, "e aperte OK (a roda).", C_YELLOW);
    view_panel_line(6, "RUN cancela.", C_GRAY);
    view_panel_line(7, zerocal_from_flash() ? "Hoje: medido no aparelho" : "Hoje: valores do PC", C_GRAY);
}

static void cal_exit(void)
{
    cal_state = CAL_OFF;
    view_init();
    scope_set_gain(cfg.gain);
    set_ac(cfg.ac);                           /* volta o acoplamento e refaz a captura */
}

static void cal_progress(unsigned g, int is_ac)
{
    char txt[24], *p = txt;
    const char *s = "Faixa ";
    while (*s) *p++ = *s++;
    if (g + 1u >= 10u) *p++ = '1';
    *p++ = (char)('0' + (g + 1u) % 10u);
    s = is_ac ? "/11 em AC..." : "/11 em DC...";
    while (*s) *p++ = *s++;
    *p = 0;
    view_panel_line(5, txt, C_WHITE);
}

static void cal_idle(void)
{
    usb_cdc_poll();
}

/* mede e grava (bloqueia ~5 s, atendendo o USB), mostra o resultado e volta */
static void cal_run(uint16_t *buf)
{
    view_panel_clear();
    view_panel_line(0, "AUTO-CALIBRACAO DO ZERO", C_WHITE);
    view_panel_line(2, "Medindo: nao mexa", C_YELLOW);
    view_panel_line(3, "na ponta.", C_YELLOW);

    int r = zerocal_run(buf, cal_progress, cal_idle);
    cal_result = (int8_t)r;
    switch (r) {
    case ZC_OK:
        view_panel_line(5, "Pronto: zeros gravados.", C_GREEN);
        break;
    case ZC_SIGNAL:
        view_panel_line(5, "Erro: sinal na entrada.", C_RED);
        view_panel_line(6, "Ponta no jacare e repita", C_RED);
        break;
    case ZC_RANGE:
        view_panel_line(5, "Erro: zero improvavel.", C_RED);
        view_panel_line(6, "Nada foi gravado.", C_RED);
        break;
    case ZC_ADC:
        view_panel_line(5, "Erro: ADC nao respondeu.", C_RED);
        view_panel_line(6, "Nada foi gravado.", C_RED);
        break;
    default:
        view_panel_line(5, "Erro ao gravar a flash.", C_RED);
        view_panel_line(6, zerocal_from_flash() ? "Mantidos os da flash." : "Usando valores do PC.", C_RED);
        break;
    }
    uint32_t t0 = sys_ms();
    while ((sys_ms() - t0) < 2500u) usb_cdc_poll();
    cal_exit();
}

/* ------------------------- estado para o PC ------------------------- */
/*
 * Quadro de estado (A5 5B), mandado quando a configuracao muda (pelo PC ou
 * pelos botoes), depois de um Q e a cada segundo:
 *     A5 5B | len (u8) | payload[len] | crc8 (de len ate o fim do payload)
 * payload (little-endian), versao 1:
 *     0 versao (1)   1 faixa 0..10   2 AC (0/1)   3 modo (0 AUTO, 1 NORMAL, 2 SINGLE)
 *     4 borda (0 subida, 1 descida)   5 rodando (0/1)   6 esperando trigger (0/1)
 *     7 calibracao (0 fora, 1 pagina aberta, 2 medindo)   8 zero da flash (0/1)
 *     9 base de tempo (i8, -1 = taxa livre)   10 trig_px (i16)   12 pos_px (i16)
 *     14 taxa pedida (u32)   18 taxa real (u32)   22 amostras por quadro (u16)
 *     24 parametro da roda (u8)   25 ultimo resultado da calibracao (i8, -1 = nenhum)
 * Os px sao os da tela: 25 px por divisao, entao volts = px * (V/div) / 25.
 * O pc/plot.py so procura A5 5A e pula este quadro.
 */
#define STATUS_LEN 26u
static uint8_t  status_buf[2u + 1u + STATUS_LEN + 1u];
static uint8_t  status_last[STATUS_LEN];
static int      status_force = 1;
static uint32_t status_ms;

static void status_payload(uint8_t *p)
{
    uint32_t real = scope_actual_rate(rate_req);
    p[0] = 1u;
    p[1] = (uint8_t)cfg.gain;
    p[2] = (uint8_t)(cfg.ac != 0);
    p[3] = (uint8_t)cfg.mode;
    p[4] = (uint8_t)(cfg.falling != 0);
    p[5] = (uint8_t)(running != 0);
    p[6] = (uint8_t)(waiting != 0);
    p[7] = (uint8_t)cal_state;
    p[8] = (uint8_t)(zerocal_from_flash() != 0);
    p[9] = (uint8_t)(int8_t)tb;
    p[10] = (uint8_t)cfg.trig_px;  p[11] = (uint8_t)((uint16_t)cfg.trig_px >> 8);
    p[12] = (uint8_t)cfg.pos_px;   p[13] = (uint8_t)((uint16_t)cfg.pos_px >> 8);
    for (int k = 0; k < 4; k++) {
        p[14 + k] = (uint8_t)(rate_req >> (8 * k));
        p[18 + k] = (uint8_t)(real >> (8 * k));
    }
    p[22] = (uint8_t)n_samples;    p[23] = (uint8_t)(n_samples >> 8);
    p[24] = (uint8_t)sel;
    p[25] = (uint8_t)cal_result;
}

/* manda o estado se mudou, se foi pedido ou se passou 1 s; so com o USB livre */
static void status_poll(void)
{
    if (!usb_cdc_ready()) { status_force = 1; return; }
    if (usb_cdc_tx_busy()) return;
    uint8_t now[STATUS_LEN];
    status_payload(now);
    int changed = 0;
    for (unsigned k = 0; k < STATUS_LEN; k++) {
        if (now[k] != status_last[k]) changed = 1;
    }
    if (!changed && !status_force && (sys_ms() - status_ms) < 1000u) return;
    for (unsigned k = 0; k < STATUS_LEN; k++) {
        status_last[k] = now[k];
        status_buf[3 + k] = now[k];
    }
    status_buf[0] = 0xA5;
    status_buf[1] = 0x5B;
    status_buf[2] = (uint8_t)STATUS_LEN;
    status_buf[3 + STATUS_LEN] = crc8(status_buf + 2, 1u + STATUS_LEN);
    usb_cdc_write(status_buf, sizeof status_buf);
    status_force = 0;
    status_ms = sys_ms();
}

/* ------------------------- comandos do PC ------------------------- */
static char     line[16];
static unsigned line_len;

/*
 * Uma letra e, conforme o comando, um numero com sinal opcional:
 *     G<0..10> faixa          R<hz> taxa livre        B<i> base de tempo da tela
 *     N<n> amostras           C<0|1> nivel do PA3     T<0|1|2> modo do trigger
 *     E<0|1> borda            L<px> nivel do trigger  P<px> posicao do 0 V
 *     S<0|1> parar / rodar (no SINGLE, S1 arma)       A trigger no meio do sinal
 *     D tudo no padrao        Q pede o estado         Z mede e grava os zeros
 */
static void run_command(void)
{
    if (line_len < 1u) return;
    int32_t v = 0;
    int neg = 0, has_num = 0;
    for (unsigned i = 1; i < line_len; i++) {
        char ch = line[i];
        if (i == 1u && ch == '-') { neg = 1; continue; }
        if (ch < '0' || ch > '9') return;
        if (v < 100000000) v = v * 10 + (ch - '0');
        has_num = 1;
    }
    if (neg) v = -v;
    char cmd = (char)(line[0] | 0x20);        /* minuscula */
    int needs_num = (cmd != 'a' && cmd != 'd' && cmd != 'q' && cmd != 'z');
    if (needs_num && !has_num) return;

    switch (cmd) {
    case 'g':
        if (v >= 0) set_gain((unsigned)v);
        break;
    case 'r':
        if (v > 0) { rate_req = (uint32_t)v; tb = -1; restart = 1; }
        break;
    case 'b':
        set_tb(v);
        break;
    case 'n':
        if (v >= (int32_t)N_MIN && v <= (int32_t)N_MAX) { n_samples = (uint16_t)v; restart = 1; }
        break;
    case 'c':                                 /* nivel cru do PA3 */
        set_ac((v != 0) != GAIN_CAL_DC_LEVEL);
        break;
    case 't':
        if (v >= TRIG_AUTO && v <= TRIG_SINGLE) { cfg.mode = (int)v; running = 1; restart = 1; }
        break;
    case 'e':
        cfg.falling = (v != 0);
        break;
    case 'l':
        cfg.trig_px = clamp_pos((int)v);
        break;
    case 'p':
        cfg.pos_px = clamp_pos((int)v);
        if (!running || waiting) view_redraw(&cfg);
        break;
    case 's':
        running = (v != 0);
        restart = 1;
        break;
    case 'a':
        cfg.trig_px = clamp_pos(view_mid_px());
        break;
    case 'd':
        set_defaults();
        break;
    case 'z':                                 /* o PC ja pediu a ponta no jacare */
        if (cal_state == CAL_OFF) cal_enter();
        cal_state = CAL_RUN;
        break;
    case 'q':
        break;
    default:
        return;
    }
    status_force = 1;                         /* responde com o estado novo */
}

static void poll_commands(void)
{
    int c;
    while ((c = usb_cdc_getc()) >= 0) {
        if (c == '\r' || c == '\n') {
            run_command();
            line_len = 0;
        } else if (line_len < sizeof line) {
            line[line_len++] = (char)c;
        }
    }
}

/* ---------------------- botoes e POWER (10 ms) ----------------------- */
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_RUN, K_MODE, K_AUTO };
static const struct { GPIO_TypeDef *port; uint8_t pin; uint8_t key; } KEYS[] = {
    {GPIOC, 6, K_UP},   {GPIOA, 9, K_DOWN}, {GPIOC, 3, K_LEFT}, {GPIOC, 5, K_RIGHT},
    {GPIOC, 4, K_OK},   {GPIOA, 10, K_RUN}, {GPIOC, 7, K_MODE}, {GPIOC, 8, K_AUTO},
};
#define N_KEYS (sizeof KEYS / sizeof KEYS[0])

static void key_short(unsigned key)
{
    if (cal_state == CAL_ASK) {               /* pagina de calibracao: so OK e RUN */
        if (key == K_OK)  cal_state = CAL_RUN;
        if (key == K_RUN) cal_exit();
        return;
    }
    if (cal_state != CAL_OFF) return;
    switch (key) {
    case K_LEFT:  sel = (sel + SEL_COUNT - 1) % SEL_COUNT; break;
    case K_RIGHT: sel = (sel + 1) % SEL_COUNT; break;
    case K_OK:    set_ac(!cfg.ac); break;
    case K_RUN:   running = !running; restart = 1; break;
    case K_MODE:  cfg.mode = (cfg.mode + 1) % 3; running = 1; restart = 1; break;
    case K_AUTO:  cfg.trig_px = clamp_pos(view_mid_px()); break;
    default:      break;
    }
}

static void key_long(unsigned key)
{
    if (cal_state != CAL_OFF) return;
    if (key == K_MODE) cfg.falling = !cfg.falling;
    if (key == K_AUTO) set_defaults();
    if (key == K_RUN)  cal_enter();
}

static void keys_tick(void)
{
    static uint16_t held[N_KEYS];
    for (unsigned i = 0; i < N_KEYS; i++) {
        unsigned key = KEYS[i].key;
        int has_long = (key == K_MODE || key == K_AUTO || key == K_RUN);
        unsigned long_at = (key == K_RUN) ? CAL_TICKS : LONG_TICKS;
        if (gpio_read(KEYS[i].port, KEYS[i].pin)) {           /* solto */
            if (has_long && held[i] >= 2u && held[i] < long_at) key_short(key);
            held[i] = 0;
            continue;
        }
        if (held[i] < 0xFFFFu) held[i]++;
        unsigned h = held[i];
        if (key == K_UP || key == K_DOWN) {
            if (cal_state != CAL_OFF) continue;
            /* posicao e trigger andam de 1 px e repetem rapido; 2 s depois, de 5 */
            int fine = (sel == SEL_POS || sel == SEL_TRIG);
            unsigned wait = fine ? 30u : 50u, every = fine ? 3u : 15u;
            if (h == 2u || (h >= wait && (h - wait) % every == 0u)) {
                adjust(key == K_UP ? +1 : -1, (fine && h >= 200u) ? 5 : 1);
            }
        } else if (has_long) {
            if (h == long_at) key_long(key);
        } else if (h == 2u) {                                 /* 20 ms: debounce */
            key_short(key);
        }
    }
}

static void power_off(void)
{
    gpio_set(GPIOC, PIN_BL);    /* apaga o backlight (feedback visual) */
    gpio_clr(GPIOC, PIN_HOLD);  /* solta o power-hold: apaga ao soltar o POWER */
    for (;;) { /* se o USB estiver alimentando, fica aqui com a tela apagada */ }
}

static void power_tick(void)
{
    static uint32_t released_ms, held_ms;
    static int armed;

    if (gpio_read(GPIOC, PIN_PWR)) {
        held_ms = 0;
        if (released_ms < ARM_MS) released_ms += TICK_MS;
        else                      armed = 1;
    } else {
        released_ms = 0;
        if (armed) {
            held_ms += TICK_MS;
            if (held_ms >= HOLD_OFF_MS) {
                power_off();
            }
        }
    }
}

int main(void)
{
    /* Habilita AFIO + clocks das portas A, B e C. */
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN
                  | RCC_APB2ENR_IOPCEN;

    /* PC10 como saida push-pull e ALTO o quanto antes (manter ligado). */
    gpio_set(GPIOC, PIN_HOLD);
    gpio_cfg(GPIOC, PIN_HOLD, GPIO_CRx_OUT_PP_2M);

    /* Desabilita JTAG mas MANTEM o SWD (preserva PA13/PA14 p/ recuperacao).
     * SWJ_CFG precisa ser escrito de uma vez; usamos o valor "010". Libera
     * PB3/PB4/PA15 para o LCD. */
    AFIO->MAPR = (AFIO->MAPR & ~(0x7u << 24)) | AFIO_MAPR_SWJ_JTAGDISABLE;

    /* backlight apagado ate a tela estar desenhada; botoes com pull-up */
    gpio_set(GPIOC, PIN_BL);
    gpio_cfg(GPIOC, PIN_BL, GPIO_CRx_OUT_PP_2M);
    gpio_cfg(GPIOC, PIN_PWR, GPIO_CRx_IN_PULL);
    gpio_set(GPIOC, PIN_PWR);
    for (unsigned i = 0; i < N_KEYS; i++) {
        gpio_cfg(KEYS[i].port, KEYS[i].pin, GPIO_CRx_IN_PULL);
        gpio_set(KEYS[i].port, KEYS[i].pin);
    }

    int clk_ok = sys_clock_hse_72mhz();
    crc8_init();
    zerocal_load();
    lcd_init();
    view_init();
    gpio_clr(GPIOC, PIN_BL);                  /* acende */

    if (clk_ok) {
        scope_init();
        scope_set_gain(cfg.gain);
        set_ac(0);
        usb_cdc_init();
    } else {
        view_message("Cristal falhou: sem USB");
    }

    int      capturing = 0, full = 0, buf = 0, tx_buf = -1, cap_ac = 0;
    uint16_t cap_n = 0;
    uint8_t  cap_gain = 0;
    uint32_t cap_rate = rate_req;
    uint32_t last_tick = sys_ms();

    for (;;) {
        if (clk_ok) {
            usb_cdc_poll();
            poll_commands();

            if (cal_state == CAL_RUN) {
                cal_run(frame_samples(tx_buf == 0 ? 1 : 0));   /* fora do que o USB envia */
            }
            if (restart) {
                scope_stop();
                capturing = 0;
                full = 0;
                restart = 0;
            }
            /* nao captura por cima de um buffer que ainda esta saindo pelo USB */
            if (running && cal_state == CAL_OFF && !capturing && !full
                && !(usb_cdc_tx_busy() && tx_buf == buf)) {
                cap_rate = scope_set_rate(rate_req);
                cap_n = n_samples;
                cap_gain = (uint8_t)cfg.gain;
                cap_ac = cfg.ac;
                scope_start(frame_samples(buf), cap_n);
                capturing = 1;
            }
            if (capturing && scope_done()) {
                scope_stop();
                capturing = 0;
                full = 1;
            }
            /* bloco pronto: manda e desenha quando o anterior tiver saido */
            if (full && !usb_cdc_tx_busy() && !view_busy()) {
                if (usb_cdc_ready()) {
                    uint32_t len = frame_finish(buf, cap_n, cap_gain, cap_ac, cap_rate);
                    usb_cdc_write(frame_bytes(buf), len);
                    tx_buf = buf;
                }
                view_cfg_t c = cfg;           /* escala do bloco = a da captura */
                c.gain = cap_gain;
                c.ac = cap_ac;
                int trig = view_wave(frame_samples(buf), cap_n, cap_rate, &c);
                waiting = (cfg.mode != TRIG_AUTO) && !trig;
                if (trig || cfg.mode == TRIG_AUTO) {
                    buf ^= 1;                 /* o mostrado fica; captura no outro */
                }
                if (trig && cfg.mode == TRIG_SINGLE) {
                    running = 0;              /* SINGLE: um disparo e para */
                    restart = 1;
                }
                full = 0;
            }
            view_step();
            status_poll();
        }

        if ((sys_ms() - last_tick) >= TICK_MS) {
            last_tick += TICK_MS;
            power_tick();
            if (clk_ok) {
                keys_tick();
                int usb = usb_cdc_ready() ? 2 : (usb_cdc_configured() ? 1 : 0);
                int run = (!running || cal_state != CAL_OFF) ? RUN_STOP
                        : (waiting ? RUN_WAIT : RUN_RUN);
                view_status(&cfg, scope_actual_rate(rate_req), run, usb, sel);
            }
        }
    }
}
