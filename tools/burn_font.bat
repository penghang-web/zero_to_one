@echo off
chcp 65001 >nul
REM ============================================================
REM  烧录 GB2312 字库到 W25Q（GD25Q32）
REM  使用前：
REM    1) 关闭串口助手（释放 COM 口）
REM    2) 确认 MCU 已上电、程序在运行（会停在等待 flashcn 命令）
REM  用法：
REM    直接双击 -> 用下面 PORT 指定的端口
REM    带参数  -> burn_font.bat COM5   （用命令行给的端口）
REM ============================================================
set PORT=COM3
if not "%1"=="" set PORT=%1

echo 正在通过 %PORT% 烧录字库到 W25Q ...
"C:\Users\PH\.workbuddy\binaries\python\envs\default\Scripts\python.exe" "F:/0-1/zero_to_one/tools/flash_w25q.py" %PORT%
echo.
echo 烧录结束（看到 FLASCN DONE 即成功）。任意键关闭窗口...
pause
