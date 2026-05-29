@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "CONFIG=release"
set "TARGET=%SCRIPT_DIR%ARIBSplitter.ax"

if not "%~1"=="" (
    if /i not "%~1"=="release" (
        if /i not "%~1"=="debug" (
            echo Usage: %~nx0 [release^|debug]
            exit /b 2
        )
    )
    set "CONFIG=%~1"
)

if not exist "%TARGET%" (
    if /i "%CONFIG%"=="debug" (
        set "TARGET=%SCRIPT_DIR%bin_x64d\ARIBSplitter.ax"
    ) else (
        set "TARGET=%SCRIPT_DIR%bin_x64\ARIBSplitter.ax"
    )
)

if not exist "%TARGET%" (
    echo ARIBSplitter.ax was not found:
    echo   %TARGET%
    echo.
    echo Put this script next to ARIBSplitter.ax, or build ARIBSplitter first.
    exit /b 1
)

net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator privileges...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%ComSpec%' -ArgumentList '/k ""%~f0"" %*' -Verb RunAs"
    exit /b 0
)

echo Registering:
echo   %TARGET%
"%SystemRoot%\System32\regsvr32.exe" /s "%TARGET%"
if errorlevel 1 (
    echo.
    echo [FAILED] Registration failed.
    echo Try running this script as Administrator, or check that all DLLs
    echo are present in the same folder as ARIBSplitter.ax.
    pause
    exit /b 1
)

echo.
echo [OK] ARIBSplitter registered successfully.
pause
exit /b 0
