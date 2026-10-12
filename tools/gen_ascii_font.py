#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 ASCII 8x16 点阵字模 font8x16.h（供 ssd1306.c 的 DrawChar 使用）。
- 覆盖可打印 ASCII 0x20..0x7E（95 个字符）
- 每个字符 8 宽 x 16 高 -> 16 字节
- 取模：横向，每行 8 像素打包成 1 字节，字节内 bit7 = 最左像素（高位在左）
- 与 HZK16(16x16) 取模方向一致，便于中英文混排时基线对齐（都是 16 高）
用法：python gen_ascii_font.py  -> 输出 Core/Inc/font8x16.h
依赖：Pillow + Windows 字体（自动挑选可用等宽字体）
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = r"F:\0-1\zero_to_one\Core\Inc\font8x16.h"
W, H = 8, 16
THRESH = 128

# 优先等宽字体，保证 8 宽格内不挤
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\consola.ttf",   # Consolas
    r"C:\Windows\Fonts\cour.ttf",       # Courier New
    r"C:\Windows\Fonts\simhei.ttf",     # 黑体（含拉丁字形）
    r"C:\Windows\Fonts\arial.ttf",      # Arial
]


def pick_font():
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            # 先用较大字号渲染，再缩放到 8x16，形状更稳
            return ImageFont.truetype(p, 28)
    raise FileNotFoundError("未找到可用字体，请检查 FONT_CANDIDATES")


def render(ch, font):
    # 大画布渲染后裁剪到字形包围盒，再缩放到 (8,16)
    big = Image.new("L", (64, 64), 0)
    d = ImageDraw.Draw(big)
    d.text((4, 2), ch, fill=255, font=font)
    bbox = big.getbbox()
    if bbox is None:
        big = Image.new("L", (8, 16), 0)
    else:
        g = big.crop(bbox)
        # 等比缩放铺满高度，宽度不足则居中
        gw, gh = g.size
        scale = H / gh
        nw = max(1, int(round(gw * scale)))
        nw = min(nw, W)
        g = g.resize((nw, H), Image.LANCZOS)
        big = Image.new("L", (W, H), 0)
        ox = (W - nw) // 2
        big.paste(g, (ox, 0))
    px = big.load()
    buf = bytearray()
    for y in range(H):
        byte = 0
        for x in range(W):
            if px[x, y] > THRESH:
                byte |= (0x80 >> x)        # bit7 = 最左
            if x == 7:
                buf.append(byte)
                byte = 0
    return bytes(buf)


def main():
    font = pick_font()
    out = bytearray()
    first = 0x20
    last = 0x7E
    for code in range(first, last + 1):
        out += render(chr(code), font)

    lines = []
    lines.append("/* 本文件由 tools/gen_ascii_font.py 自动生成，勿手改 */")
    lines.append("/**")
    lines.append(" * @file    font8x16.h")
    lines.append(" * @brief   ASCII 8x16 点阵字模（可打印 0x20..0x7E），供 ssd1306.c DrawChar 使用")
    lines.append(" * @note    取模：横向，每行 8 像素打包 1 字节，bit7 = 最左像素；与 HZK16(16x16) 方向一致")
    lines.append(" */")
    lines.append("#ifndef FONT8X16_H")
    lines.append("#define FONT8X16_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define FONT8X16_W 8")
    lines.append("#define FONT8X16_H 16")
    lines.append("#define FONT8X16_FIRST 0x20")   # 第一个字符码
    lines.append("#define FONT8X16_NUM  95")       # 字符数（0x20..0x7E）
    lines.append("")
    lines.append("/* 每个字符 16 字节（8 列 x 16 行），行优先，bit7 = 最左 */")
    lines.append("static const uint8_t font8x16[FONT8X16_NUM * FONT8X16_H] = {")
    # 每行 12 字节（便于阅读）；n = 字符数 * 高度
    n = 95 * 16
    row_vals = []
    for i in range(0, n, 12):
        row_vals.append(", ".join("0x%02X" % b for b in out[i:i + 12]))
    lines.append(",\n ".join(row_vals))
    lines.append("")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* FONT8X16_H */")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("done: %d chars, %d bytes -> %s" % (95, len(out), OUT))


if __name__ == "__main__":
    main()
