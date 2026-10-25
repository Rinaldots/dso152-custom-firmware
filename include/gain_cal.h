/*
 * gain_cal.h — calibracao das faixas de ganho, gerada por pc/calibrate.py em
 * 2026-10-25T19:19:39: medido com Vcal = 3.300 V.
 * Nao edite a mao: rode o script de novo.
 */
#ifndef GAIN_CAL_H
#define GAIN_CAL_H

#include <stdint.h>

#define GAIN_CAL_MEASURED 1
#define GAIN_CAL_DC_LEVEL 1   /* nivel do PA3 que da acoplamento DC */

/* Por faixa (G0..G10, rotulos do original): zero em DC e em AC (contagens com
 * 0 V na ponta), V/div em uV, pixels por contagem em Q16 (25 px por divisao)
 * e uV por contagem. */
static const struct {
    int16_t  off_dc;
    int16_t  off_ac;
    uint32_t vdiv_uv;
    uint32_t px_q16;
    uint32_t uv_count;
} GAIN_CAL[11] __attribute__((unused)) = {
    { 2452, 2452,    10000u,  18867u,     115u },  /* G0 0.01 V/div (zeroAC=DC cpv=rele) */
    { 2298, 2298,    20000u,  18856u,     230u },  /* G1 0.02 V/div (zeroAC=DC cpv=rele) */
    { 2170, 2170,    50000u,  18892u,     577u },  /* G2 0.05 V/div (zeroAC=DC cpv=rele) */
    { 2129, 2050,   100000u,  18922u,    1155u },  /* G3 0.1 V/div (cpv=rele) */
    { 2108, 2084,   200000u,  19073u,    2328u },  /* G4 0.2 V/div */
    { 2097, 2090,   500000u,  16474u,    5028u },  /* G5 0.5 V/div */
    { 2090, 2092,  1000000u,  16464u,   10049u },  /* G6 1 V/div */
    { 2086, 2086,  2000000u,  20620u,   25171u },  /* G7 2 V/div */
    { 2085, 2083,  5000000u,  16521u,   50418u },  /* G8 5 V/div */
    { 2084, 2085, 10000000u,  16654u,  101647u },  /* G9 10 V/div */
    { 2084, 2083, 20000000u,  16627u,  202963u },  /* G10 20 V/div */
};

#endif
