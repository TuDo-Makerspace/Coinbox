#!/usr/bin/env python3
"""
plot_dump.py – visualise Coinbox ADC dumps
   • Figure 1 : Raw ADC values
   • Figure 2 : Averaged ADC values (if present)
   • Y‑axis fixed to ESP32 12‑bit span (0 … 1023)

Usage
-----
$ python plot_dump.py dump.txt
$ python plot_dump.py          # falls back to dump.txt or stdin
"""
import sys, re, matplotlib.pyplot as plt

# ── 1. read file ------------------------------------------------------------
if len(sys.argv) > 1:
    text = open(sys.argv[1], encoding="utf-8").read()
else:
    try:
        text = open("dump.txt", encoding="utf-8").read()
    except FileNotFoundError:
        print("Paste the dump then Ctrl‑D / Ctrl‑Z:")
        text = sys.stdin.read()

# ── 2. extract numbers ------------------------------------------------------
parts = text.split("Averaged", 1)
adc_vals = list(map(int, re.findall(r"\d+", parts[0])))
avg_vals = list(map(int, re.findall(r"\d+", parts[1]))) if len(parts) == 2 else []

# ── 3. raw figure -----------------------------------------------------------
if adc_vals:
    plt.figure("Raw ADC")
    plt.plot(adc_vals)
    plt.ylim(0, 1023)
    plt.title("Raw ADC values")
    plt.xlabel("Sample index")
    plt.ylabel("Counts (0‑1023)")
    plt.grid(True)

# ── 4. averaged figure ------------------------------------------------------
if avg_vals:
    plt.figure("Averaged ADC")
    plt.plot(avg_vals, color="tab:orange")
    plt.ylim(0, 1023)
    plt.title("Averaged ADC values")
    plt.xlabel("Averaged sample index")
    plt.ylabel("Counts (0‑1023)")
    plt.grid(True)

if not (adc_vals or avg_vals):
    print("No numeric data found in input.")
else:
    plt.tight_layout()
    plt.show()
