@echo off
set "DEST=%CommonProgramFiles%\VST3"
echo Instalando VocalChain en "%DEST%" ...
xcopy /E /I /Y "%~dp0VocalChain.vst3" "%DEST%\VocalChain.vst3"
echo.
echo Listo. En FL Studio: Options ^> Manage plugins ^> Find installed plugins.
pause
