@echo off
rem cts4p - compila tudo (Release x64), testa e gera dist\cts4p-<versao>.zip
rem   build_all.bat              build + testes + pacote
rem   build_all.bat -FullTests   + testes de rede entre processos e overlay
rem   build_all.bat -SkipTests   so build + pacote
rem   build_all.bat -Install     instala neste PC no final (pelo instalador do pacote)
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_all.ps1" %*
exit /b %ERRORLEVEL%
