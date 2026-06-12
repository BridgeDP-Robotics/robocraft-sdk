# sdk_diag — standalone, dependency-minimal SDK wrapper diagnostic & package checker.
#
# Can be built on its own:
#   cmake -S tools/sdk_diag -B build-sdk_diag && cmake --build build-sdk_diag
# or as part of the runtime build (add_subdirectory from the top-level project).
#
# The only external input it needs is the public interface header
# sdk_wrapper/interface/sdk_wrapper_interface.h. Override its location with
# -DSDK_WRAPPER_INTERFACE_DIR=<dir-containing-sdk_wrapper/>.

cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED PROJECT_NAME)
    project(sdk_diag LANGUAGES CXX)
endif()

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# Where to find sdk_wrapper/interface/sdk_wrapper_interface.h
if(NOT DEFINED SDK_WRAPPER_INTERFACE_DIR)
    get_filename_component(SDK_WRAPPER_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

find_package(Threads REQUIRED)

# Self-contained check library (no runtime/src dependency).
add_library(sdk_diag_check STATIC
    elf_inspect.cpp
    sha256.cpp
    yaml_lite.cpp
    manifest.cpp
    package_check.cpp
)
target_include_directories(sdk_diag_check PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDK_WRAPPER_INTERFACE_DIR}
)
target_link_libraries(sdk_diag_check PUBLIC ${CMAKE_DL_LIBS})

add_executable(sdk_diag sdk_diag.cpp)
target_link_libraries(sdk_diag sdk_diag_check Threads::Threads ${CMAKE_DL_LIBS})

# When built inside the runtime tree, drop the binary next to the other tools.
if(DEFINED ROBOCRAFT_ENTRY_OUTPUT_DIR)
    set_target_properties(sdk_diag PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ROBOCRAFT_ENTRY_OUTPUT_DIR}")
elseif(DEFINED ROBOCRAFT_OUTPUT_DIR)
    set_target_properties(sdk_diag PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ROBOCRAFT_OUTPUT_DIR}/runtime")
endif()
