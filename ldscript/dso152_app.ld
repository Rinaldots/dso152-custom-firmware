/*
 * dso152_app.ld — linker script do app do DSO152 (CH32F103 / STM32F103).
 *
 * O app e gravado logo apos o bootloader "DSO BOOT" (0x08000000-0x08003BFF).
 * Por isso a FLASH do app comeca em 0x08003C00.
 *
 * TAMANHO DA FLASH: o total (64 ou 128 KB) ainda nao foi confirmado; tudo
 * aqui cabe nos primeiros 64 KB. O app vai ate 0x0800F000 (45 KB); a pagina
 * seguinte guarda a auto-calibracao do zero e o fim dos 64 KB (0x0800FE00) e
 * onde o firmware original grava as configuracoes dele.
 *
 * RAM: STM32F103C8 = 20 KB em 0x20000000 (coerente com o SP 0x20004F68 do
 * firmware oficial).
 */

ENTRY(Reset_Handler)

MEMORY
{
    /* termina em 0x0800F000: a pagina de 1 KB ali guarda a auto-calibracao do
     * zero (src/zerocal.c), e 0x0800FC00..0x0800FFFF e a area de configuracoes
     * do firmware original */
    FLASH (rx)  : ORIGIN = 0x08003C00, LENGTH = 45K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

/* Topo da pilha no fim da RAM (pilha cresce para baixo). */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* Tabela de vetores primeiro: 1a palavra = SP, 2a = Reset_Handler. */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* glue/ARM->Thumb (normalmente vazio em M3) */
    .glue :
    {
        *(.glue_7) *(.glue_7t)
        *(.eh_frame)
    } > FLASH

    /* LMA de onde o Reset_Handler copia o .data */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Verificacao: garante que o app nao invade a regiao do bootloader. */
    ASSERT(ORIGIN(FLASH) >= 0x08003C00, "FLASH do app nao pode comecar antes de 0x08003C00")
    ASSERT(ORIGIN(FLASH) + LENGTH(FLASH) <= 0x0800F000, "FLASH do app invade a pagina da auto-calibracao")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
