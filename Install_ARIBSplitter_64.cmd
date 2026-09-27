@cd /d "%~dp0"
@regsvr32.exe "%~dp0ARIBSplitter.ax" /s
@if %errorlevel% NEQ 0 goto error
@regsvr32.exe "%~dp0ARIBAudio.ax" /s
@if %errorlevel% NEQ 0 goto rollback
:success
@echo.
@echo.
@echo    Installation succeeded.
@echo.
@echo    Please do not delete the ARIBSplitter.ax and ARIBAudio.ax files.
@echo    The installer has not copied the files anywhere.
@echo    Keep the bundled DLL files and ARIBSplitter.ini in this folder.
@echo.
@pause >NUL
@exit /b 0
:rollback
@regsvr32.exe "%~dp0ARIBSplitter.ax" /u /s
@if %errorlevel% NEQ 0 echo    Could not undo ARIBSplitter.ax registration; unregister it manually.
:error
@echo.
@echo.
@echo    Installation failed.
@echo.
@echo    You need to right click "Install_ARIBSplitter_64.cmd" and choose "Run as administrator".
@echo.
@pause >NUL
@exit /b 1
