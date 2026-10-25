# Makefile — firmware do DSO152 (CH32F103 / Cortex-M3)
#
# Uso:
#   make            -> compila build/dso152_app.{elf,bin,hex}
#   make size       -> mostra o tamanho das secoes
#   make disasm     -> desassembla o elf em build/dso152_app.lst
#   make clean      -> limpa build/
#
# O artefato a arrastar para o drive "DSO BOOT" e build/dso152_app.bin
# (binario cru, com a tabela de vetores em 0x08003C00).
#
# Requer o GNU Arm Embedded Toolchain (arm-none-eabi-*). Veja o README.

TARGET  := dso152_app
BUILD   := build

# Toolchain (pode ser sobrescrito: make CROSS=/caminho/arm-none-eabi-)
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

LDSCRIPT := ldscript/$(TARGET).ld

SRCS := \
    src/startup.c \
    src/system.c \
    src/libc_min.c \
    src/scope.c \
    src/usb_cdc.c \
    src/lcd.c \
    src/view.c \
    src/zerocal.c \
    src/main.c

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

CPU    := -mcpu=cortex-m3 -mthumb
CFLAGS := $(CPU) -std=c11 -Os -g3 \
          -Wall -Wextra -Wundef -Wshadow \
          -ffreestanding -ffunction-sections -fdata-sections \
          -fno-common -fno-builtin \
          -fno-tree-loop-distribute-patterns \
          -Iinclude
# -ffreestanding + -fno-tree-loop-distribute-patterns: impedem o GCC de trocar
# os loops de copia/zeragem da startup por chamadas a memcpy/memset (que nao
# existem com -nostdlib e quebrariam a ligacao).
LDFLAGS := $(CPU) -T$(LDSCRIPT) -nostartfiles -nostdlib \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

.PHONY: all size disasm clean
all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex size

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

disasm: $(BUILD)/$(TARGET).elf
	$(OBJDUMP) -d -S $< > $(BUILD)/$(TARGET).lst
	@echo "-> $(BUILD)/$(TARGET).lst"

clean:
	rm -rf $(BUILD)
