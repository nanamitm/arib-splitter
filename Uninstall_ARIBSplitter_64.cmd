@cd /d "%~dp0"
@regsvr32.exe "%~dp0ARIBAudio.ax" /u /s
@if %errorlevel% NEQ 0 goto error
@regsvr32.exe "%~dp0ARIBSplitter.ax" /u /s
@if %errorlevel% NEQ 0 goto error
:success
@echo.
@echo.
@echo    Uninstallation succeeded.
@echo.
@goto done
:error
@echo.
@echo.
@echo    Uninstallation failed.
@echo.
@echo    You need to right click "Uninstall_ARIBSplitter_64.cmd" and choose "Run as administrator".
@echo.
:done
@pause >NUL
