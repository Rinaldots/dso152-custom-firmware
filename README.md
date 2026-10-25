# DSO152 Hack — firmware customizado

Firmware bare-metal para o osciloscópio de bolso **FNIRSI DSO152** (MCU
CH32F103, compatível com STM32F103 / Cortex-M3), com envio das amostras do ADC
ao PC por USB CDC e um plotter ao vivo em Python.

> Projeto de engenharia reversa e firmware sobre hardware próprio. Veja
> [`CLAUDE.md`](CLAUDE.md) para todo o levantamento de hardware feito a partir
> do firmware oficial, e [`include/dso152_pins.h`](include/dso152_pins.h) para a
> pinagem (com marcações `[C]` confirmado / `[P]` provável).

## ⚠️ Leia antes de gravar

- Gravar um `.bin` diferente do oficial **pode danificar o aparelho de forma
  irreversível** (aviso do manual). Vá com calma e pelo caminho mínimo.
- **Tenha um WCH-Link ou ST-Link** ligado ao SWD (PA13/PA14) para recuperar.
  O firmware mantém o SWD vivo de propósito.
- O **bootloader** "DSO BOOT" (`0x08000000`–`0x08003BFF`) **não deve ser
  apagado**. O app vive a partir de `0x08003C00`.
- Para **voltar ao original**, arraste o `DSO152_APP_V0.1.bin` oficial no
  drive DSO BOOT. Ele **não está neste repositório** (é da FNIRSI): baixe do
  site da FNIRSI e guarde em `firmware_original/`, pasta que o `.gitignore`
  deixa fora do git.
- O DSO BOOT **aceita um .bin próprio**: o "hello" (888 bytes, `dso152_app.bin`)
  gravou e rodou, então ele não exige o tamanho nem o nome do oficial.
- Ainda **não está confirmado** se a flash total é 64 KB ou 128 KB (o linker usa
  49 KB, seguro para ambos).

## Estrutura

```
dso152-hack/
├── CLAUDE.md               levantamento de hardware + roteiro
├── Makefile                build do firmware (arm-none-eabi)
├── ldscript/dso152_app.ld  linker: FLASH @ 0x08003C00, RAM 20 KB
├── include/
│   ├── dso152_pins.h       pinagem extraída do firmware oficial
│   ├── gain_cal.h          escala/zero por faixa (gerado por pc/calibrate.py)
│   ├── ch32f103.h          registradores (RCC/GPIO/ADC/DMA/TIM/USB/SysTick...)
│   ├── lcd.h               API do ST7789
│   ├── scope.h             API da aquisição
│   ├── usb_cdc.h           API da serial USB
│   ├── view.h              API da tela de osciloscópio
│   └── zerocal.h           API dos zeros por faixa
├── src/
│   ├── startup.c           tabela de vetores + Reset_Handler + VTOR
│   ├── system.c            clock (HSI no boot, depois HSE 8 MHz x9 = 72 MHz) + SysTick 1 ms
│   ├── libc_min.c          memset/memcpy mínimos (build freestanding)
│   ├── scope.c             ganho, AC/DC, captura TIM1_CC1 -> ADC1 -> DMA, calibração 1 kHz
│   ├── usb_cdc.c           USB CDC-ACM mínimo (porta COM virtual), por polling
│   ├── lcd.c               driver do ST7789 (init do original) + fonte 5x7
│   ├── view.c              grade, forma de onda com trigger, frequência e textos
│   ├── zerocal.c           auto-calibração do zero (mede e grava na flash)
│   └── main.c              captura em blocos, tela, frames para o PC, comandos, botões
├── firmware_original/      .bin oficial, só na cópia local (fora do git)
├── tools/                  scripts de análise do .bin (capstone)
├── docs/                   manual do DSO152, só na cópia local (fora do git)
└── pc/
    ├── plot.py             plotter serial ao vivo, em volts (pyserial + matplotlib)
    ├── calibrate.py        mede as 11 faixas pelo borne de calibração
    └── gain_cal.json       resultado da calibração (usado pelo plot.py)
```

## Toolchain

Este ambiente **não** tem o compilador ARM instalado. Para compilar, instale o
**GNU Arm Embedded Toolchain** (`arm-none-eabi-gcc`) e o `make`:

- **Windows:** baixe o "Arm GNU Toolchain" (arm-none-eabi) da Arm e adicione ao
  PATH; para o `make`, use o do MSYS2 (`pacman -S make`) ou o `mingw32-make`.
