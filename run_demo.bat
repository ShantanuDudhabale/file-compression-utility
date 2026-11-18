@echo off
cd /d "%~dp0"
cls
echo ===================================================
echo   OS Project: Parallel File Compressor Demonstration
echo ===================================================
echo.

echo [1/3] Running Parallel Compression (4 threads)...
.\pcompress.exe -c parallel_compress.c demo.pcmp 4
if errorlevel 1 goto error

echo.
echo [2/3] Running Parallel Decompression (4 threads)...
.\pcompress.exe -d demo.pcmp restored.c 4
if errorlevel 1 goto error

echo.
echo [3/3] Verifying byte-by-byte file integrity (fc /b)...
fc /b parallel_compress.c restored.c >nul
if errorlevel 1 (
    echo [FAIL] Checksum/file mismatch!
) else (
    echo [PASS] 100%% byte-for-byte exact match! Verified OK.
)

del demo.pcmp restored.c >nul 2>&1

echo.
echo ===================================================
echo   Demonstration Complete!
echo   To launch the web UI, run: start_dashboard.bat
echo ===================================================
echo.
pause
goto end

:error
echo.
echo [ERROR] An error occurred during execution.
pause

:end
