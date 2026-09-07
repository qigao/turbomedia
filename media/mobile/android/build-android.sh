#!/bin/bash
# Build script for TurboNet Media Android

set -e

# Configuration
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/25.2.9519653}"
ANDROID_API=26
BUILD_TYPE="${BUILD_TYPE:-Release}"
ABIS="${ABIS:-arm64-v8a x86_64 x86}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}TurboNet Media Android Build Script${NC}"
echo "======================================"
echo "NDK: $ANDROID_NDK"
echo "API: $ANDROID_API"
echo "Build Type: $BUILD_TYPE"
echo "ABIs: $ABIS"
echo ""

# Check NDK
if [ ! -d "$ANDROID_NDK" ]; then
    echo -e "${RED}Error: Android NDK not found at $ANDROID_NDK${NC}"
    echo "Set ANDROID_NDK environment variable or install NDK"
    exit 1
fi

# Build for each ABI
for ABI in $ABIS; do
    echo -e "${YELLOW}Building for $ABI...${NC}"
    
    BUILD_DIR="build-android-$ABI"
    
    # Configure
    cmake -B "$BUILD_DIR" -S . \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$ANDROID_API" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_TESTING=OFF
    
    # Build
    cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j$(nproc)
    
    # Install
    cmake --install "$BUILD_DIR" --prefix "install/$ABI"
    
    echo -e "${GREEN}✓ Built $ABI${NC}"
    echo ""
done

echo -e "${GREEN}Build complete!${NC}"
echo "Libraries installed in: install/"
echo ""
echo "To use in Android Studio:"
echo "1. Copy libs to your project: cp -r install/*/lib/* app/src/main/jniLibs/"
echo "2. Add dependency in build.gradle"
echo "3. Load library: System.loadLibrary(\"turbonet_media_android\")"