- **Linux (Debian/Ubuntu):** `sudo apt install gcc-arm-none-eabi make`
- **macOS:** `brew install --cask gcc-arm-embedded` e `make` (via Xcode CLT).

Confirme com: `arm-none-eabi-gcc --version`.

## Compilar

```bash
make            # gera build/dso152_app.bin (e .hex, .elf) + mostra o tamanho
make disasm     # build/dso152_app.lst (conferir vetores/VTOR)
make clean
```

O artefato a gravar é **`build/dso152_app.bin`** (binário cru, vetores em
`0x08003C00`).

## Gravar (método DSO BOOT)

1. Com o aparelho **desligado**, segure **OK (aperte a roda) + POWER**.
2. Deve aparecer o drive USB **"DSO BOOT"**.
3. Arraste `build/dso152_app.bin` para o drive.
4. Reinicie o aparelho: segure **POWER** por ~1 s, como no original.

**Uso:** a tela mostra a forma de onda e, ao mesmo tempo, o firmware envia os
dados pela USB-C. Ligue o cabo no PC: aparece uma porta COM nova ("Dispositivo
serial USB", VID/PID 1209:0001; o Windows 10/11 usa o driver próprio, sem
instalar nada).

Na tela:

- Grade de 12×8 divisões de 25 px, igual à do original.
- **Em cima:** V/div (`1V`), t/div (`500us`), trigger (`A/600mV` = modo AUTO,
  borda de subida, nível 600 mV) e `RUN`/`WAIT`/`STOP`.
- **Embaixo:** Vpp, frequência medida, posição do 0 V (`-2V`), `DC`/`AC` e um
  ponto de USB (cinza = sem cabo, branco = enumerado, verde = porta aberta no
  PC, enviando).
- Marcador ciano na margem esquerda: onde está o 0 V. Marcador laranja na
  direita: o nível de trigger.
- As faixas são as do original (10 mV/div a 20 V/div, ponta em 1X), com a
  escala real medida por `pc/calibrate.py` (veja abaixo).

Botões, no estilo do original (pinos do código do Atlan). A roda escolhe o
parâmetro, que fica destacado na tela, e CIMA/BAIXO ajustam:

| Botão              | Função                                                         |
|--------------------|----------------------------------------------------------------|
| roda esq. / dir.   | escolhe o parâmetro: V/div, t/div, posição ou nível de trigger |
| CIMA / BAIXO       | aumenta / diminui o parâmetro (segure para repetir)            |
| OK (aperta a roda) | alterna AC / DC                                                |
| MODE               | curto: trigger AUTO → NORMAL → SINGLE; longo: borda ↑/↓        |
| RUN                | congela / continua; no SINGLE, arma de novo; segurado 2 s: auto-calibração do zero |
| AUTO               | curto: nível de trigger no meio do sinal; longo: tudo no padrão |

Modos de trigger: **AUTO** mostra sempre (alinhado quando dispara), **NORMAL**
só atualiza a tela quando dispara (`WAIT` enquanto espera) e **SINGLE** para
no primeiro disparo. Posição e nível de trigger andam de 1 px (segurando por
2 s, de 5 px). O USB recebe todos os blocos, disparando ou não.

O **borne de calibração** (PA0) solta uma onda quadrada de 1 kHz (0 a 3,3 V),
boa para o primeiro teste: ligue a ponta de prova nele.

**Para desligar:** solte o POWER por pelo menos 0,5 s e depois segure o POWER
por **3 s** (apaga o backlight e solta o PC10). Alternativa: segure **OK** e
aperte o **furo de reset** para cair no DSO BOOT.

Deu errado? Recupere pela SWD com o WCH-Link/ST-Link, ou regrave o firmware
oficial (`DSO152_APP_V0.1.bin`, do site da FNIRSI) pelo DSO BOOT.

## Plotter no PC

```bash
pip install pyserial matplotlib
python pc/plot.py --list            # descobre a porta (COMx / ttyACMx)
python pc/plot.py --port COM5       # abre o gráfico ao vivo
python pc/plot.py --port COM5 --gain 6 --rate 50000 --n 1024
python pc/plot.py --port COM5 --probe 10     # ponta em 10X
```

O eixo Y mostra a **tensão na ponta**, com ±4 divisões no V/div da faixa e o
0 V no meio, como na tela. A escala vem de `pc/gain_cal.json`; sem ele, valem
os números de projeto do original.

