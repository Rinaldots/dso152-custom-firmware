#!/usr/bin/env python3
"""
calibrate.py - calibra as 11 faixas de ganho do DSO152 pelo borne de calibracao.

As faixas e os rotulos sao os do firmware original (tabela de V/div na .data
dele): G0..G10 = 10mV, 20mV, 50mV, 100mV, 200mV, 500mV, 1V, 2V, 5V, 10V, 20V
por divisao. No projeto do original, toda faixa da 100 contagens do ADC por
divisao (25 px/div, 4 contagens por pixel); este script mede o valor real.

Montagem: ponta de prova em 1X, ponta no borne de calibracao (quadrada de
1 kHz, 0 a Vcal ~ 3,3 V) e o jacare no GND. Firmware novo rodando, USB ligado
e o plot.py fechado (a porta precisa estar livre).

O que mede, por faixa:
    offset DC = contagens com 0 V na ponta (nivel baixo da quadrada, em DC)
    offset AC = contagens do 0 V em AC (media da quadrada, em AC)
    cpv       = contagens por volt na ponta = (alto - baixo) / Vcal
Nas faixas em que os 3,3 V saturam o ADC, o cpv sai da estrutura do front-end:
cada faixa e um ponto do mux (PA4/PA5/PA6) com ou sem o rele (PA2), entao
cpv(rele A, mux m) = cpv(rele B, mux m) * razao do rele. Se nem isso der, usa
a razao nominal entre os V/div da faixa medida mais proxima.

Antes, descobre qual nivel do PA3 (comando C) e o DC: no DC o nivel baixo da
quadrada e o 0 V; no AC a quadrada fica centrada no 0 V.

Gera pc/gain_cal.json (usado pelo plot.py) e include/gain_cal.h (usado pelo
firmware; recompile e grave depois).

Uso:
    python pc/calibrate.py --port COM17
    python pc/calibrate.py --port COM17 --vcal 3.28
    python pc/calibrate.py --nominal        # so escreve os valores de projeto
"""
import argparse
import datetime
import json
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from plot import FrameReader  # noqa: E402

RATE = 50000          # 50 kS/s: ~20 periodos de 1 kHz por frame de 1024
N = 1024
# O op-amp do front-end satura antes do ADC: o nivel alto trava em ~3812
# contagens (~3,07 V) nas faixas sensiveis. Zero fica em ~2085.
CLIP_LO, CLIP_HI = 400, 3700
REF_GAIN = 7          # 2 V/div: nao satura com 3,3 V; usada para achar o DC
DC_DEFAULT = 1        # no original, a flag de acoplamento 0 (padrao) poe PA3 = 1

# V/div de cada faixa, na ordem da tabela do original
VDIV = [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0]
NOMINAL_OFFSET = 2048
NOMINAL_COUNTS_PER_DIV = 100

# copia da GAIN_TABLE de include/dso152_pins.h: {PA2, PA4, PA5, PA6}
GAIN_TABLE = [
    (0, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 1, 0), (0, 0, 0, 1),
    (1, 0, 0, 0), (1, 1, 0, 0), (1, 0, 1, 0), (1, 1, 1, 0), (1, 0, 0, 1), (1, 1, 0, 1),
]
RELAY = [g[0] for g in GAIN_TABLE]
MUX = [g[1] + 2 * g[2] + 4 * g[3] for g in GAIN_TABLE]


def send(ser, cmd):
    ser.write((cmd + "\n").encode())


def grab(ser, gain, frames=8, settle=0.4, timeout=5.0):
    """Frames da faixa pedida, depois de deixar o rele e o filtro assentarem."""
    time.sleep(settle)
    ser.reset_input_buffer()
    reader = FrameReader()
    got = []
    t0 = time.time()
    while len(got) < frames and time.time() - t0 < timeout:
        chunk = ser.read(max(1, ser.in_waiting))
        for fr in reader.feed(chunk):
            if fr["gain"] == gain:
                got.append(fr["samples"])
    if len(got) < frames:
        sys.exit(f"so recebi {len(got)} frames da faixa G{gain}; o firmware esta rodando e a porta livre?")
    return got[2:] if len(got) > 4 else got     # descarta os primeiros


def plateau(vals, swing):
    """Media do patamar: amostras perto da mediana (tira as das bordas). A media
    de muitas amostras com ruido da resolucao abaixo de 1 contagem."""
    med = statistics.median(vals)
    near = [v for v in vals if abs(v - med) <= max(2.0, swing / 4)]
    return statistics.fmean(near)


