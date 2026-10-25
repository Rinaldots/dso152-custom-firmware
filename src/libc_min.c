/*
 * libc_min.c — memset/memcpy minimos para o build freestanding (-nostdlib).
 *
 * Mesmo com -ffreestanding, o GCC pode emitir chamadas a memset/memcpy (ex.:
 * ao zerar um array local com "= {0}" ou copiar structs). A documentacao do GCC
 * exige que o ambiente freestanding forneca essas funcoes. O Makefile compila
 * com -fno-tree-loop-distribute-patterns, entao os loops abaixo nao viram
 * chamadas recursivas a si mesmos.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}
