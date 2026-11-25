@echo off
rem cts4p - instalador. Dois cliques para abrir o menu (instalar / desinstalar).
rem Linha de comando: instalar.bat /instalar  ou  instalar.bat /desinstalar
setlocal
cd /d "%~dp0"
set ACTION=Menu
if /i "%~1"=="/instalar" set ACTION=Install
if /i "%~1"=="/desinstalar" set ACTION=Uninstall
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar.ps1" -Action %ACTION%
set RC=%ERRORLEVEL%
echo.
pause
exit /b %RC%
