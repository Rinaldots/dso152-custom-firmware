/*
 * zerocal.h — zero (contagens do ADC com 0 V na ponta) de cada faixa, em DC e
 * em AC. O padrao vem de include/gain_cal.h (medido pelo PC); a
 * auto-calibracao no aparelho mede de novo e grava na flash, e o que estiver
 * gravado e valido passa a valer.
 */
#ifndef ZEROCAL_H
#define ZEROCAL_H

#include <stdint.h>

enum { ZC_OK, ZC_SIGNAL, ZC_RANGE, ZC_ADC, ZC_FLASH };

void zerocal_load(void);                 /* chamar no boot */
int  zerocal_from_flash(void);           /* 1 = zeros em uso vieram da flash */
int  zero_offset(unsigned gain, int ac);

/* Mede o zero das 11 faixas em DC e em AC (ponta em curto) e grava na flash.
 * Precisa do scope_init() feito; buf comporta ZC_SAMPLES amostras.
 * progress(faixa, ac) e chamado antes de cada medida e idle() durante as
 * esperas (para o USB continuar atendido). Devolve ZC_OK ou o erro. */
#define ZC_SAMPLES 1024u
int  zerocal_run(uint16_t *buf, void (*progress)(unsigned gain, int ac), void (*idle)(void));

#endif
