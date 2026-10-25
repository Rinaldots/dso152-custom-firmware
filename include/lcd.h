/*
 * lcd.h — ST7789 320x240 no barramento 8080 de 16 bits do DSO152.
 *
 * Dados em PB0..PB15 (GPIOB->ODR), RS = PC13, WR = PC14, RD = PC15 (fica em 1),
 * RST = PA8, CS = PA15 (fica em 0). RGB565, MADCTL = 00 (igual ao original).
 *
 * Coordenadas logicas: X = 0..319 da esquerda para a direita, Y = 0..239 de
 * cima para baixo. Neste painel, com MADCTL = 00, a coluna do controlador
 * (CASET, 0..239) corre na vertical, de baixo para cima, e a linha (RASET,
 * 0..319) na horizontal; lcd_window() faz a conversao. Por isso os pixels de
 * uma janela chegam em X crescente por fora e, dentro de cada X, com Y de y1
 * ate y0 (de baixo para cima).
 */
#ifndef LCD_DRV_H
#define LCD_DRV_H

#include <stdint.h>
#include "ch32f103.h"

#define LCD_W 320u
#define LCD_H 240u

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

#define LCD_RS (1u << 13)
#define LCD_WR (1u << 14)

/* Um pixel; so vale depois de lcd_window(), que deixa o RS em 1 (dados). */
static inline void lcd_put(uint16_t color)
{
    GPIOB->ODR = color;
    GPIOC->BRR = LCD_WR;
    GPIOC->BSRR = LCD_WR;
}

void lcd_init(void);   /* reset + sequencia de init do original + tela preta */
/* Janela logica; depois dela, lcd_put() na ordem descrita acima. */
void lcd_window(unsigned x0, unsigned y0, unsigned x1, unsigned y1);
void lcd_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
/* Texto com fonte 5x7; cada caractere ocupa 6*scale x 8*scale pixels. */
void lcd_text(unsigned x, unsigned y, const char *s, uint16_t fg, uint16_t bg, unsigned scale);

#endif
