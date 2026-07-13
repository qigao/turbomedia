#!/bin/bash
# Build script for TurboNet Media iOS

set -e

# Configuration
BUILD_TYPE="${BUILD_TYPE:-Release}"
ARCHS="${ARCHS:-arm64 x86_64}"
IOS_DEPLOYMENT_TARGET="14.0"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}TurboNet Media iOS Build Script${NC}"
echo "======================================"
echo "Build Type: $BUILD_TYPE"
echo "Architectures: $ARCHS"
echo "iOS Deployment Target: $IOS_DEPLOYMENT_TARGET"
echo ""

# Build for each architecture
for ARCH in $ARCHS; do
    echo -e "${YELLOW}Building for $ARCH...${NC}"
    
    BUILD_DIR="build-ios-$ARCH"
    
    # Determine SDK
    if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "i386" ]; then
        SDK="iphonesimulator"
        PLATFORM="SIMULATOR64"
    else
        SDK="iphoneos"
        PLATFORM="OS64"
    fi
    
    # Configure
    cmake -B "$BUILD_DIR" -S . \
        -G Xcode \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_SYSROOT="$SDK" \
        -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO \
        -DCMAKE_IOS_INSTALL_COMBINED=YES \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_TESTING=OFF
    
    # Build
    cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -- -sdk "$SDK" -arch "$ARCH"
    
    # Install
    cmake --install "$BUILD_DIR" --prefix "install/$ARCH" --config "$BUILD_TYPE"
    
    echo -e "${GREEN}✓ Built $ARCH${NC}"
    echo ""
done

# Create universal library
echo -e "${YELLOW}Creating universal library...${NC}"

DEVICE_LIB="install/arm64/lib/libturbonet_media_ios.a"
SIMULATOR_LIB="install/x86_64/lib/libturbonet_media_ios.a"

if [ -f "$DEVICE_LIB" ] && [ -f "$SIMULATOR_LIB" ]; then
    mkdir -p install/universal/lib
    lipo -create "$DEVICE_LIB" "$SIMULATOR_LIB" -output install/universal/lib/libturbonet_media_ios.a
    echo -e "${GREEN}✓ Created universal library${NC}"
fi

# Create XCFramework
echo -e "${YELLOW}Creating XCFramework...${NC}"

if [ -f "$DEVICE_LIB" ]; then
    xcodebuild -create-xcframework \
        -library "$DEVICE_LIB" \
        -headers include \
        -library "$SIMULATOR_LIB" \
        -headers include \
        -output "TurboMedia.xcframework"
    
    echo -e "${GREEN}✓ Created TurboMedia.xcframework${NC}"
fi

echo ""
echo -e "${GREEN}Build complete!${NC}"
echo "Libraries installed in: install/"
echo "XCFramework: TurboMedia.xcframework"
echo ""
echo "To use in Xcode:"
echo "1. Drag TurboMedia.xcframework into your project"
echo "2. Add to 'Frameworks, Libraries, and Embedded Content'"
echo "3. Set 'Embed' to 'Embed & Sign'"
