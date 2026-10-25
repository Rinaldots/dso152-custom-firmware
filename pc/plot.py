#!/usr/bin/env python3
"""
plot.py - plotter ao vivo das amostras enviadas pelo firmware do DSO152.

Le a porta serial virtual (USB CDC) criada pelo firmware novo, decodifica os
frames binarios e plota a forma de onda em tempo real.

Protocolo de frame (little-endian), conforme CLAUDE.md:

    offset  campo         tipo    descricao
    0       sync          u8[2]   0xA5 0x5A
    2       n_amostras    u16     quantidade de amostras u16 que seguem
    4       faixa_ganho   u8      bits 0..6: indice 0..10 da GAIN_TABLE; bit 7: 1 = AC
    5       taxa          u32     taxa de amostragem em Hz
    9       zero          i16     contagem do ADC com 0 V na ponta nessa faixa
    11      amostras      u16[n]  amostras cruas do ADC (0..4095, 12 bits)
    11+2n   crc8          u8      CRC-8 (poly 0x07, init 0x00) de offset 2..11+2n-1

O CRC cobre tudo entre o sync e o proprio crc (ou seja, n_amostras, faixa_ganho,
taxa, zero e as amostras). O zero vem do aparelho (auto-calibracao gravada na
flash, ou o da calibracao do PC); a escala (contagens por volt) vem de
pc/gain_cal.json. Mantenha este spec em sincronia com o firmware.

Comandos PC -> firmware (texto, um por linha): G<0..10> faixa, R<hz> taxa,
N<n> amostras por frame, C<0|1> nivel do PA3 (AC/DC).

Uso:
    python plot.py --list                 # lista as portas seriais
    python plot.py --port COM5            # Windows
    python plot.py --port /dev/ttyACM0    # Linux
    python plot.py --port COM5 --gain 6 --rate 50000 --n 1024

Dependencias:  pip install pyserial matplotlib
(No USB CDC o baud e "virtual" e normalmente ignorado, mas alguns drivers pedem.)
"""
import argparse
import json
import os
import sys

SYNC0, SYNC1 = 0xA5, 0x5A
HDR = 11              # sync(2) + n(2) + faixa(1) + taxa(4) + zero(2)
N_MAX = 2048          # maximo de amostras por frame no firmware

# V/div de cada faixa (indice 0..10), da tabela do firmware original (ponta 1X)
GAIN_VOLTS_PER_DIV = [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0]

# Sem calibracao, vale o projeto do original: 100 contagens por divisao.
CAL_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gain_cal.json")


def load_cal():
    """Contagens por volt de cada faixa, de gain_cal.json se existir."""
    cpv = [100.0 / v for v in GAIN_VOLTS_PER_DIV]
    try:
        with open(CAL_FILE, encoding="utf-8") as f:
            cal = json.load(f)
    except OSError:
        return cpv, "nominal (sem pc/gain_cal.json)"
    for r in cal.get("ranges", []):
        if r.get("cpv"):
            cpv[r["gain"]] = r["cpv"]
    return cpv, f"{cal.get('source', '?')} em {cal.get('date', '?')}"


def crc8(data: bytes, poly: int = 0x07, init: int = 0x00) -> int:
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def list_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        sys.exit("pyserial nao instalado. Rode: pip install pyserial")
    ports = list(list_ports.comports())
    if not ports:
        print("Nenhuma porta serial encontrada.")
        return
    for p in ports:
        print(f"{p.device:12} {p.description}")


