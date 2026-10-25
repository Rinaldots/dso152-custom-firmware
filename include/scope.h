/*
 * scope.h — aquisicao do DSO152: ganho, acoplamento, captura por blocos e
 * sinal de calibracao.
 */
#ifndef SCOPE_H
#define SCOPE_H

#include <stdint.h>

#define SCOPE_RATE_MIN   10u        /* Hz */
#define SCOPE_RATE_MAX   800000u    /* Hz: ADC a 12 MHz, 14 ciclos por conversao */

void     scope_init(void);                 /* precisa do clock em 72 MHz */
void     scope_set_gain(unsigned idx);     /* 0..10, indice da GAIN_TABLE */
void     scope_set_coupling(int level);    /* nivel do PA3 (AC/DC) */
uint32_t scope_set_rate(uint32_t hz);      /* devolve a taxa real obtida */
uint32_t scope_actual_rate(uint32_t hz);   /* taxa real para hz, sem aplicar */

void     scope_start(uint16_t *dst, uint16_t n);  /* inicia captura de n amostras */
int      scope_done(void);                        /* 1 quando o DMA terminou */
void     scope_stop(void);                        /* para o timer e o DMA */

#endif
