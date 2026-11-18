@echo off
title OS Project: Parallel File Compressor
cd /d "%~dp0"

echo ===================================================
echo   OS Project: Parallel File Compressor System
echo ===================================================
echo.

where gcc >nul 2>&1
if errorlevel 1 (
    echo [WARNING] GCC compiler not found in PATH!
    echo Checking if pre-compiled pcompress.exe exists...
    if not exist pcompress.exe (
        echo [ERROR] pcompress.exe not found and GCC is missing.
        echo Please install MinGW-w64 or add GCC to your PATH.
        pause
        exit /b 1
    )
) else (
    echo [1/3] Compiling parallel_compress.c with GCC...
    gcc -O2 -Wall -Wextra -std=c11 -static parallel_compress.c -o pcompress.exe -lz
    if errorlevel 1 (
        echo.
        echo [BUILD ERROR] Failed to compile parallel_compress.c!
        echo Fix compilation errors above before launching the dashboard.
        echo.
        pause
        exit /b 1
    )
    echo [Build OK] pcompress.exe compiled successfully!
)

echo [2/3] Checking Python runtime...
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python is not installed or not in PATH!
    pause
    exit /b 1
)

echo [3/3] Starting dashboard server at http://localhost:5050 ...
start "" cmd /c "timeout /t 2 /nobreak >nul && start http://localhost:5050"

python server.py
pause
