#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 6x8 ASCII 字模头文件 font6x8.h（供 ssd1306.c 小号显示）。

取模约定（与 font8x16.h / HZK16 一致）：
  - 每个字符 8 行，每行 1 字节（共 8 字节）
  - bit7 = 最左列（bit7..bit2 为 6 个有效列，bit1..bit0 留空）
  - 行优先：数组第 0 字节 = 该字符第 0 行（顶部）
绘制时：col 列亮  <=>  byte & (0x80 >> col)
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "..", "Core", "Inc", "font6x8.h")
W, H = 6, 8
FIRST, LAST = 0x20, 0x7E
NUM = LAST - FIRST + 1

# 优先用等宽字体，失败回退默认位图字体
font = None
for path in [
    "C:/Windows/Fonts/consolai.ttf",
    "C:/Windows/Fonts/consola.ttf",
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/simhei.ttf",
]:
    if os.path.exists(path):
        try:
            font = ImageFont.truetype(path, 8)
            print("use font:", path)
            break
        except Exception:
            pass
if font is None:
    font = ImageFont.load_default()
    print("use default font")


def render(ch):
    # 直接 1:1 渲染到 6x8 画布（不放大缩小），保留细笔画与字形区分度。
    # 6x8 太小，放大再缩小必失真；size=8 字体基本 1:1，居中绘制即可。
    tmp = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(tmp)
    d.text((W // 2, H // 2), ch, fill=255, font=font, anchor="mm")
    px = tmp.load()
    data = []
    for y in range(H):
        b = 0
        for x in range(W):
            if px[x, y] > 128:   # bit7 = 最左列
                b |= (0x80 >> x)
        data.append(b)
    return data


lines = []
lines.append("/* 自动生成：6x8 ASCII 字模（Pillow 渲染）。bit7=最左列，行优先。 */")
lines.append("#ifndef FONT6X8_H")
lines.append("#define FONT6X8_H")
lines.append("")
lines.append("#include <stdint.h>")
lines.append("")
lines.append("#define FONT6X8_W 6")
lines.append("#define FONT6X8_H 8")
lines.append("#define FONT6X8_FIRST 0x20")
lines.append("#define FONT6X8_NUM 95")
lines.append("")
lines.append("/* 每个字符 8 字节（6 列 x 8 行），行优先，bit7 = 最左 */")
lines.append("static const uint8_t font6x8[FONT6X8_NUM * FONT6X8_H] = {")
for c in range(FIRST, LAST + 1):
    g = render(chr(c))
    nums = ", ".join("0x%02X" % v for v in g)
    disp = chr(c) if 32 < c < 127 else " "
    lines.append("  /* 0x%02X '%s' */ %s," % (c, disp, nums))
lines.append("};")
lines.append("")
lines.append("#endif /* FONT6X8_H */")

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
print("done: %d chars -> %s" % (NUM, os.path.abspath(OUT)))
