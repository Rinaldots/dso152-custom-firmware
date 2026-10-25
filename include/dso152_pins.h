/*
 * FNIRSI DSO152 - pinagem extraída do firmware oficial DSO152_APP_V0.1.bin
 * MCU: CH32F103 (compatível STM32F103)
 * App começa em 0x08003C00 (bootloader "DSO BOOT" ocupa 0x08000000-0x08003BFF)
 *
 * Legenda: [C] = confirmado pelo código   [P] = provável, confirmar com multímetro
 *          [G] = bate com o PCB anotado do projeto dso152-breakout (David Given),
 *                que roda código próprio no aparelho
 *          [A] = bate com o código/esquema do Atlan (DSO152-play-game)
 *
 * ATENÇÃO: no firmware oficial, 0x08007F26 escreve em BRR (GPIO_ResetBits) e
 * 0x08007F2A escreve em BSRR (GPIO_SetBits). Uma versão anterior deste header
 * tinha os dois trocados, e por isso PC9/PC10 e a GAIN_TABLE estavam invertidos.
 */
#ifndef DSO152_PINS_H
#define DSO152_PINS_H

#define APP_BASE_ADDR       0x08003C00u   /* linkar o firmware aqui; setar SCB->VTOR */

/* ---------- Display: ST7789 320x240, barramento 8080 16 bits, RGB565 ---------- */
/* LCD_DB0..DB15 = PB0..PB15 (GPIOB->ODR inteiro)                       [C][G] */
/* PB3/PB4 e PA15 são pinos de JTAG: desabilite JTAG (mantendo SWD) via AFIO */
#define LCD_RS_PIN          GPIO_Pin_13   /* PC13  0=comando 1=dado         [C][G] */
#define LCD_WR_PIN          GPIO_Pin_14   /* PC14  strobe de escrita         [C][G] */
#define LCD_RD_PIN          GPIO_Pin_15   /* PC15  strobe de leitura, fica em 1 [C][G] */
#define LCD_RST_PIN         GPIO_Pin_8    /* PA8   reset: 1, 0 (20 ms), 1   [C][G] */
#define LCD_CS_PIN          GPIO_Pin_15   /* PA15  chip select, ativo em 0   [C][G] */
#define LCD_BL_PIN          GPIO_Pin_9    /* PC9   backlight, ATIVO EM 0     [C][G], testado no aparelho */
/* O original apaga o backlight (PC9=1) no init do LCD e só acende (PC9=0)
 * depois de desenhar a tela de abertura.                                    */

/* ---------- Aquisição ---------- */
#define ADC_IN_PIN          GPIO_Pin_1    /* PA1   ADC1_IN1, entrada do sinal[C] */
/* ADC1 disparado por TIM1_CC1 (ExternalTrigConv = T1_CC1); DMA1_Ch1 copia
 * ADC1->DR (0x4001244C) para o buffer em RAM 0x20004104                  [C] */

/* Seleção de ganho (11 faixas, índice 0..10)  PA2/PA4/PA5/PA6            [C] */
#define GAIN_A_PIN          GPIO_Pin_2
#define GAIN_B_PIN          GPIO_Pin_4
#define GAIN_C_PIN          GPIO_Pin_5
#define GAIN_D_PIN          GPIO_Pin_6
/* Tabela do SetGain (0x080050F4) do original: {PA2,PA4,PA5,PA6} por índice */
static const unsigned char GAIN_TABLE[11][4] __attribute__((unused)) = {
    {0,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,1,1,0}, {0,0,0,1},
    {1,0,0,0}, {1,1,0,0}, {1,0,1,0}, {1,1,1,0}, {1,0,0,1}, {1,1,0,1},
};

#define COUPLING_PIN        GPIO_Pin_3    /* PA3   AC/DC (segue flag)        [P] */
#define CAL_OUT_PIN         GPIO_Pin_0    /* PA0   borne de calibração 1 kHz, PWM TIM2_CH1 [C][G] */

/* ---------- Energia ---------- */
#define PWR_HOLD_PIN        GPIO_Pin_10   /* PC10  power-hold, ativo em 1     [C][A], testado no aparelho */
/* O original deixa PC10=0 no init e só faz PC10=1 depois de o POWER ficar
 * ~1 s apertado (código de tecla 0x3C); a partir daí o aparelho se mantém.
 * [A] = esquema do Atlan (github.com/Atlan4/DSO152-play-game): PC10 -> 1k5 ->
 * base do NPN Q4 (MMBT3904), que puxa o gate do MOSFET P Q2 (WPM2015,
 * bateria -> VCC) para GND. O POWER também puxa esse gate, por um diodo.     */
#define PWR_BTN_PIN         GPIO_Pin_11   /* PC11  botão POWER (ativo em 0)  [C][G][A], testado no aparelho */

/* ---------- Botões: 9 entradas com pull-up (ativo em 0) ---------- */
/* AUTO, MODE, CIMA, BAIXO, RUN, roda-esq, roda-OK, roda-dir, POWER
 * Pinos: PA9, PA10, PC3, PC4, PC5, PC6, PC7, PC8, PC11 (POWER = PC11)
 * Ordem de varredura no firmware: PC7, PA9, PC6, PA10, PC8, PC11, PC3, PC5, PC4
 * Códigos de tecla do original (curto/longo): PC7 1/0x0A, PC6 2/0x14,
 *   PA9 3/0x1E, PA10 4/0x28, PC8 5/0x32, PC11 6/0x3C, PC3 7/0x46,
 *   PC5 8/0x50, PC4 9/0x5A
 * Segundo o código do Atlan [A]: AUTO = PC8, MODE = PC7, CIMA = PC6,
 * BAIXO = PA9, RUN = PA10. A roda fica em PC3/PC4/PC5 (OK provavelmente no
 * meio, PC4); o lado de cada um ainda precisa ser conferido no aparelho.   */

/* ---------- Livres / debug ---------- */
/* PA13 = SWDIO, PA14 = SWCLK (porta de debug)
 * PA11/PA12 = USB D-/D+ (usado só pelo bootloader; app oficial não usa USB) */
/* PA7, PC0-PC2, PC12 não aparecem no app - possíveis livres (PB é todo do LCD) */

/* Gravação: desligado, segure OK (roda) + power -> aparece o drive "DSO BOOT";
 * arraste o .bin (binário cru com vetores em 0x08003C00).                   */

#endif
