/*
 * startup.c — tabela de vetores + Reset_Handler para o app do DSO152.
 *
 * O app e linkado em 0x08003C00 (logo apos o bootloader "DSO BOOT"). A tabela
 * de vetores precisa estar no inicio da imagem: a 1a palavra e o Stack Pointer
 * inicial, a 2a e o endereco do Reset_Handler. O .bin cru gerado pelo objcopy
 * comeca exatamente por esta tabela — e o que o bootloader espera receber.
 *
 * Reset_Handler: copia .data (flash->RAM), zera .bss, ajusta VTOR, chama
 * SystemInit() e depois main().
 */
#include <stdint.h>
#include "ch32f103.h"

/* Simbolos definidos pelo linker script (dso152_app.ld) */
extern uint32_t _sidata;   /* inicio de .data na FLASH (LMA) */
extern uint32_t _sdata;    /* inicio de .data na RAM  (VMA) */
extern uint32_t _edata;    /* fim de .data na RAM */
extern uint32_t _sbss;     /* inicio de .bss */
extern uint32_t _ebss;     /* fim de .bss */
extern uint32_t _estack;   /* topo da pilha (fim da RAM) */

void SystemInit(void);     /* system.c */
int  main(void);

void Reset_Handler(void);
void Default_Handler(void);

/* Handlers fracos: tudo que nao for sobrescrito cai no Default_Handler. */
#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void)        WEAK_ALIAS;
void HardFault_Handler(void)  WEAK_ALIAS;
void MemManage_Handler(void)  WEAK_ALIAS;
void BusFault_Handler(void)   WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void)        WEAK_ALIAS;
void DebugMon_Handler(void)   WEAK_ALIAS;
void PendSV_Handler(void)     WEAK_ALIAS;
void SysTick_Handler(void)    WEAK_ALIAS;

/* IRQs usadas nas proximas etapas (DMA1_Ch1, ADC, TIM1, USB...) podem ser
 * sobrescritas definindo-se funcoes de mesmo nome; por ora caem no default. */
void DMA1_Channel1_IRQHandler(void) WEAK_ALIAS;
void ADC1_2_IRQHandler(void)        WEAK_ALIAS;
void USB_LP_CAN1_RX0_IRQHandler(void) WEAK_ALIAS;
void TIM1_UP_IRQHandler(void)       WEAK_ALIAS;
void TIM2_IRQHandler(void)          WEAK_ALIAS;

/* A tabela vai na secao .isr_vector, posta no inicio da FLASH pelo linker.
 * Preenche os 16 vetores do core + as primeiras IRQs relevantes; o resto
 * fica 0 (nunca disparado enquanto nao habilitarmos essas interrupcoes). */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))(&_estack),   /* 0x00 Stack Pointer inicial */
    Reset_Handler,                /* 0x04 Reset */
    NMI_Handler,                  /* 0x08 */
    HardFault_Handler,            /* 0x0C */
    MemManage_Handler,            /* 0x10 */
    BusFault_Handler,             /* 0x14 */
    UsageFault_Handler,           /* 0x18 */
    0, 0, 0, 0,                   /* 0x1C-0x28 reservados */
    SVC_Handler,                  /* 0x2C */
    DebugMon_Handler,             /* 0x30 */
    0,                            /* 0x34 reservado */
    PendSV_Handler,               /* 0x38 */
    SysTick_Handler,              /* 0x3C */
    /* ---- IRQs externas (posicao 0 = WWDG) ---- */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* IRQ 0..10 */
    DMA1_Channel1_IRQHandler,         /* IRQ 11: DMA1_Channel1 */
    0, 0, 0, 0, 0, 0,                 /* IRQ 12..17 */
    ADC1_2_IRQHandler,                /* IRQ 18: ADC1_2 */
    0,                                /* IRQ 19: USB_HP_CAN1_TX */
    USB_LP_CAN1_RX0_IRQHandler,       /* IRQ 20: USB_LP_CAN1_RX0 */
    0, 0,                             /* IRQ 21..22 */
    0,                                /* IRQ 23: EXTI9_5 */
    0,                                /* IRQ 24: TIM1_BRK */
    TIM1_UP_IRQHandler,               /* IRQ 25: TIM1_UP */
    0, 0,                             /* IRQ 26..27 */
    TIM2_IRQHandler,                  /* IRQ 28: TIM2 */
};

void Reset_Handler(void)
{
    /* copia .data: FLASH (_sidata) -> RAM (_sdata.._edata) */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    /* zera .bss */
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    /* Realoca a tabela de vetores para o app (critico: o reset entra pela
     * tabela do bootloader; a partir daqui usamos a nossa). Aponta VTOR para
     * onde a tabela realmente ficou (inicio da FLASH do app = 0x08003C00). */
    SCB_VTOR = (uint32_t)(uintptr_t)g_vectors;

    SystemInit();
    (void)main();

    for (;;) { /* main nao deve retornar */ }
}

void Default_Handler(void)
{
    for (;;) { /* trava aqui em excecao nao tratada (ajuda a depurar) */ }
}
