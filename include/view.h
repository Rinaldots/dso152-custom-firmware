/*
 * view.h — tela de osciloscopio no LCD: grade, forma de onda e estado.
 */
#ifndef VIEW_H
#define VIEW_H

#include <stdint.h>

#define VIEW_COLS 300u      /* colunas da area do sinal (12 divisoes de 25 px) */
#define VIEW_POS_MAX 200    /* limite da posicao e do nivel de trigger, em px */

enum { TRIG_AUTO, TRIG_NORMAL, TRIG_SINGLE };
enum { SEL_VDIV, SEL_TDIV, SEL_POS, SEL_TRIG, SEL_COUNT };  /* parametro na roda */
enum { RUN_STOP, RUN_RUN, RUN_WAIT };

typedef struct {
    unsigned gain;      /* faixa 0..10 */
    int      ac;        /* 0 = DC, 1 = AC */
    int      pos_px;    /* 0 V acima da linha do meio, em pixels */
    int      trig_px;   /* nivel de trigger acima do 0 V, em pixels */
    int      falling;   /* 1 = borda de descida */
    int      mode;      /* TRIG_AUTO / TRIG_NORMAL / TRIG_SINGLE */
} view_cfg_t;

void view_init(void);       /* desenha a moldura e a grade vazias */

/* Analisa um bloco (trigger e medidas) e, se for para mostrar (disparou, ou
 * modo AUTO), comeca a desenha-lo aos poucos. Devolve 1 se disparou. O buffer
 * precisa continuar valido ate view_busy() == 0. */
int  view_wave(const uint16_t *s, uint16_t n, uint32_t rate, const view_cfg_t *cfg);
void view_redraw(const view_cfg_t *cfg);   /* redesenha o ultimo bloco (posicao nova) */
int  view_busy(void);
void view_step(void);       /* desenha mais algumas colunas; chamar no loop */
int  view_mid_px(void);     /* meio do ultimo bloco, em px acima do 0 V */

/* Atualiza textos e marcadores (so redesenha o que mudou). usb: 0 = sem
 * cabo, 1 = enumerado, 2 = porta aberta no PC. sel: parametro destacado. */
void view_status(const view_cfg_t *cfg, uint32_t rate, int run, int usb, int sel);
void view_message(const char *msg);   /* aviso na linha de baixo */

/* Painel de texto no lugar do sinal (pagina de auto-calibracao): limpa a
 * area e escreve linhas de ate 24 caracteres (0..7). view_init() volta a grade. */
void view_panel_clear(void);
void view_panel_line(unsigned line, const char *text, uint16_t color);

#endif