def levels(frames):
    """Nivel baixo e alto da quadrada (patamares) e a media geral."""
    lows, highs, means = [], [], []
    for s in frames:
        lo, hi = min(s), max(s)
        mid = (lo + hi) / 2
        a = [v for v in s if v < mid]
        b = [v for v in s if v >= mid]
        if hi - lo < 6 or not a or not b:
            a = b = s                           # sem quadrada visivel
        lows.append(plateau(a, hi - lo))
        highs.append(plateau(b, hi - lo))
        means.append(statistics.fmean(s))
    return statistics.fmean(lows), statistics.fmean(highs), statistics.fmean(means)


def saturated(lo, hi):
    return hi >= CLIP_HI or lo <= CLIP_LO


def find_dc(ser):
    meas = {}
    send(ser, f"G{REF_GAIN}")
    for c in (0, 1):
        send(ser, f"C{c}")
        lo, hi, mean = levels(grab(ser, REF_GAIN, settle=0.8))
        meas[c] = (lo, hi, mean)
        print(f"  C{c}: baixo={lo:7.1f}  alto={hi:7.1f}  media={mean:7.1f}")
    # no DC, o nivel baixo (0 V) coincide com a media do modo AC (que tambem e 0 V)
    err0 = abs(meas[0][0] - meas[1][2])
    err1 = abs(meas[1][0] - meas[0][2])
    dc = 0 if err0 < err1 else 1
    if abs(err0 - err1) < 0.25 * abs(meas[0][1] - meas[0][0]):
        print(f"  aviso: os dois niveis do PA3 dao quase o mesmo resultado; assumindo C{DC_DEFAULT} como DC")
        dc = DC_DEFAULT
    print(f"  -> DC = C{dc}")
    return dc, meas


def measure(ser, vcal):
    send(ser, f"R{RATE}")
    send(ser, f"N{N}")

    print("Acoplamento (PA3):")
    dc, coupling = find_dc(ser)

    print(f"Faixas (Vcal = {vcal:.3f} V):")
    raw = []
    for g in range(11):
        send(ser, f"G{g}")
        send(ser, f"C{dc}")
        lo, hi, _ = levels(grab(ser, g, settle=0.6))
        send(ser, f"C{1 - dc}")
        ac_lo, ac_hi, ac_mean = levels(grab(ser, g, settle=0.8))
        clip = saturated(lo, hi)
        cpv = None if clip or hi - lo < 4 else (hi - lo) / vcal
        # em AC a quadrada fica centrada no 0 V; cortada, a media nao vale
        ac_ok = not saturated(ac_lo, ac_hi)
        raw.append({"gain": g, "low": lo, "high": hi, "ac_zero": ac_mean if ac_ok else lo,
                    "clipped": clip, "cpv": cpv, "derived": "" if ac_ok else "zeroAC=DC"})
        cpv_txt = f"{cpv:8.2f}" if cpv else "  satura" if clip else "     ---"
        print(f"  G{g:<2} ({VDIV[g]:g} V/div) rele={RELAY[g]} mux={MUX[g]}  baixo={lo:7.1f}"
              f"  alto={hi:7.1f}  zeroAC={ac_mean:7.1f}{'' if ac_ok else '(satura)'}  cont/V={cpv_txt}")
    send(ser, f"C{dc}")

    # faixas que saturam: razao do rele num mux medido nos dois grupos
    by = {(RELAY[r["gain"]], MUX[r["gain"]]): r for r in raw}
    ratio = None
    for m in range(8):
        a, b = by.get((0, m)), by.get((1, m))
        if a and b and a["cpv"] and b["cpv"]:
            ratio = a["cpv"] / b["cpv"]            # grupo 0 / grupo 1
            print(f"Razao do rele (mux {m}): grupo0/grupo1 = {ratio:.3f}")
            break
    measured = [r for r in raw if r["cpv"]]
    for r in raw:
        if r["cpv"] is None and ratio:
            other = by.get((1 - RELAY[r["gain"]], MUX[r["gain"]]))
            if other in measured:
                r["cpv"] = other["cpv"] * (ratio if RELAY[r["gain"]] == 0 else 1 / ratio)
                r["derived"] = (r["derived"] + " cpv=rele").strip()
    for r in raw:
        if r["cpv"] is None and measured:
            near = min(measured, key=lambda m: abs(m["gain"] - r["gain"]))
            r["cpv"] = near["cpv"] * VDIV[near["gain"]] / VDIV[r["gain"]]
            r["derived"] = (r["derived"] + " cpv=nominal").strip()
    return dc, coupling, raw


