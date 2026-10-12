#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 hzk16.bin 烧录到 W25Q（手持实为 GD25Q32）字库区。配合 MCU 端 `flashcn` 命令。

依赖：pip install pyserial
用法：
  python flash_w25q.py COM3                 # 默认 115200，烧 tools/hzk16.bin
  python flash_w25q.py COM3 -b 115200 --bin tools/hzk16.bin

协议（与 Core/Src/console.c 的 flashcn 命令对应）：
  1) 打开串口，发 "flashcn <size>\n"（size = 4 + len(bin)，前 4 字节为魔数 HZK1）
  2) 等待 MCU 回 "FLASCN READY <size>"
  3) 以 60 字节/帧流发 [魔数(4) + bin]，每帧等 MCU 回 '>' 节流
  4) 收 "FLASCN DONE" 完成

说明：字库放在 Flash 地址 [0..3]=魔数"HZK1"，之后是 HZK16 点阵；MCU 端
      W25QXX_ReadGlyph16x16 按 offset = 4 + ((区-0xA1)*94+(位-0xA1))*32 读取。
"""
import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial，请先安装：pip install pyserial")
    sys.exit(1)

MAGIC = b"HZK1"
CHUNK = 60          # 每帧字节数，须 < MCU 端 RX_BUF_SIZE(64)
ACK = b">"


def read_until(ser, token, timeout=30.0):
    """从串口读取，直到出现 token 或超时；返回已收字节。"""
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
            if token in buf:
                return buf
        time.sleep(0.01)
    return buf


def drain_acks(ser, timeout=0.3):
    """吞掉当前已到达的多余 '>'（处理 IDLE 把一帧拆成多帧时多出的 ACK）。"""
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
        elif buf:
            time.sleep(0.05)
            if not ser.in_waiting:
                break
    return buf


def main():
    ap = argparse.ArgumentParser(description="烧录 hzk16.bin 到 W25Q 字库区")
    ap.add_argument("port", help="串口，如 COM3 或 /dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--bin", default=os.path.join(os.path.dirname(__file__), "hzk16.bin"),
                    help="字库 bin 路径（默认 tools/hzk16.bin）")
    args = ap.parse_args()

    if not os.path.exists(args.bin):
        print("字库文件不存在：%s" % args.bin)
        sys.exit(1)

    with open(args.bin, "rb") as f:
        data = f.read()
    size = len(MAGIC) + len(data)
    print("bin=%d B, total=%d B (magic %d + bin %d)" % (len(data), size, len(MAGIC), len(data)))

    ser = serial.Serial(args.port, args.baud, timeout=1.0)
    time.sleep(1.0)            # 等模块/电平稳定
    ser.reset_input_buffer()   # 清掉启动时的多余输出

    # 1) 发 flashcn 命令
    ser.write(("flashcn %d\n" % size).encode())
    print("-> sent: flashcn %d" % size)

    # 2) 等 READY（MCU 会先擦除字库区，约几秒）
    resp = read_until(ser, b"FLASCN READY", timeout=40.0)
    print("<-", resp.decode(errors="replace").strip()[:200])
    if b"FLASCN READY" not in resp:
        print("未收到 FLASCN READY，终止")
        ser.close()
        sys.exit(1)

    # 3) 流式烧录：魔数 + bin，按帧发送并等 ACK
    stream = MAGIC + data
    sent = 0
    t0 = time.time()
    while sent < size:
        chunk = stream[sent:sent + CHUNK]
        ser.write(chunk)
        ack = read_until(ser, ACK, timeout=10.0)
        drain_acks(ser, timeout=0.3)
        if ACK not in ack:
            print("第 %d 字节后未收到 ACK，终止" % sent)
            ser.close()
            sys.exit(1)
        sent += len(chunk)
        if sent % 6000 == 0 or sent == size:
            print("progress %d/%d (%.0f%%)" % (sent, size, sent * 100.0 / size))
    print("sent %d bytes in %.1fs" % (sent, time.time() - t0))

    # 4) 等 DONE
    done = read_until(ser, b"FLASCN DONE", timeout=20.0)
    print("<-", done.decode(errors="replace").strip()[:200])
    if b"FLASCN DONE" in done:
        print("烧录完成！复位后 OLED 应显示中文；串口发 'oled' 重绘，'w25' 看 font=READY。")
    else:
        print("未收到 FLASCN DONE，请检查。")
    ser.close()


if __name__ == "__main__":
    main()
