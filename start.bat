@echo off
rem ============================================================
rem  启动网页验收台（Web Serial 要求 http(s) 或 localhost）
rem  用法：双击本文件，然后用 Chrome/Edge 打开打印出的地址
rem ============================================================
setlocal
cd /d "%~dp0"

set PORT=8000
rem 找一个空闲端口（被占用就 +1，最多试到 8010）
:findport
netstat -an | findstr /r /c:":%PORT% .*LISTENING" >nul 2>&1
if %errorlevel%==0 (
  set /a PORT=%PORT%+1
  if %PORT% gtr 8010 (
    echo [ERROR] 8000-8010 都被占用，请手动改脚本里的 PORT
    pause
    exit /b 1
  )
  goto findport
)

echo.
echo   ============================================================
echo    验收台地址（复制到 Chrome / Edge 打开）:
echo.
echo      http://localhost:%PORT%/tools/acceptance.html
echo.
echo    只连【主机】一个串口（COM17）。
echo    按 Ctrl+C 结束本服务。
echo   ============================================================
echo.

start "" "http://localhost:%PORT%/tools/acceptance.html"
python -m http.server %PORT%
