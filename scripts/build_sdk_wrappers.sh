#!/bin/bash
# Build all SDK wrappers
#
# Usage:
#   ./scripts/build_sdk_wrappers.sh              # build all (Docker)
#   ./scripts/build_sdk_wrappers.sh t1           # build Booster T1 only (Docker)
#   ./scripts/build_sdk_wrappers.sh g1           # build Unitree G1 only (Docker)
#   ./scripts/build_sdk_wrappers.sh --local g1-rmw  # build Unitree G1 RMW only
#   ./scripts/build_sdk_wrappers.sh --local lx2501-3-rmw  # build LX2501_3 RMW only
#   ./scripts/build_sdk_wrappers.sh x2           # build X2 only (local)
#   ./scripts/build_sdk_wrappers.sh pm01         # build EngineAI PM01 EDU only (local)
#   ./scripts/build_sdk_wrappers.sh --local      # build all locally (no Docker)
#   ./scripts/build_sdk_wrappers.sh --local t1   # build T1 locally
#   ./scripts/build_sdk_wrappers.sh --local g1   # build G1 locally
#   ./scripts/build_sdk_wrappers.sh --local x2   # build X2 locally
#   ./scripts/build_sdk_wrappers.sh --local pm01 # build EngineAI PM01 EDU locally

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
LOCAL_MODE=false
TARGET="all"

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --local)
            LOCAL_MODE=true
            ;;
        t1|g1|g1-rmw|lx2501-3-rmw|x2|pm01|all)
            TARGET="$arg"
            ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: $0 [--local] [t1|g1|g1-rmw|lx2501-3-rmw|x2|pm01|all]"
            exit 1
            ;;
    esac
done

echo "=== SDK Wrapper Builder ==="