O firmware só envia com a porta aberta no PC (DTR). Cada frame é um bloco de
`n` amostras capturado de uma vez: sync `A5 5A`, contagem, faixa (bit 7 = AC),
taxa, zero da faixa (contagem com 0 V na ponta), amostras de 12 bits, CRC8. O
mesmo spec está no topo de `pc/plot.py` e de `src/main.c`; **mantenha firmware
e PC em sincronia**.

Comandos que o PC pode mandar (texto, um por linha; o `plot.py` manda pelas
opções acima):

| Comando  | Faixa         | Efeito                                              |
|----------|---------------|-----------------------------------------------------|
| `G<n>`   | 0..10         | faixa: 10mV, 20mV, 50mV, 100mV, 200mV, 500mV, 1V, 2V, 5V, 10V, 20V por divisão (padrão 6 = 1 V) |
| `R<hz>`  | 10..800000    | taxa de amostragem (padrão 50000)                   |
| `N<n>`   | 16..2048      | amostras por frame (padrão 600)                     |
| `C<0/1>` | 0 ou 1        | nível do PA3: 1 = DC, 0 = AC (medido)               |

## Auto-calibração do zero (no aparelho)

1. Segure **RUN por 2 s**: abre a página de auto-calibração.
2. Encoste a ponta no jacaré (0 V na entrada) e aperte **OK** (a roda).
3. Em ~5 s ele mede o zero das 11 faixas, em DC e em AC, e grava na flash
   (página 0x0800F000, que o firmware original não usa). RUN cancela antes de
   começar.

Testado no aparelho: grava e, depois de religar, o traço fica no 0 V em DC e
em AC. Se houver sinal na entrada, ele avisa e não grava. O zero gravado vale para a
tela e vai em cada frame do USB, então o `plot.py` também usa. A escala (V/div)
continua a da calibração do PC, abaixo.

## Calibração das faixas (no PC)

```bash
python pc/calibrate.py --port COM5
```

Com a ponta em **1X** no borne de calibração (jacaré no GND) e o `plot.py`
fechado, o script passa por todas as faixas e mede o zero (em DC e AC) e as
contagens por volt de cada uma. Ele grava `pc/gain_cal.json` (para o
`plot.py`) e `include/gain_cal.h` (para o firmware: recompile e grave). Nas
faixas sensíveis, em que os 3,3 V saturam o amplificador, a escala sai da
razão do relé medida na faixa de 200 mV/div.

Medido neste aparelho (2026-10-25): as faixas com o relé (500 mV a 20 V/div)
dão ~99 contagens por divisão, como no projeto do original, menos a de 2 V/div
(79). As sem o relé (10 a 200 mV/div) dão ~87. O zero em AC das três faixas
mais sensíveis não dá para medir com a quadrada (satura) e usa o de DC.

## Ferramentas de análise (`tools/`)

Trabalham sobre o firmware oficial, que não vem no repositório: coloque o
`DSO152_APP_V0.1.bin` em `firmware_original/`.

```bash
pip install capstone
python tools/scan.py  firmware_original/DSO152_APP_V0.1.bin   # periféricos
python tools/calls.py firmware_original/DSO152_APP_V0.1.bin   # chamadas StdPeriph
python tools/gpio_direct.py firmware_original/DSO152_APP_V0.1.bin
python tools/dis.py   firmware_original/DSO152_APP_V0.1.bin 0x080044A4 0x80
```

## Roteiro (de `CLAUDE.md`)

1. ✅ Projeto: Makefile + linker em `0x08003C00`.
2. ✅ Firmware "hello": gravação pelo DSO BOOT, backlight (PC9, ativo em 0), power-hold (PC10) e botão POWER (PC11) validados no aparelho.
3. ✅ Driver do ST7789 e forma de onda na tela junto com o USB: confirmado no aparelho, com V/div calibrado, posição e trigger (nível, borda, AUTO/NORMAL/SINGLE).
4. ✅ ADC + DMA + TIM1 e envio por USB CDC: testado no aparelho (porta COM + quadrada de 1 kHz no `plot.py`).
5. ✅/⬜ `pc/plot.py`: comandos e eixo de tempo prontos; falta calibrar `GAIN_VOLTS_PER_DIV`.
6. ⬜ Mapear botões e confirmar pinos `[P]`.
