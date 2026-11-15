#!/usr/bin/env python3
"""Minimal ANSI screen emulator: feed btop's raw pty output, print final screen."""
import re
import sys

data = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
rows, cols = int(sys.argv[2]), int(sys.argv[3])
screen = [[" "] * cols for _ in range(rows)]
r = c = 0
i = 0
tokens = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])|\x1b(.)|(.)|\r|\n|\t")
last = 0
for m in tokens.finditer(data):
    csi, cmd, other, ch = m.group(1), m.group(2), m.group(3), m.group(4)
    if ch is not None:
        if 0 <= r < rows and 0 <= c < cols:
            screen[r][c] = ch
        c += 1
    elif m.group(0) == "\n":
        r += 1
        c = 0
    elif m.group(0) == "\r":
        c = 0
    elif m.group(0) == "\t":
        c = (c // 8 + 1) * 8
    elif other:
        continue
    elif cmd in ("H", "f"):
        parts = csi.split(";")
        r = (int(parts[0]) - 1) if parts and parts[0] else 0
        c = (int(parts[1]) - 1) if len(parts) > 1 and parts[1] else 0
    elif cmd == "A":
        r -= int(csi or 1)
    elif cmd == "B":
        r += int(csi or 1)
    elif cmd == "C":
        c += int(csi or 1)
    elif cmd == "D":
        c -= int(csi or 1)
    elif cmd == "J":
        mode = csi or "2"
        if mode == "2":
            screen = [[" "] * cols for _ in range(rows)]
            r = c = 0
    elif cmd == "K":
        mode = csi or "0"
        if mode == "0":
            for x in range(c, cols):
                screen[r][x] = " "
        elif mode in ("1", "2"):
            for x in range(0, cols):
                screen[r][x] = " "

for line in screen:
    print("".join(line).rstrip())
