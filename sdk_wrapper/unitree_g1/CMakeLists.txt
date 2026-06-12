cmake_minimum_required(VERSION 3.10)
project(unitree_g1_wrapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# Architecture detection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(ARCH "aarch64")
else()
    set(ARCH "x86_64")
endif()

# PROJECT_ROOT: Docker 内为 /workspace，本地编译时由父 CMakeLists 传入或自动推导
if(NOT DEFINED PROJECT_ROOT)
    if(EXISTS "/workspace/sdk/unitree_sdk2")
        set(PROJECT_ROOT /workspace)
    else()
        # 本地编译：从当前 CMakeLists.txt 位置推导项目根目录
        get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    endif()
endif()

set(UNITREE_SDK_DIR ${PROJECT_ROOT}/sdk/unitree_sdk2)

# Import Cyclone DDS from unitree thirdparty
if(NOT TARGET ddsc)
    add_subdirectory(${UNITREE_SDK_DIR}/thirdparty ${CMAKE_BINARY_DIR}/unitree_thirdparty)
endif()
# Prevent build-tree paths from leaking into RUNPATH of consumers:
# 1) clear INTERFACE_LINK_DIRECTORIES (set by target_link_directories)
# 2) SKIP_BUILD_RPATH stops CMake from adding the IMPORTED_LOCATION dir
set_target_properties(ddsc ddscxx PROPERTIES
    INTERFACE_LINK_DIRECTORIES ""
    SKIP_BUILD_RPATH TRUE
)

# Import unitree_sdk2 static library
if(NOT TARGET unitree_sdk2)
    add_library(unitree_sdk2 STATIC IMPORTED GLOBAL)
    set_target_properties(unitree_sdk2 PROPERTIES
        IMPORTED_LOCATION ${UNITREE_SDK_DIR}/lib/${ARCH}/libunitree_sdk2.a
    )
    target_link_libraries(unitree_sdk2 INTERFACE ddsc ddscxx Threads::Threads)
    target_include_directories(unitree_sdk2 INTERFACE ${UNITREE_SDK_DIR}/include)
endif()

find_package(Threads REQUIRED)

# Build the wrapper shared library
add_library(unitree_g1_wrapper SHARED
    ${PROJECT_ROOT}/sdk_wrapper/unitree_g1/unitree_g1_wrapper.cpp
)
target_compile_definitions(unitree_g1_wrapper PRIVATE SDK_WRAPPER_BUILDING)
set_target_properties(unitree_g1_wrapper PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # Let CMake skip all automatic RPATH — prevents IMPORTED DDS target paths
    # from leaking.  RUNPATH is set post-build by ci/package_sdk.sh via patchelf.
    SKIP_BUILD_RPATH TRUE
)
target_include_directories(unitree_g1_wrapper PRIVATE
    ${PROJECT_ROOT}
)
target_link_libraries(unitree_g1_wrapper
    unitree_sdk2
)

# Export only the C ABI entry point; hide symbols pulled in from the static SDK.
# RUNPATH is set to $ORIGIN/lib by ci/package_sdk.sh via patchelf after build,
# avoiding CMake/Ninja escaping issues with the $ sign.
target_link_options(unitree_g1_wrapper PRIVATE
    "-Wl,--version-script=${PROJECT_ROOT}/sdk_wrapper/export.map"
)
