cmake_minimum_required(VERSION 3.20)
project(OmniBridgeOS LANGUAGES C ASM)

# 可选 CMake 包装。主构建系统仍然是 Makefile。
# 使用方式：
#   cmake -S . -B build-cmake -G Ninja
#   cmake --build build-cmake
#
# HUMAN REVIEW REQUIRED: 当前 CMake 仅调用 make，不直接管理 UEFI PE/COFF 链接参数。
add_custom_target(ombridge ALL
    COMMAND ${CMAKE_COMMAND} -E env make all
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
)