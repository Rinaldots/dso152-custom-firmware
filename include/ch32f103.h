/*
 * ch32f103.h — definicoes minimas de registradores para o DSO152
 *
 * O MCU do DSO152 e um CH32F103 (WCH), compativel a nivel de registradores com
 * o STM32F103 (Cortex-M3). Este header traz apenas o subconjunto usado pelo
 * firmware (RCC, GPIO, AFIO, FLASH, ADC, DMA, TIM, USB, NVIC, SCB, SysTick).
 *
 * Mapa base (identico ao STM32F103):
 *   GPIOA 0x40010800  GPIOB 0x40010C00  GPIOC 0x40011000  AFIO 0x40010000
 *   RCC   0x40021000  FLASH 0x40022000  SCB  0xE000ED00   SysTick 0xE000E010
 */
#ifndef CH32F103_H
#define CH32F103_H

#include <stdint.h>

#define __IO volatile

/* ----------------------------- GPIO ----------------------------- */
typedef struct {
    __IO uint32_t CRL;   /* 0x00 config low  (pinos 0..7)  */
    __IO uint32_t CRH;   /* 0x04 config high (pinos 8..15) */
    __IO uint32_t IDR;   /* 0x08 input data                */
    __IO uint32_t ODR;   /* 0x0C output data               */
    __IO uint32_t BSRR;  /* 0x10 bit set/reset             */
    __IO uint32_t BRR;   /* 0x14 bit reset                 */
    __IO uint32_t LCKR;  /* 0x18 lock                      */
} GPIO_TypeDef;

#define GPIOA ((GPIO_TypeDef *)0x40010800u)
#define GPIOB ((GPIO_TypeDef *)0x40010C00u)
#define GPIOC ((GPIO_TypeDef *)0x40011000u)

/* CRL/CRH: 4 bits por pino -> [CNF1 CNF0 MODE1 MODE0]
 * MODE: 00=input  01=out 10MHz  10=out 2MHz  11=out 50MHz
 * CNF(saida): 00=push-pull 01=open-drain 10=AF push-pull 11=AF open-drain
 * CNF(entrada):00=analog   01=flutuante  10=pull-up/down
 */
#define GPIO_CRx_IN_ANALOG    0x0u   /* entrada analogica (ADC) */
#define GPIO_CRx_IN_FLOAT     0x4u   /* entrada flutuante */
#define GPIO_CRx_IN_PULL      0x8u   /* entrada com pull (direcao via ODR) */
#define GPIO_CRx_OUT_PP_2M    0x2u   /* saida push-pull 2 MHz */
#define GPIO_CRx_OUT_PP_50M   0x3u   /* saida push-pull 50 MHz */
#define GPIO_CRx_AF_PP_2M     0xAu   /* AF push-pull 2 MHz */
#define GPIO_CRx_AF_PP_50M    0xBu   /* AF push-pull 50 MHz (ex.: TIM PWM) */

/* Configura 1 pino (0..15) com um dos nibbles acima. */
static inline void gpio_cfg(GPIO_TypeDef *p, int pin, uint32_t mode4) {
    if (pin < 8) {
        p->CRL = (p->CRL & ~(0xFu << (pin * 4))) | (mode4 << (pin * 4));
    } else {
        int s = (pin - 8) * 4;
        p->CRH = (p->CRH & ~(0xFu << s)) | (mode4 << s);
    }
}
static inline void gpio_set(GPIO_TypeDef *p, int pin)   { p->BSRR = (1u << pin); }
static inline void gpio_clr(GPIO_TypeDef *p, int pin)   { p->BRR  = (1u << pin); }
static inline int  gpio_read(GPIO_TypeDef *p, int pin)  { return (p->IDR >> pin) & 1u; }