def nominal():
    raw = [{"gain": g, "low": NOMINAL_OFFSET, "ac_zero": NOMINAL_OFFSET,
            "cpv": NOMINAL_COUNTS_PER_DIV / VDIV[g], "derived": "projeto"} for g in range(11)]
    return DC_DEFAULT, {}, raw


def main():
    ap = argparse.ArgumentParser(description="Calibracao das faixas de ganho do DSO152")
    ap.add_argument("--port", help="porta serial (ex.: COM17)")
    ap.add_argument("--vcal", type=float, default=3.30, help="nivel alto do borne de calibracao (V)")
    ap.add_argument("--nominal", action="store_true",
                    help="nao mede: escreve os valores de projeto do original (100 contagens/div)")
    args = ap.parse_args()

    if args.nominal:
        dc, coupling, raw = nominal()
    else:
        if not args.port:
            ap.error("informe --port (ou use --nominal)")
        import serial
        ser = serial.Serial(args.port, 115200, timeout=0.05)
        try:
            dc, coupling, raw = measure(ser, args.vcal)
        finally:
            ser.close()

    ranges = [{"gain": r["gain"], "vdiv": VDIV[r["gain"]], "offset_dc": round(r["low"], 1),
               "offset_ac": round(r["ac_zero"], 1), "cpv": round(r["cpv"], 3) if r["cpv"] else None,
               "derived": r["derived"]} for r in raw]
    cal = {
        "date": datetime.datetime.now().isoformat(timespec="seconds"),
        "source": "nominal" if args.nominal else "medido",
        "vcal": None if args.nominal else args.vcal,
        "dc_level": dc,
        "coupling_check": {f"C{c}": dict(zip(("low", "high", "mean"), v)) for c, v in coupling.items()},
        "ranges": ranges,
    }
    with open(os.path.join(HERE, "gain_cal.json"), "w", encoding="utf-8") as f:
        json.dump(cal, f, indent=2)
    write_header(cal)

    print("\nFaixa   V/div  zeroDC  zeroAC   cont/V  cont/div")
    for e in ranges:
        if e["cpv"]:
            print(f"  G{e['gain']:<2} {e['vdiv']:6g} {e['offset_dc']:7.1f} {e['offset_ac']:7.1f}"
                  f" {e['cpv']:8.2f} {e['cpv'] * e['vdiv']:8.1f}  {e['derived']}")
        else:
            print(f"  G{e['gain']:<2} {e['vdiv']:6g}  sem calibracao")
    print("\nGravado pc/gain_cal.json e include/gain_cal.h; recompile o firmware.")


def write_header(cal):
    rows = []
    for e in cal["ranges"]:
        vdiv_uv = int(round(e["vdiv"] * 1e6))
        cpv = e["cpv"] or NOMINAL_COUNTS_PER_DIV / e["vdiv"]
        px_q16 = int(round(25.0 / (e["vdiv"] * cpv) * 65536))   # pixels por contagem, Q16
        uv_count = int(round(1e6 / cpv))                         # uV por contagem
        rows.append(f"    {{ {int(round(e['offset_dc'])):4d}, {int(round(e['offset_ac'])):4d},"
                    f" {vdiv_uv:8d}u, {px_q16:6d}u, {uv_count:7d}u }},"
                    f"  /* G{e['gain']} {e['vdiv']:g} V/div{' (' + e['derived'] + ')' if e['derived'] else ''} */")
    origin = ("valores de projeto do original (100 contagens/div), sem medir"
              if cal["source"] == "nominal" else f"medido com Vcal = {cal['vcal']:.3f} V")
    text = f"""/*
 * gain_cal.h — calibracao das faixas de ganho, gerada por pc/calibrate.py em
 * {cal['date']}: {origin}.
 * Nao edite a mao: rode o script de novo.
 */
#ifndef GAIN_CAL_H
#define GAIN_CAL_H

#include <stdint.h>

#define GAIN_CAL_MEASURED {0 if cal['source'] == 'nominal' else 1}
#define GAIN_CAL_DC_LEVEL {cal['dc_level']}   /* nivel do PA3 que da acoplamento DC */

/* Por faixa (G0..G10, rotulos do original): zero em DC e em AC (contagens com
 * 0 V na ponta), V/div em uV, pixels por contagem em Q16 (25 px por divisao)
 * e uV por contagem. */
static const struct {{
    int16_t  off_dc;
    int16_t  off_ac;
    uint32_t vdiv_uv;
    uint32_t px_q16;
    uint32_t uv_count;
}} GAIN_CAL[11] __attribute__((unused)) = {{
{chr(10).join(rows)}
}};

#endif
"""
    with open(os.path.join(ROOT, "include", "gain_cal.h"), "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    main()
