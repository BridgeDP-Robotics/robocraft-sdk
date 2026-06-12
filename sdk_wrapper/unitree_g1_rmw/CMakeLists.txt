cmake_minimum_required(VERSION 3.15)
project(unitree_g1_rmw_wrapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED PROJECT_ROOT)
    get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

if(DEFINED ENV{ROS_DISTRO} AND EXISTS "/opt/ros/$ENV{ROS_DISTRO}")
    list(PREPEND CMAKE_PREFIX_PATH "/opt/ros/$ENV{ROS_DISTRO}")
elseif(EXISTS "/opt/ros/humble")
    list(PREPEND CMAKE_PREFIX_PATH "/opt/ros/humble")
endif()

set(UNITREE_ROS2_INSTALL "${PROJECT_ROOT}/sdk/unitree_ros2/cyclonedds_ws/install/unitree_hg")
if(EXISTS "${UNITREE_ROS2_INSTALL}")
    list(PREPEND CMAKE_PREFIX_PATH "${UNITREE_ROS2_INSTALL}")
endif()
set(UNITREE_API_INSTALL "${PROJECT_ROOT}/sdk/unitree_ros2/cyclonedds_ws/install/unitree_api")
if(EXISTS "${UNITREE_API_INSTALL}")
    list(PREPEND CMAKE_PREFIX_PATH "${UNITREE_API_INSTALL}")
endif()

find_package(Threads REQUIRED)
find_package(rcl REQUIRED)
find_package(rcutils REQUIRED)
find_package(rmw REQUIRED)
find_package(rmw_implementation REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_c REQUIRED)
find_package(unitree_api REQUIRED)
find_package(unitree_hg REQUIRED)

add_library(unitree_g1_rmw_wrapper SHARED
    ${PROJECT_ROOT}/sdk_wrapper/unitree_g1_rmw/unitree_g1_rmw_wrapper.cpp
)

target_compile_definitions(unitree_g1_rmw_wrapper PRIVATE SDK_WRAPPER_BUILDING)

set_target_properties(unitree_g1_rmw_wrapper PROPERTIES
    OUTPUT_NAME unitree_g1_rmw_wrapper
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    SOVERSION 1
    VERSION 1.0.0
    BUILD_RPATH "$ORIGIN;$ORIGIN/lib;${UNITREE_ROS2_INSTALL}/lib;${UNITREE_API_INSTALL}/lib;/opt/ros/$ENV{ROS_DISTRO}/lib;/opt/ros/humble/lib"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/lib"
)

target_include_directories(unitree_g1_rmw_wrapper PRIVATE
    ${PROJECT_ROOT}
)

target_link_libraries(unitree_g1_rmw_wrapper PRIVATE
    Threads::Threads
    rcl::rcl
    rcutils::rcutils
    rmw::rmw
    rmw_implementation::rmw_implementation
    rosidl_runtime_c::rosidl_runtime_c
    rosidl_typesupport_c::rosidl_typesupport_c
    unitree_api::unitree_api__rosidl_generator_c
    unitree_api::unitree_api__rosidl_typesupport_c
    unitree_api::unitree_api__rosidl_typesupport_introspection_c
    unitree_hg::unitree_hg__rosidl_generator_c
    unitree_hg::unitree_hg__rosidl_typesupport_c
    unitree_hg::unitree_hg__rosidl_typesupport_introspection_c
)
# fastrtps_c CMake targets are only exported on Humble+. On Foxy the .so
# files still exist and are needed by ForceLoadUnitreeTypeSupportLibraries().
if(TARGET unitree_api::unitree_api__rosidl_typesupport_fastrtps_c)
    target_link_libraries(unitree_g1_rmw_wrapper PRIVATE
        unitree_api::unitree_api__rosidl_typesupport_fastrtps_c
        unitree_hg::unitree_hg__rosidl_typesupport_fastrtps_c
    )
else()
    target_link_libraries(unitree_g1_rmw_wrapper PRIVATE
        "${UNITREE_API_INSTALL}/lib/libunitree_api__rosidl_typesupport_fastrtps_c.so"
        "${UNITREE_ROS2_INSTALL}/lib/libunitree_hg__rosidl_typesupport_fastrtps_c.so"
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Export only the C ABI entry point (sdk_vtable). CXX_VISIBILITY_PRESET=hidden
    # alone leaks weak/template symbols pulled in from the ROS / rosidl objects;
    # the shared version script forces everything except sdk_vtable to local
    # linkage. --exclude-libs,ALL additionally hides symbols from static archives.
    target_link_options(unitree_g1_rmw_wrapper PRIVATE
        "-Wl,--version-script=${PROJECT_ROOT}/sdk_wrapper/export.map"
        "-Wl,--exclude-libs,ALL"
        "-Wl,--no-undefined"
    )
endif()
