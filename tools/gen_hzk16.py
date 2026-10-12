#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 GB2312 16x16 点阵字库 hzk16.bin（标准 HZK16 横排格式）。
- 遍历 GB2312 码位：区码 0xA1..0xF7（87 区），位码 0xA1..0xFE（94 位）
- 每个汉字 16x16 像素 -> 32 字节
- 取模：横向，每行 16 像素打包成 2 字节，字节内 bit7 = 最左像素（高位在左）
- 偏移公式（MCU 端按此读取）：offset = ((区-0xA1)*94 + (位-0xA1)) * 32
- 依赖 Pillow + 系统中文字体（默认用 simhei 黑体）
用法：python gen_hzk16.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT = r"C:\Windows\Fonts\simhei.ttf"
OUT = r"F:\0-1\zero_to_one\tools\hzk16.bin"
SIZE = 16
THRESH = 128


def render(ch):
    img = Image.new("L", (SIZE, SIZE), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, fill=255, font=font)
    px = img.load()
    buf = bytearray()
    for y in range(SIZE):
        byte = 0
        for x in range(SIZE):
            if px[x, y] > THRESH:
                byte |= (0x80 >> (x % 8))
            if x % 8 == 7:
                buf.append(byte)
                byte = 0
        if SIZE % 8 != 0:
            buf.append(byte)
    return bytes(buf)


def main():
    global font
    font = ImageFont.truetype(FONT, SIZE)
    out = bytearray()
    total = 0
    for qu in range(0xA1, 0xF8):       # 87 区
        for wei in range(0xA1, 0xFF):  # 94 位
            try:
                ch = bytes([qu, wei]).decode("gb2312")
            except Exception:
                ch = " "
            out += render(ch)
            total += 1
    with open(OUT, "wb") as f:
        f.write(out)
    print("done: %d chars, %d bytes -> %s" % (total, len(out), OUT))


if __name__ == "__main__":
    main()
