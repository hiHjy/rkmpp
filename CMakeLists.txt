cmake_minimum_required(VERSION 3.16)

# 优先使用环境里已经 source 过的交叉编译设置。
# 如果已经 source 了 environment-setup，通常 CC/CXX/PKG_CONFIG_* 都会可用。
if(NOT DEFINED CMAKE_SYSROOT AND DEFINED ENV{SYSROOT})
    set(CMAKE_SYSROOT "$ENV{SYSROOT}")
endif()

if(NOT CMAKE_C_COMPILER AND DEFINED ENV{CC})
    set(CMAKE_C_COMPILER "$ENV{CC}")
endif()

if(NOT CMAKE_CXX_COMPILER AND DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER "$ENV{CXX}")
endif()

project(rk3568_media_demo C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDK / SYSROOT / FFmpeg 安装路径
set(SDK_ROOT "$ENV{SDK}")
if(NOT SDK_ROOT)
    set(SDK_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# MPP 源码根目录。
# 这些头文件里有一部分（如 mpp_common.h / mpp_mem.h / mpp_time.h）
# 不会安装到 sysroot/usr/include/rockchip，下游工程需要额外指向
# SDK 里的 external/mpp 源码树。
set(MPP_ROOT "${SDK_ROOT}/external/mpp")
if(NOT EXISTS "${MPP_ROOT}/inc/rk_mpi.h")
    set(MPP_ROOT "$ENV{MPP_ROOT}")
endif()

set(SYSROOT_DIR "$ENV{SYSROOT}")
if(NOT SYSROOT_DIR)
    set(SYSROOT_DIR "${SDK_ROOT}/buildroot/output/rockchip_atk_dlrk3568/host/aarch64-buildroot-linux-gnu/sysroot")
endif()

set(FFMPEG_ROOT "${SYSROOT_DIR}/opt/ffmpeg61")

# 让 CMake 的 find_* / pkg-config 都优先从交叉 sysroot 查找
set(CMAKE_FIND_ROOT_PATH "${SYSROOT_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config 环境
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SYSROOT_DIR}")
set(ENV{PKG_CONFIG_PATH}
    "${FFMPEG_ROOT}/lib/pkgconfig:${FFMPEG_ROOT}/share/pkgconfig:${SYSROOT_DIR}/usr/lib/pkgconfig:${SYSROOT_DIR}/usr/share/pkgconfig")

find_package(PkgConfig REQUIRED)

# FFmpeg
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# Rockchip MPP / RGA / DRM
pkg_check_modules(MPP REQUIRED IMPORTED_TARGET rockchip_mpp)
pkg_check_modules(RGA REQUIRED IMPORTED_TARGET librga)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)

# 你可以把这个默认源码换成你自己的，不需要改 CMake 主体。
set(APP_SOURCES
    "${CMAKE_SOURCE_DIR}/main.c"
    "${CMAKE_SOURCE_DIR}/rkmpp_dec.c"
)

add_executable(rk_media_demo ${APP_SOURCES})

# 如果源码里会直接 include SDK 内部头，也顺手补上常用目录。
target_include_directories(rk_media_demo PRIVATE
    "${SYSROOT_DIR}/usr/include"
    "${SYSROOT_DIR}/usr/include/rockchip"
    "${SYSROOT_DIR}/usr/include/rga"
    "${FFMPEG_ROOT}/include"
    "${MPP_ROOT}/inc"
    "${MPP_ROOT}/osal/inc"
    "${MPP_ROOT}/mpp/inc"
    "${MPP_ROOT}/utils"
)

# 链接顺序：先业务 target，再 FFmpeg / MPP / RGA / DRM / pthread / dl / m
# pkg_check_modules(... IMPORTED_TARGET ...) 会自动带上包含路径和库路径。
target_link_libraries(rk_media_demo PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::SWRESAMPLE
    PkgConfig::MPP
    PkgConfig::RGA
    PkgConfig::DRM
    pthread
    dl
    m
)

# 输出目录放到 SDK 根目录下的 build_out，方便找。
set_target_properties(rk_media_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
)

message(STATUS "SDK_ROOT        = ${SDK_ROOT}")
message(STATUS "MPP_ROOT        = ${MPP_ROOT}")
message(STATUS "SYSROOT_DIR     = ${SYSROOT_DIR}")
message(STATUS "FFMPEG_ROOT     = ${FFMPEG_ROOT}")
message(STATUS "PKG_CONFIG_PATH = $ENV{PKG_CONFIG_PATH}")
message(STATUS "APP_SOURCES     = ${APP_SOURCES}")
