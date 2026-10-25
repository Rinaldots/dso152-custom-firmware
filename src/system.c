/*
 * system.c — clock do sistema e temporizacao.
 *
 * O Reset_Handler chama SystemInit(), que deixa o MCU num estado conhecido no
 * HSI (8 MHz interno). O main() depois chama sys_clock_hse_72mhz() para subir
 * a 72 MHz pelo cristal de 8 MHz (o original usa o mesmo cristal com PLL x10 =
 * 80 MHz; aqui usamos x9 porque o USB precisa de exatamente 48 MHz = 72/1,5).
 */
#include <stdint.h>
#include "ch32f103.h"

/* Frequencia atual do core, em Hz. Atualizada pelas rotinas de clock. */
volatile uint32_t SystemCoreClock = 8000000u;   /* HSI = 8 MHz */

/* Milissegundos desde o boot (incrementado pelo SysTick). */
static volatile uint32_t g_ms;

void SysTick_Handler(void)
{
    g_ms++;
}

uint32_t sys_ms(void)
{
    return g_ms;
}

static void systick_1ms(void)
{
    SysTick->LOAD = (SystemCoreClock / 1000u) - 1u;
    SysTick->VAL  = 0u;
    SysTick->CTRL = SYSTICK_CTRL_CLKSOURCE | SYSTICK_CTRL_TICKINT | SYSTICK_CTRL_ENABLE;
}

/* Chamado pelo Reset_Handler antes do main().
 *
 * IMPORTANTE: o bootloader "DSO BOOT" entrega o controle com o clock DELE ligado
 * (provavelmente PLL, por causa do USB) e possivelmente com IRQs habilitadas no
 * NVIC. Por isso: desliga todas as IRQs, forca SYSCLK = HSI 8 MHz e so entao
 * religa as interrupcoes (so o SysTick fica ativo). */
void SystemInit(void)
{
    /* 0) Nenhuma IRQ herdada do bootloader: desabilita e limpa pendentes. */
    SysTick->CTRL = 0u;
    for (int i = 0; i < 2; i++) {
        NVIC_ICER(i) = 0xFFFFFFFFu;
        NVIC_ICPR(i) = 0xFFFFFFFFu;
    }

    /* 1) Garante HSI ligado. */
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) { }

    /* 2) Comuta SYSCLK para HSI (SW=00) e confirma (SWS=00). */
    RCC->CFGR &= ~0x3u;
    while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != 0u) { }

    /* 3) Prescalers AHB/APB = /1 (bits HPRE/PPRE1/PPRE2 = [13:4]) -> core = 8 MHz. */
    RCC->CFGR &= ~0x3FF0u;

    /* 4) Agora que nao dependemos mais deles, desliga PLL e HSE. */
    RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_HSEON);

    SystemCoreClock = 8000000u;
    systick_1ms();
    __asm volatile ("cpsie i" ::: "memory");
}

/*
 * Sobe o clock para 72 MHz via HSE(8 MHz)+PLL x9, com USB a 48 MHz (PLL/1,5)
 * e ADC a 12 MHz (PCLK2/6). Faz fallback para HSI se o HSE nao estabilizar no
 * tempo esperado (nao trava). Retorna 1 se chegou em 72 MHz, 0 se permaneceu
 * no HSI (sem USB).
 */
int sys_clock_hse_72mhz(void)
{
    /* 2 wait states + prefetch (obrigatorio acima de 48 MHz). */
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    /* Liga o HSE e espera (com timeout). */
    RCC->CR |= RCC_CR_HSEON;
    for (volatile uint32_t t = 0; !(RCC->CR & RCC_CR_HSERDY); t++) {
        if (t > 0x20000u) {           /* HSE nao subiu: fica no HSI */
            RCC->CR &= ~RCC_CR_HSEON;
            return 0;
        }
    }

    /* Prescalers: AHB/1 (72), APB1/2 (36, limite), APB2/1 (72), ADC/6 (12). */
    RCC->CFGR &= ~(0x003FFFFFu | RCC_CFGR_USBPRE_MASK);
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1
               | RCC_CFGR_ADCPRE_DIV6;

    /* PLL = HSE * 9 = 72 MHz; USB = PLL/1,5 = 48 MHz (USBPRE = 0). */
    RCC->CFGR |= RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMULL9;
    RCC->CR  |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    /* Comuta SYSCLK para o PLL. */
    RCC->CFGR = (RCC->CFGR & ~0x3u) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) { }

    SystemCoreClock = 72000000u;
    systick_1ms();
    return 1;
}

/* Delay bloqueante em ms, contado pelo SysTick. */
void delay_ms(uint32_t ms)
{
    uint32_t t0 = g_ms;
    while ((g_ms - t0) < ms) { }
}
