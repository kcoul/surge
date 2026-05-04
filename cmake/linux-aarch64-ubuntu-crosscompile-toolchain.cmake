#
# This Toolchain is set up for a set of defaults which works on ubuntu-20 cross compile
# to headless. Feel free to add a different toolchain config and then jut send us
# a pull request for different circumstances.
#
# If you make a toolchain in addition to the standard toolchain stuff, also
# set LINUX_ON_ARM to True and export LINUX_ON_ARM_COMPILE_OPTIONS
#
# sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 1. Force the cross-pkg-config wrapper
set(PKG_CONFIG_EXECUTABLE /usr/bin/aarch64-linux-gnu-pkg-config CACHE FILEPATH "pkg-config executable")

# 2. Tell pkg-config to ONLY look in the arm64 directories
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")

# 3. Explicitly override the variables JUCE is mis-detecting
set(ALSA_LIBRARIES "/usr/lib/aarch64-linux-gnu/libasound.so" CACHE FILEPATH "" FORCE)
set(FONTCONFIG_LIBRARIES "/usr/lib/aarch64-linux-gnu/libfontconfig.so" CACHE FILEPATH "" FORCE)
set(FREETYPE_LIBRARIES "/usr/lib/aarch64-linux-gnu/libfreetype.so" CACHE FILEPATH "" FORCE)
set(OPENGL_gl_LIBRARY "/usr/lib/aarch64-linux-gnu/libGL.so" CACHE FILEPATH "" FORCE)

# 1. Define the architecture triplet
set(ARCH_TRIPLET aarch64-linux-gnu)
set(CMAKE_LIBRARY_ARCHITECTURE ${ARCH_TRIPLET})

# 2. Force the Linker to prioritize the arm64 library directory
# This prepends the correct path to the linker's search list
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L/usr/lib/${ARCH_TRIPLET} -Wl,-rpath-link,/usr/lib/${ARCH_TRIPLET}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L/usr/lib/${ARCH_TRIPLET} -Wl,-rpath-link,/usr/lib/${ARCH_TRIPLET}")

# 3. Fix for ALSA (asound)
# By setting these CACHE variables, FindALSA.cmake will skip its search
# and use these correct ARM64 paths immediately.
set(ALSA_LIBRARY "/usr/lib/${ARCH_TRIPLET}/libasound.so" CACHE FILEPATH "ARM64 ALSA Lib")
set(ALSA_INCLUDE_DIR "/usr/include" CACHE PATH "ALSA Include")

# 4. Compilers
set(tools /usr/bin)
set(CMAKE_C_COMPILER ${tools}/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER ${tools}/aarch64-linux-gnu-g++)

# 5. Search Behavior: Whitelist ONLY the arm64 and generic paths
#
# HAILO_CROSS_PREFIX points to the root of the extracted arm64 HailoRT deb.
# Set it here or override via -DHAILO_CROSS_PREFIX=<path> at configure time.
# See extras/SurgeMidiToOscBridge/docs/cross-compile-linux-aarch64-ubuntu.md
# for how to create this directory.
set(HAILO_CROSS_PREFIX "/opt/hailo-cross/arm64" CACHE PATH
    "Root of arm64 HailoRT deb extraction (for find_package(HailoRT))")

# HAILO_CROSS_PREFIX is listed first so the arm64 cmake configs are found
# before the host-architecture HailoRT that may also be installed on the system.
set(CMAKE_FIND_ROOT_PATH ${HAILO_CROSS_PREFIX} /usr/lib/${ARCH_TRIPLET} /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 6. Pkg-config isolation
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${ARCH_TRIPLET}/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")