/* ----------------------------- AFIO ----------------------------- */
typedef struct {
    __IO uint32_t EVCR;
    __IO uint32_t MAPR;    /* 0x04 remap/debug: SWJ_CFG em [26:24] */
    __IO uint32_t EXTICR[4];
    __IO uint32_t MAPR2;
} AFIO_TypeDef;
#define AFIO ((AFIO_TypeDef *)0x40010000u)
/* SWJ_CFG = 010 => JTAG desabilitado, SWD habilitado (libera PB3/PB4/PA15) */
#define AFIO_MAPR_SWJ_JTAGDISABLE (0x2u << 24)

/* ----------------------------- RCC ------------------------------ */
typedef struct {
    __IO uint32_t CR;        /* 0x00 */
    __IO uint32_t CFGR;      /* 0x04 */
    __IO uint32_t CIR;       /* 0x08 */
    __IO uint32_t APB2RSTR;  /* 0x0C */
    __IO uint32_t APB1RSTR;  /* 0x10 */
    __IO uint32_t AHBENR;    /* 0x14 */
    __IO uint32_t APB2ENR;   /* 0x18 */
    __IO uint32_t APB1ENR;   /* 0x1C */
    __IO uint32_t BDCR;      /* 0x20 */
    __IO uint32_t CSR;       /* 0x24 */
} RCC_TypeDef;
#define RCC ((RCC_TypeDef *)0x40021000u)

#define RCC_CR_HSION   (1u << 0)
#define RCC_CR_HSIRDY  (1u << 1)
#define RCC_CR_HSEON   (1u << 16)
#define RCC_CR_HSERDY  (1u << 17)
#define RCC_CR_PLLON   (1u << 24)
#define RCC_CR_PLLRDY  (1u << 25)

#define RCC_CFGR_SW_PLL   (0x2u << 0)
#define RCC_CFGR_SWS_PLL  (0x2u << 2)
#define RCC_CFGR_SWS_MASK (0x3u << 2)
#define RCC_CFGR_HPRE_DIV1  (0x0u << 4)
#define RCC_CFGR_PPRE1_DIV2 (0x4u << 8)   /* APB1 <= 36 MHz */
#define RCC_CFGR_PPRE2_DIV1 (0x0u << 11)  /* APB2 = 72 MHz  */
#define RCC_CFGR_ADCPRE_DIV6 (0x2u << 14) /* ADC = PCLK2/6 = 12 MHz */
#define RCC_CFGR_PLLSRC_HSE (1u << 16)
#define RCC_CFGR_PLLMULL9   (0x7u << 18)  /* x9: 8 MHz -> 72 MHz */
#define RCC_CFGR_USBPRE_MASK (0x3u << 22) /* 00 = PLL/1,5 -> 48 MHz no USB */

/* APB2ENR */
#define RCC_APB2ENR_AFIOEN  (1u << 0)
#define RCC_APB2ENR_IOPAEN  (1u << 2)
#define RCC_APB2ENR_IOPBEN  (1u << 3)
#define RCC_APB2ENR_IOPCEN  (1u << 4)
#define RCC_APB2ENR_ADC1EN  (1u << 9)
#define RCC_APB2ENR_TIM1EN  (1u << 11)
#define RCC_APB2ENR_USART1EN (1u << 14)
/* APB1ENR (os bits de APB1RSTR/APB2RSTR ficam na mesma posicao) */
#define RCC_APB1ENR_TIM2EN  (1u << 0)
#define RCC_APB1ENR_USBEN   (1u << 23)
/* AHBENR */
#define RCC_AHBENR_DMA1EN   (1u << 0)