if [ "$LOCAL_MODE" = true ]; then
    echo "Mode: Local build (no Docker)"
    echo "Project root: $PROJECT_ROOT"

    mkdir -p "$BUILD_DIR/hardware"
    mkdir -p "$BUILD_DIR/sdk_wrappers_local"
    cd "$BUILD_DIR/sdk_wrappers_local"

    CMAKE_ARGS="-DPROJECT_ROOT=$PROJECT_ROOT"

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "t1" ]; then
        echo ""
        echo "--- Building Booster T1 wrapper ---"
        mkdir -p booster_t1 && cd booster_t1
        cmake "$PROJECT_ROOT/sdk_wrapper/booster_t1" $CMAKE_ARGS
        make -j$(nproc) VERBOSE=1
        mkdir -p "$BUILD_DIR/hardware/booster_t1/lib"
        cp libbooster_t1_wrapper.so* "$BUILD_DIR/hardware/booster_t1/"
        ARCH=$(uname -m)
        SDK_LIB="$PROJECT_ROOT/sdk/booster_robotics_sdk/third_party/lib/${ARCH}"
        cp "${SDK_LIB}/libfastrtps.so" "$BUILD_DIR/hardware/booster_t1/lib/libfastrtps.so.2.13"
        cp "${SDK_LIB}/libfastcdr.so" "$BUILD_DIR/hardware/booster_t1/lib/libfastcdr.so.2"
        # Copy to assets
        mkdir -p "$PROJECT_ROOT/assets-t1/hardware/sdk_wrapper/lib"
        # Package root keeps a single entrypoint .so (no versioned symlinks).
        rm -f "$PROJECT_ROOT/assets-t1/hardware/sdk_wrapper/"libbooster_t1_wrapper.so*
        cp -L libbooster_t1_wrapper.so "$PROJECT_ROOT/assets-t1/hardware/sdk_wrapper/libbooster_t1_wrapper.so"
        cp "$BUILD_DIR/hardware/booster_t1/lib/"* "$PROJECT_ROOT/assets-t1/hardware/sdk_wrapper/lib/"
        if command -v patchelf &>/dev/null; then
            patchelf --set-rpath '$ORIGIN/lib' "$PROJECT_ROOT/assets-t1/hardware/sdk_wrapper/libbooster_t1_wrapper.so"
        fi
        "$PROJECT_ROOT/ci/gen_wrapper_manifest.sh" \
            "$PROJECT_ROOT/assets-t1/hardware/sdk_wrapper" \
            "$PROJECT_ROOT/sdk_wrapper/booster_t1/manifest.yaml" "$(uname -m)"
        cd ..
        echo "SUCCESS: $PROJECT_ROOT/assets-t1/hardware/sdk_wrapper/libbooster_t1_wrapper.so"
    fi

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "g1" ]; then
        echo ""
        echo "--- Building Unitree G1 wrapper ---"
        mkdir -p unitree_g1 && cd unitree_g1
        cmake "$PROJECT_ROOT/sdk_wrapper/unitree_g1" $CMAKE_ARGS
        make -j$(nproc) VERBOSE=1
        mkdir -p "$BUILD_DIR/hardware/unitree_g1/lib"
        cp libunitree_g1_wrapper.so* "$BUILD_DIR/hardware/unitree_g1/"
        ARCH=$(uname -m)
        cp "$PROJECT_ROOT/sdk/unitree_sdk2/thirdparty/lib/${ARCH}/libddsc.so" "$BUILD_DIR/hardware/unitree_g1/lib/libddsc.so.0"
        cp "$PROJECT_ROOT/sdk/unitree_sdk2/thirdparty/lib/${ARCH}/libddscxx.so" "$BUILD_DIR/hardware/unitree_g1/lib/libddscxx.so.0"
        # Copy to assets
        mkdir -p "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/lib"
        # Package root keeps a single entrypoint .so (no versioned symlinks).
        rm -f "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/"libunitree_g1_wrapper.so*
        cp -L libunitree_g1_wrapper.so "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/libunitree_g1_wrapper.so"
        cp "$BUILD_DIR/hardware/unitree_g1/lib/"* "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/lib/"
        if command -v patchelf &>/dev/null; then
            patchelf --set-rpath '$ORIGIN/lib' "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/libunitree_g1_wrapper.so"
        fi
        "$PROJECT_ROOT/ci/gen_wrapper_manifest.sh" \
            "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper" \
            "$PROJECT_ROOT/sdk_wrapper/unitree_g1/manifest.yaml" "$(uname -m)"
        cd ..
        echo "SUCCESS: $PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/libunitree_g1_wrapper.so"
    fi

    if [ "$TARGET" = "g1-rmw" ]; then
        echo ""
        echo "--- Building Unitree G1 RMW wrapper ---"
        source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
        if [ ! -f "$PROJECT_ROOT/sdk/unitree_ros2/cyclonedds_ws/install/unitree_hg/share/unitree_hg/cmake/unitree_hgConfig.cmake" ] || \
           [ ! -f "$PROJECT_ROOT/sdk/unitree_ros2/cyclonedds_ws/install/unitree_api/share/unitree_api/cmake/unitree_apiConfig.cmake" ]; then
            echo "--- Building Unitree ROS2 unitree_hg/unitree_api typesupport ---"
            (cd "$PROJECT_ROOT/sdk/unitree_ros2/cyclonedds_ws" && \
                colcon build --packages-select unitree_hg unitree_api --cmake-args -DBUILD_TESTING=OFF)
        fi
        source "$PROJECT_ROOT/sdk/unitree_ros2/cyclonedds_ws/install/setup.bash"
        mkdir -p unitree_g1_rmw && cd unitree_g1_rmw
        cmake "$PROJECT_ROOT/sdk_wrapper/unitree_g1_rmw" $CMAKE_ARGS
        make -j$(nproc) VERBOSE=1
        mkdir -p "$BUILD_DIR/hardware/unitree_g1_rmw/lib"
        cp libunitree_g1_rmw_wrapper.so* "$BUILD_DIR/hardware/unitree_g1_rmw/"
        cp "$PROJECT_ROOT/sdk/unitree_ros2/cyclonedds_ws/install/unitree_hg/lib/libunitree_hg__rosidl_"*.so \
            "$BUILD_DIR/hardware/unitree_g1_rmw/lib/"
        cp "$PROJECT_ROOT/sdk/unitree_ros2/cyclonedds_ws/install/unitree_api/lib/libunitree_api__rosidl_"*.so \
            "$BUILD_DIR/hardware/unitree_g1_rmw/lib/"
        mkdir -p "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/lib"
        cp libunitree_g1_rmw_wrapper.so* "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/"
        cp "$BUILD_DIR/hardware/unitree_g1_rmw/lib/"* "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/lib/"
        if command -v patchelf &>/dev/null; then
            patchelf --set-rpath '$ORIGIN/lib:/opt/ros/humble/lib' \
                "$PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/libunitree_g1_rmw_wrapper.so"
        fi
        cd ..
        echo "SUCCESS: $PROJECT_ROOT/assets-g1/hardware/sdk_wrapper/libunitree_g1_rmw_wrapper.so"
    fi

    if [ "$TARGET" = "lx2501-3-rmw" ]; then
        echo ""
        echo "--- Building LX2501_3 RMW wrapper ---"
        source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
        if [ ! -f "$PROJECT_ROOT/sdk/lx2501_3-v0.9.0.4/install/aimdk_msgs/share/aimdk_msgs/cmake/aimdk_msgsConfig.cmake" ]; then
            echo "--- Building LX2501_3 aimdk_msgs typesupport ---"
            (cd "$PROJECT_ROOT/sdk/lx2501_3-v0.9.0.4" && \
                colcon build --packages-select aimdk_msgs --cmake-args -DBUILD_TESTING=OFF)
        fi
        source "$PROJECT_ROOT/sdk/lx2501_3-v0.9.0.4/install/setup.bash"
        mkdir -p lx2501_3_rmw && cd lx2501_3_rmw
        cmake "$PROJECT_ROOT/sdk_wrapper/lx2501_3_rmw" $CMAKE_ARGS
        make -j$(nproc) VERBOSE=1
        mkdir -p "$BUILD_DIR/hardware/lx2501_3_rmw/lib"
        cp liblx2501_3_rmw_wrapper.so* "$BUILD_DIR/hardware/lx2501_3_rmw/"
        cp "$PROJECT_ROOT/sdk/lx2501_3-v0.9.0.4/install/aimdk_msgs/lib/libaimdk_msgs__rosidl_"*.so \
            "$BUILD_DIR/hardware/lx2501_3_rmw/lib/"
        mkdir -p "$PROJECT_ROOT/assets-lx2501_3/hardware/sdk_wrapper/lib"
        cp liblx2501_3_rmw_wrapper.so* "$PROJECT_ROOT/assets-lx2501_3/hardware/sdk_wrapper/"
        cp "$BUILD_DIR/hardware/lx2501_3_rmw/lib/"* \
            "$PROJECT_ROOT/assets-lx2501_3/hardware/sdk_wrapper/lib/"
        if command -v patchelf &>/dev/null; then
            patchelf --set-rpath '$ORIGIN/lib:/opt/ros/humble/lib' \
                "$PROJECT_ROOT/assets-lx2501_3/hardware/sdk_wrapper/liblx2501_3_rmw_wrapper.so"
        fi
        cd ..
        echo "SUCCESS: $PROJECT_ROOT/assets-lx2501_3/hardware/sdk_wrapper/liblx2501_3_rmw_wrapper.so"
    fi

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "x2" ]; then
        echo ""
        echo "--- Building X2 wrapper ---"
        mkdir -p x2 && cd x2
        cmake "$PROJECT_ROOT/sdk_wrapper/x2" $CMAKE_ARGS
        make -j$(nproc) VERBOSE=1
        cp libx2_wrapper.so* "$BUILD_DIR/"
        # Assemble the deployable package: single entrypoint .so + bundled
        # private libs (yaml-cpp, zmq) under lib/, with $ORIGIN/lib runpath.
        X2_PKG="$PROJECT_ROOT/assets-x2/hardware/sdk_wrapper"
        mkdir -p "$X2_PKG/lib"
        rm -f "$X2_PKG/"libx2_wrapper.so*
        cp -L libx2_wrapper.so "$X2_PKG/libx2_wrapper.so"
        for soname in $(readelf -d libx2_wrapper.so | awk '/NEEDED/{gsub(/[][]/,"",$5); print $5}' | grep -E 'yaml-cpp|zmq'); do
            src=$(ldd libx2_wrapper.so | awk -v s="$soname" '$1==s{print $3}')
            [ -n "$src" ] && cp -L "$src" "$X2_PKG/lib/$soname"
        done
        if command -v patchelf &>/dev/null; then
            patchelf --set-rpath '$ORIGIN/lib' "$X2_PKG/libx2_wrapper.so"
        fi
        "$PROJECT_ROOT/ci/gen_wrapper_manifest.sh" \
            "$X2_PKG" "$PROJECT_ROOT/sdk_wrapper/x2/manifest.yaml" "$(uname -m)"
        cd ..
        echo "SUCCESS: $X2_PKG/libx2_wrapper.so"
    fi

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "pm01" ]; then
        echo ""
        echo "--- Building EngineAI PM01 EDU wrapper ---"
        mkdir -p engineai_pm01_edu && cd engineai_pm01_edu
        cmake "$PROJECT_ROOT/sdk_wrapper/engineai_pm01_edu" $CMAKE_ARGS
        make -j$(nproc) VERBOSE=1
        mkdir -p "$BUILD_DIR/hardware/engineai_pm01_edu"
        cp libengineai_pm01_edu_wrapper.so* "$BUILD_DIR/hardware/engineai_pm01_edu/"
        mkdir -p "$PROJECT_ROOT/assets-pm01-edu/hardware/sdk_wrapper/lib"
        # Package root keeps a single entrypoint .so (no versioned symlinks).
        rm -f "$PROJECT_ROOT/assets-pm01-edu/hardware/sdk_wrapper/"libengineai_pm01_edu_wrapper.so*
        cp -L libengineai_pm01_edu_wrapper.so "$PROJECT_ROOT/assets-pm01-edu/hardware/sdk_wrapper/libengineai_pm01_edu_wrapper.so"
        if command -v patchelf &>/dev/null; then
            patchelf --set-rpath '$ORIGIN/lib' "$PROJECT_ROOT/assets-pm01-edu/hardware/sdk_wrapper/libengineai_pm01_edu_wrapper.so"
        fi
        "$PROJECT_ROOT/ci/gen_wrapper_manifest.sh" \
            "$PROJECT_ROOT/assets-pm01-edu/hardware/sdk_wrapper" \
            "$PROJECT_ROOT/sdk_wrapper/engineai_pm01_edu/manifest.yaml" "$(uname -m)"
        cd ..
        echo "SUCCESS: $PROJECT_ROOT/assets-pm01-edu/hardware/sdk_wrapper/libengineai_pm01_edu_wrapper.so"
    fi
else
    echo "Mode: Docker build"

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "t1" ]; then
        "$SCRIPT_DIR/build_booster_t1_wrapper.sh"
        echo ""
    fi

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "g1" ]; then
        "$SCRIPT_DIR/build_unitree_g1_wrapper.sh"
        echo ""
    fi

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "x2" ]; then
        "$SCRIPT_DIR/build_x2_wrapper.sh"
        echo ""
    fi

    if [ "$TARGET" = "all" ] || [ "$TARGET" = "pm01" ]; then
        echo ""
        echo "--- Building EngineAI PM01 EDU wrapper locally ---"
        "$0" --local pm01
        echo ""
    fi
fi

echo "=== All requested wrappers built successfully ==="
