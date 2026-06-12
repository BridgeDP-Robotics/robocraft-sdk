#!/bin/bash
# Build unitree_g1_wrapper.so using Docker (Ubuntu 20.04 + GCC 9.4.0)
#
# Environment matches unitree_sdk2 official devcontainer exactly.
#
# Usage:
#   ./scripts/build_unitree_g1_wrapper.sh
#
# Output:
#   assets-g1/hardware/sdk_wrapper/libunitree_g1_wrapper.so
#   assets-g1/hardware/sdk_wrapper/lib/libddsc.so.0
#   assets-g1/hardware/sdk_wrapper/lib/libddscxx.so.0

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WRAPPER_DIR="$PROJECT_ROOT/sdk_wrapper/unitree_g1"
BUILD_DIR="$PROJECT_ROOT/build"
IMAGE_NAME="unitree-g1-builder"

# Use sudo if user is not in docker group
DOCKER="docker"
if ! docker info >/dev/null 2>&1; then
    DOCKER="sudo docker"
fi

echo "=== Building Unitree G1 SDK Wrapper ==="
echo "Project root: $PROJECT_ROOT"

mkdir -p "$BUILD_DIR/hardware/unitree_g1"

# Build Docker image
echo "[1/3] Building Docker image (gcc-9 + unitree deps)..."
$DOCKER build -t "$IMAGE_NAME" "$WRAPPER_DIR"

# Compile inside container
echo "[2/3] Compiling in Docker container..."
$DOCKER run --rm \
    -v "$PROJECT_ROOT:/workspace:ro" \
    -v "$BUILD_DIR/hardware/unitree_g1:/output" \
    "$IMAGE_NAME" \
    bash -c '
        set -e
        echo "gcc version: $(gcc --version | head -1)"
        echo "g++ version: $(g++ --version | head -1)"

        echo "--- Building unitree_sdk2 examples (verify SDK works) ---"
        mkdir -p /tmp/sdk_build && cd /tmp/sdk_build
        cmake /workspace/sdk/unitree_sdk2 -DBUILD_EXAMPLES=ON 2>&1 | tail -5
        make -j$(nproc) 2>&1 | tail -5
        echo "--- SDK examples build OK ---"

        echo "--- Building wrapper ---"
        mkdir -p /tmp/wrapper_build && cd /tmp/wrapper_build
        cmake /workspace/sdk_wrapper/unitree_g1
        make -j$(nproc) VERBOSE=1
        cp libunitree_g1_wrapper.so* /output/
        echo "--- Copying DDS runtime libs ---"
        mkdir -p /output/lib
        ARCH=$(uname -m)
        cp /workspace/sdk/unitree_sdk2/thirdparty/lib/${ARCH}/libddsc.so /output/lib/libddsc.so.0
        cp /workspace/sdk/unitree_sdk2/thirdparty/lib/${ARCH}/libddscxx.so /output/lib/libddscxx.so.0
    '

# Copy to assets directory
echo "[3/3] Copying to assets-g1/hardware/sdk_wrapper/..."
mkdir -p "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/lib"
cp "$BUILD_DIR/hardware/unitree_g1/libunitree_g1_wrapper.so"* "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/"
cp "$BUILD_DIR/hardware/unitree_g1/lib/"* "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/lib/"
if command -v patchelf &>/dev/null; then
    patchelf --set-rpath '$ORIGIN/lib' "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/libunitree_g1_wrapper.so"
    echo "  RPATH set to \$ORIGIN/lib"
else
    echo "  WARNING: patchelf not found, RPATH not set"
fi

# Verify output
echo "Verifying output..."
OUTPUT="$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/libunitree_g1_wrapper.so"
if [ -f "$OUTPUT" ]; then
    echo "SUCCESS: $OUTPUT"
    ls -lh "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/"*
else
    echo "ERROR: Build failed, .so not found at $OUTPUT"
    exit 1
fi