/* ------------------------------ ADC ----------------------------- */
typedef struct {
    __IO uint32_t SR;        /* 0x00 */
    __IO uint32_t CR1;       /* 0x04 */
    __IO uint32_t CR2;       /* 0x08 */
    __IO uint32_t SMPR1;     /* 0x0C canais 10..17 */
    __IO uint32_t SMPR2;     /* 0x10 canais 0..9 */
    __IO uint32_t JOFR[4];   /* 0x14 */
    __IO uint32_t HTR;       /* 0x24 */
    __IO uint32_t LTR;       /* 0x28 */
    __IO uint32_t SQR1;      /* 0x2C L[23:20] = n conversoes - 1 */
    __IO uint32_t SQR2;      /* 0x30 */
    __IO uint32_t SQR3;      /* 0x34 SQ1[4:0] */
    __IO uint32_t JSQR;      /* 0x38 */
    __IO uint32_t JDR[4];    /* 0x3C */
    __IO uint32_t DR;        /* 0x4C */
} ADC_TypeDef;
#define ADC1 ((ADC_TypeDef *)0x40012400u)
#define ADC_CR2_ADON     (1u << 0)
#define ADC_CR2_CAL      (1u << 2)
#define ADC_CR2_RSTCAL   (1u << 3)
#define ADC_CR2_DMA      (1u << 8)
#define ADC_CR2_EXTSEL_T1CC1 (0x0u << 17)
#define ADC_CR2_EXTTRIG  (1u << 20)

/* ------------------------------ DMA ----------------------------- */
typedef struct {
    __IO uint32_t ISR;       /* 0x00 */
    __IO uint32_t IFCR;      /* 0x04 */
} DMA_TypeDef;
typedef struct {
    __IO uint32_t CCR;       /* 0x08 + 20*(n-1) */
    __IO uint32_t CNDTR;
    __IO uint32_t CPAR;
    __IO uint32_t CMAR;
} DMA_Channel_TypeDef;
#define DMA1          ((DMA_TypeDef *)0x40020000u)
#define DMA1_Channel1 ((DMA_Channel_TypeDef *)0x40020008u)
#define DMA_ISR_TCIF1   (1u << 1)
#define DMA_IFCR_CH1    (0xFu << 0)
#define DMA_CCR_EN      (1u << 0)
#define DMA_CCR_MINC    (1u << 7)
#define DMA_CCR_PSIZE16 (1u << 8)
#define DMA_CCR_MSIZE16 (1u << 10)
#define DMA_CCR_PL_HIGH (2u << 12)

/* ----------------------------- TIMx ----------------------------- */
typedef struct {
    __IO uint32_t CR1;       /* 0x00 */
    __IO uint32_t CR2;       /* 0x04 */
    __IO uint32_t SMCR;      /* 0x08 */
    __IO uint32_t DIER;      /* 0x0C */
    __IO uint32_t SR;        /* 0x10 */
    __IO uint32_t EGR;       /* 0x14 */
    __IO uint32_t CCMR1;     /* 0x18 */
    __IO uint32_t CCMR2;     /* 0x1C */
    __IO uint32_t CCER;      /* 0x20 */
    __IO uint32_t CNT;       /* 0x24 */
    __IO uint32_t PSC;       /* 0x28 */
    __IO uint32_t ARR;       /* 0x2C */
    __IO uint32_t RCR;       /* 0x30 */
    __IO uint32_t CCR1;      /* 0x34 */
    __IO uint32_t CCR2;      /* 0x38 */
    __IO uint32_t CCR3;      /* 0x3C */
    __IO uint32_t CCR4;      /* 0x40 */
    __IO uint32_t BDTR;      /* 0x44 so TIM1 */
} TIM_TypeDef;
#define TIM1 ((TIM_TypeDef *)0x40012C00u)
#define TIM2 ((TIM_TypeDef *)0x40000000u)
#define TIM_CR1_CEN      (1u << 0)
#define TIM_CR1_ARPE     (1u << 7)
#define TIM_EGR_UG       (1u << 0)
#define TIM_CCMR1_OC1PE  (1u << 3)
#define TIM_CCMR1_OC1M_PWM1 (6u << 4)
#define TIM_CCER_CC1E    (1u << 0)
#define TIM_CCER_CC1P    (1u << 1)
#define TIM_BDTR_MOE     (1u << 15)

/* ------------------------- USB FS device ------------------------ */
/* USBD compativel com o do STM32F103: registradores de 16 bits em passos
 * de 32, e a memoria de pacotes (PMA, 512 bytes) vista pela CPU com cada
 * meia-palavra a cada 4 bytes (offset x da PMA -> 0x40006000 + 2*x). */
