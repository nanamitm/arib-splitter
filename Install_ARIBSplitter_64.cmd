@cd /d "%~dp0"
@regsvr32.exe "%~dp0ARIBSplitter.ax" /s
@if %errorlevel% NEQ 0 goto error
:success
@echo.
@echo.
@echo    Installation succeeded.
@echo.
@echo    Please do not delete the ARIBSplitter.ax file.
@echo    The installer has not copied the files anywhere.
@echo    Keep the bundled DLL files and ARIBSplitter.ini in this folder.
@echo.
@goto done
:error
@echo.
@echo.
@echo    Installation failed.
@echo.
@echo    You need to right click "Install_ARIBSplitter_64.cmd" and choose "Run as administrator".
@echo.
:done
@pause >NUL
