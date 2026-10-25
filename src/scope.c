/*
 * scope.c — aquisicao do DSO152, igual ao firmware original:
 *   - ADC1 no PA1 (canal 1, amostragem de 1,5 ciclo), disparado pelo evento
 *     TIM1_CC1 (PWM1, CCR1 = meio periodo, MOE ligado);
 *   - DMA1_Ch1 copia ADC1->DR para o buffer, n amostras por captura;
 *   - ganho pela GAIN_TABLE em PA2 (rele do atenuador) e PA4/PA5/PA6 (mux
 *     CD74HC4051); acoplamento AC/DC em PA3;
 *   - borne de calibracao (PA0): 1 kHz, 50 %, pelo TIM2_CH1, como no manual.
 *
 * O PA8 (TIM1_CH1) e o reset do LCD: fica como GPIO, entao o PWM do TIM1 nao
 * sai no pino; ele so serve de gatilho para o ADC.
 */
#include <stdint.h>
#include "ch32f103.h"
#include "dso152_pins.h"
#include "scope.h"

void delay_ms(uint32_t ms);

#define TIM_CLK_HZ 72000000u   /* TIM1 (APB2) e TIM2 (APB1 x2) */

/* {PA2, PA4, PA5, PA6}, na ordem das colunas da GAIN_TABLE */
static const uint8_t GAIN_PINS[4] = {2, 4, 5, 6};

static uint16_t tim_psc, tim_arr;

void scope_set_gain(unsigned idx)
{
    if (idx > 10u) return;
    for (unsigned k = 0; k < 4; k++) {
        if (GAIN_TABLE[idx][k]) gpio_set(GPIOA, GAIN_PINS[k]);
        else                    gpio_clr(GPIOA, GAIN_PINS[k]);
    }
}

void scope_set_coupling(int level)
{
    if (level) gpio_set(GPIOA, 3);
    else       gpio_clr(GPIOA, 3);
}

/* ticks do timer por amostra = (PSC+1)*(ARR+1); menor PSC com ARR <= 65535 */
static uint32_t rate_timing(uint32_t hz, uint32_t *psc, uint32_t *arr)
{
    if (hz < SCOPE_RATE_MIN) hz = SCOPE_RATE_MIN;
    if (hz > SCOPE_RATE_MAX) hz = SCOPE_RATE_MAX;
    uint32_t ticks = (TIM_CLK_HZ + hz / 2u) / hz;
    *psc = (ticks - 1u) / 65536u;
    *arr = ticks / (*psc + 1u) - 1u;
    return TIM_CLK_HZ / ((*psc + 1u) * (*arr + 1u));
}

uint32_t scope_actual_rate(uint32_t hz)
{
    uint32_t psc, arr;
    return rate_timing(hz, &psc, &arr);
}

uint32_t scope_set_rate(uint32_t hz)
{
    uint32_t psc, arr;
    uint32_t real = rate_timing(hz, &psc, &arr);
    tim_psc = (uint16_t)psc;
    tim_arr = (uint16_t)arr;
    return real;
}

void scope_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_ADC1EN | RCC_APB2ENR_TIM1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    RCC->AHBENR  |= RCC_AHBENR_DMA1EN;

    /* perifericos limpos (o bootloader pode ter mexido neles) */
    RCC->APB2RSTR |=  (RCC_APB2ENR_ADC1EN | RCC_APB2ENR_TIM1EN);
    RCC->APB2RSTR &= ~(RCC_APB2ENR_ADC1EN | RCC_APB2ENR_TIM1EN);
    RCC->APB1RSTR |=  RCC_APB1ENR_TIM2EN;
    RCC->APB1RSTR &= ~RCC_APB1ENR_TIM2EN;

    /* pinos: PA1 analogico; PA2..PA6 saidas (ganho e AC/DC) */
    gpio_cfg(GPIOA, 1, GPIO_CRx_IN_ANALOG);
    for (int pin = 2; pin <= 6; pin++) {
        gpio_cfg(GPIOA, pin, GPIO_CRx_OUT_PP_2M);
    }

    /* calibracao: TIM2_CH1 em PA0, 72 MHz / 72 / 1000 = 1 kHz, 50 % */
    gpio_cfg(GPIOA, 0, GPIO_CRx_AF_PP_2M);
    TIM2->PSC   = 72u - 1u;
    TIM2->ARR   = 1000u - 1u;
    TIM2->CCR1  = 500u;
    TIM2->CCMR1 = TIM_CCMR1_OC1M_PWM1 | TIM_CCMR1_OC1PE;
    TIM2->CCER  = TIM_CCER_CC1E;
    TIM2->EGR   = TIM_EGR_UG;
    TIM2->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;

    /* ADC1: 1 conversao (canal 1), 1,5 ciclo, calibra e liga o gatilho externo */
    ADC1->CR1   = 0u;
    ADC1->SMPR2 = 0u;
    ADC1->SQR1  = 0u;
    ADC1->SQR3  = 1u;
    ADC1->CR2   = ADC_CR2_ADON;
    delay_ms(2);
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (ADC1->CR2 & ADC_CR2_RSTCAL) { }
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL) { }
    /* mudar outros bits junto com ADON nao dispara conversao */
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_DMA | ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL_T1CC1;

    scope_set_rate(100000u);
}

void scope_start(uint16_t *dst, uint16_t n)
{
    scope_stop();

    /* TIM1: PWM1 no canal 1, igual ao original (polaridade baixa, MOE) */
    TIM1->PSC   = tim_psc;
    TIM1->ARR   = tim_arr;
    TIM1->CCR1  = (uint16_t)((tim_arr + 1u) / 2u);
    TIM1->CCMR1 = TIM_CCMR1_OC1M_PWM1 | TIM_CCMR1_OC1PE;
    TIM1->CCER  = TIM_CCER_CC1E | TIM_CCER_CC1P;
    TIM1->BDTR  = TIM_BDTR_MOE;
    TIM1->EGR   = TIM_EGR_UG;      /* carrega PSC/ARR/CCR1 */
    TIM1->SR    = 0u;
    TIM1->CNT   = 0u;

    (void)ADC1->DR;                /* descarta conversao velha (limpa EOC) */
    DMA1->IFCR = DMA_IFCR_CH1;
    DMA1_Channel1->CPAR  = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR  = (uint32_t)dst;
    DMA1_Channel1->CNDTR = n;
    DMA1_Channel1->CCR   = DMA_CCR_PL_HIGH | DMA_CCR_MSIZE16 | DMA_CCR_PSIZE16
                         | DMA_CCR_MINC | DMA_CCR_EN;

    TIM1->CR1 = TIM_CR1_CEN;
}

int scope_done(void)
{
    return (DMA1->ISR & DMA_ISR_TCIF1) != 0u;
}

void scope_stop(void)
{
    TIM1->CR1 = 0u;
    DMA1_Channel1->CCR = 0u;
    DMA1->IFCR = DMA_IFCR_CH1;
}
