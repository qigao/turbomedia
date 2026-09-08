@echo off
REM Build script for TurboNet Media Android (Windows)

setlocal enabledelayedexpansion

REM Configuration
if "%ANDROID_NDK%"=="" set ANDROID_NDK=%LOCALAPPDATA%\Android\Sdk\ndk\25.2.9519653
set ANDROID_API=26
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release
if "%ABIS%"=="" set ABIS=arm64-v8a x86_64 x86

echo TurboNet Media Android Build Script
echo ======================================
echo NDK: %ANDROID_NDK%
echo API: %ANDROID_API%
echo Build Type: %BUILD_TYPE%
echo ABIs: %ABIS%
echo.

REM Check NDK
if not exist "%ANDROID_NDK%" (
    echo Error: Android NDK not found at %ANDROID_NDK%
    echo Set ANDROID_NDK environment variable or install NDK
    exit /b 1
)

REM Build for each ABI
for %%A in (%ABIS%) do (
    echo Building for %%A...
    
    set BUILD_DIR=build-android-%%A
    
    REM Configure
    cmake -B "!BUILD_DIR!" -S . ^
        -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK%/build/cmake/android.toolchain.cmake" ^
        -DANDROID_ABI=%%A ^
        -DANDROID_PLATFORM=android-%ANDROID_API% ^
        -DANDROID_STL=c++_shared ^
        -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
        -DBUILD_TESTING=OFF
    
    if errorlevel 1 (
        echo Failed to configure %%A
        exit /b 1
    )
    
    REM Build
    cmake --build "!BUILD_DIR!" --config %BUILD_TYPE% -j 8
    
    if errorlevel 1 (
        echo Failed to build %%A
        exit /b 1
    )
    
    REM Install
    cmake --install "!BUILD_DIR!" --prefix install/%%A
    
    echo [OK] Built %%A
    echo.
)

echo Build complete!
echo Libraries installed in: install\
echo.
echo To use in Android Studio:
echo 1. Copy libs to your project: xcopy /E /I install\*\lib\* app\src\main\jniLibs\
echo 2. Add dependency in build.gradle
echo 3. Load library: System.loadLibrary("turbonet_media_android")

endlocal
