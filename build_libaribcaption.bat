@echo off
REM Build libaribcaption as static library for x64
REM Run this once before building ARIBSplitter in Visual Studio

setlocal

set BUILD_DIR=%~dp0libaribcaption\build

cmake -S "%~dp0libaribcaption" -B "%BUILD_DIR%\x64\Release" -A x64 ^
  -DARIBCC_SHARED_LIBRARY=OFF ^
  -DARIBCC_USE_DIRECTWRITE=ON ^
  -DARIBCC_USE_GDI_FONT=OFF ^
  -DARIBCC_NO_RENDERER=OFF ^
  "-DCMAKE_CXX_FLAGS_RELEASE=/MT /O2 /DNDEBUG" ^
  "-DCMAKE_C_FLAGS_RELEASE=/MT /O2 /DNDEBUG"

cmake --build "%BUILD_DIR%\x64\Release" --config Release

cmake -S "%~dp0libaribcaption" -B "%BUILD_DIR%\x64\Debug" -A x64 ^
  -DARIBCC_SHARED_LIBRARY=OFF ^
  -DARIBCC_USE_DIRECTWRITE=ON ^
  -DARIBCC_USE_GDI_FONT=OFF ^
  -DARIBCC_NO_RENDERER=OFF ^
  "-DCMAKE_CXX_FLAGS_DEBUG=/MTd /Zi /Ob0 /Od /RTC1" ^
  "-DCMAKE_C_FLAGS_DEBUG=/MTd /Zi /Ob0 /Od /RTC1"

cmake --build "%BUILD_DIR%\x64\Debug" --config Debug

echo.
echo libaribcaption build complete.
echo   Release: %BUILD_DIR%\x64\Release\Release\aribcaption.lib
echo   Debug:   %BUILD_DIR%\x64\Debug\Debug\aribcaption.lib
endlocal