#define USB_BASE      0x40005C00u
#define USB_EPR(n)    (*(volatile uint32_t *)(USB_BASE + 4u * (n)))
#define USB_CNTR      (*(volatile uint32_t *)(USB_BASE + 0x40u))
#define USB_ISTR      (*(volatile uint32_t *)(USB_BASE + 0x44u))
#define USB_DADDR     (*(volatile uint32_t *)(USB_BASE + 0x4Cu))
#define USB_BTABLE    (*(volatile uint32_t *)(USB_BASE + 0x50u))
#define USB_PMA_BASE  0x40006000u
#define USB_CNTR_FRES   (1u << 0)
#define USB_ISTR_EP_ID  0x000Fu
#define USB_ISTR_ESOF   (1u << 8)
#define USB_ISTR_SOF    (1u << 9)
#define USB_ISTR_RESET  (1u << 10)
#define USB_ISTR_SUSP   (1u << 11)
#define USB_ISTR_WKUP   (1u << 12)
#define USB_ISTR_ERR    (1u << 13)
#define USB_ISTR_PMAOVR (1u << 14)
#define USB_ISTR_CTR    (1u << 15)
#define USB_DADDR_EF    (1u << 7)

/* Registrador estendido do CH32F103 (nao existe no STM32): pull-up interno
 * de 1,5k no D+ (PA12). */
#define EXTEN_CTR (*(volatile uint32_t *)0x40023800u)
#define EXTEN_USBD_PU_EN (1u << 1)

/* ----------------------------- FLASH ---------------------------- */
typedef struct {
    __IO uint32_t ACR;       /* 0x00: LATENCY[2:0], PRFTBE bit4 */
    __IO uint32_t KEYR;      /* 0x04 */
    __IO uint32_t OPTKEYR;   /* 0x08 */
    __IO uint32_t SR;        /* 0x0C */
    __IO uint32_t CR;        /* 0x10 */
    __IO uint32_t AR;        /* 0x14 */
} FLASH_TypeDef;
#define FLASH ((FLASH_TypeDef *)0x40022000u)
#define FLASH_ACR_LATENCY_2  (0x2u << 0)  /* 2 wait states p/ 48<SYSCLK<=72MHz */
#define FLASH_ACR_PRFTBE     (1u << 4)
#define FLASH_KEY1           0x45670123u
#define FLASH_KEY2           0xCDEF89ABu
#define FLASH_SR_BSY         (1u << 0)
#define FLASH_SR_PGERR       (1u << 2)
#define FLASH_SR_WRPRTERR    (1u << 4)
#define FLASH_SR_EOP         (1u << 5)
#define FLASH_CR_PG          (1u << 0)
#define FLASH_CR_PER         (1u << 1)
#define FLASH_CR_STRT        (1u << 6)
#define FLASH_CR_LOCK        (1u << 7)
/* CH32F103: depois de apagar/programar, o StdPeriph da WCH (e o firmware
 * original) le o endereco ^ 0x1000 e grava o valor lido neste registrador. */
#define FLASH_CH32_REG34     (*(volatile uint32_t *)0x40022034u)

/* ------------------------- Core: SCB / SysTick ------------------ */
typedef struct {
    __IO uint32_t CTRL;
    __IO uint32_t LOAD;
    __IO uint32_t VAL;
    __IO uint32_t CALIB;
} SysTick_TypeDef;
#define SysTick ((SysTick_TypeDef *)0xE000E010u)
#define SYSTICK_CTRL_ENABLE    (1u << 0)
#define SYSTICK_CTRL_TICKINT   (1u << 1)
#define SYSTICK_CTRL_CLKSOURCE (1u << 2)  /* 1 = clock do processador */

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)

/* NVIC: desabilitar/limpar pendentes (IRQs 0..63) */
#define NVIC_ICER(n) (*(volatile uint32_t *)(0xE000E180u + 4u * (n)))
#define NVIC_ICPR(n) (*(volatile uint32_t *)(0xE000E280u + 4u * (n)))

#endif /* CH32F103_H */