class FrameReader:
    """Maquina de estados que extrai frames validos de um fluxo de bytes."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, chunk: bytes):
        """Adiciona bytes e devolve a lista de frames completos e validos."""
        self.buf.extend(chunk)
        frames = []
        while True:
            # procura o sync
            i = self.buf.find(bytes((SYNC0, SYNC1)))
            if i < 0:
                # mantem no maximo 1 byte (pode ser meio-sync)
                if len(self.buf) > 1:
                    del self.buf[:-1]
                break
            if i > 0:
                del self.buf[:i]           # descarta lixo antes do sync
            if len(self.buf) < HDR:
                break                       # cabecalho incompleto
            n = self.buf[2] | (self.buf[3] << 8)
            if n > N_MAX:
                del self.buf[:1]            # sync falso (no meio dos dados)
                continue
            total = HDR + 2 * n + 1         # cabecalho + amostras + crc
            if len(self.buf) < total:
                break                       # frame incompleto, espera mais
            frame = bytes(self.buf[:total])
            if crc8(frame[2:-1]) != frame[-1]:
                del self.buf[:1]            # crc ruim: procura o proximo sync
                continue
            del self.buf[:total]
            gain = frame[4] & 0x7F
            ac = frame[4] >> 7
            rate = int.from_bytes(frame[5:9], "little")
            zero = int.from_bytes(frame[9:11], "little", signed=True)
            samples = [frame[HDR + 2 * k] | (frame[HDR + 1 + 2 * k] << 8) for k in range(n)]
            frames.append({"gain": gain, "ac": ac, "rate": rate, "zero": zero, "samples": samples})
        return frames


def raw_to_volts(raw, gain_idx, zero, cal, probe=1):
    """Contagem crua do ADC -> volts na ponta (x probe para a ponta em 10X)."""
    cpv = cal[min(gain_idx, len(cal) - 1)]
    return [(r - zero) / cpv * probe for r in raw]


def main():
    ap = argparse.ArgumentParser(description="Plotter ao vivo do DSO152")
    ap.add_argument("--port", help="porta serial (ex.: COM5 ou /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--list", action="store_true", help="lista portas e sai")
    ap.add_argument("--gain", type=int, help="faixa de ganho 0..10 (comando G)")
    ap.add_argument("--rate", type=int, help="taxa de amostragem em Hz, 10..800000 (comando R)")
    ap.add_argument("--n", type=int, help="amostras por frame, 16..2048 (comando N)")
    ap.add_argument("--coupling", type=int, choices=(0, 1), help="nivel do PA3, AC/DC (comando C)")
    ap.add_argument("--probe", type=int, choices=(1, 10), default=1, help="ponta em 1X ou 10X")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return
    if not args.port:
        ap.error("informe --port (ou use --list para ver as portas)")

    try:
        import serial
    except ImportError:
        sys.exit("pyserial nao instalado. Rode: pip install pyserial")
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib nao instalado. Rode: pip install matplotlib")

    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    print(f"Lendo {args.port} ... (Ctrl+C para sair)")

    cmds = []
    if args.gain is not None:
        cmds.append(f"G{args.gain}")
    if args.rate is not None:
        cmds.append(f"R{args.rate}")
    if args.n is not None:
        cmds.append(f"N{args.n}")
    if args.coupling is not None:
        cmds.append(f"C{args.coupling}")
    for c in cmds:
        ser.write((c + "\n").encode())

    cal, cal_src = load_cal()
    print(f"Calibracao: {cal_src}")

    reader = FrameReader()
    plt.ion()
    fig, ax = plt.subplots()
    (line,) = ax.plot([], [])
    ax.set_xlabel("tempo (ms)")
    ax.set_ylabel(f"tensao na ponta (V, {args.probe}X)")
    ax.grid(True, alpha=0.3)
    shown_vdiv = None

    try:
        while True:
            # le tudo o que chegou e desenha so o frame mais recente, para o
            # grafico nao ficar atrasado quando o firmware manda mais rapido
            chunk = ser.read(max(1, ser.in_waiting))
            frames = reader.feed(chunk) if chunk else []
            if frames:
                fr = frames[-1]
                volts = raw_to_volts(fr["samples"], fr["gain"], fr["zero"], cal, args.probe)
                dt_ms = 1000.0 / fr["rate"] if fr["rate"] else 1.0
                line.set_data([k * dt_ms for k in range(len(volts))], volts)
                ax.set_xlim(0, max(dt_ms, (len(volts) - 1) * dt_ms))
                vdiv = GAIN_VOLTS_PER_DIV[min(fr["gain"], 10)] * args.probe
                if vdiv != shown_vdiv:              # 8 divisoes, 0 V no meio, como na tela
                    ax.set_ylim(-4 * vdiv, 4 * vdiv)
                    ax.set_yticks([k * vdiv for k in range(-4, 5)])
                    shown_vdiv = vdiv
                vpp = max(volts) - min(volts)
                ax.set_title(f"{vdiv:g} V/div  {'AC' if fr['ac'] else 'DC'}  Vpp={vpp:.3g} V"
                             f"  taxa={fr['rate']} Hz  n={len(volts)}")
            plt.pause(0.001)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


if __name__ == "__main__":
    main()
