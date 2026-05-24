cmake_minimum_required(VERSION 3.15)

project(SurgeMidiToOscBridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT COMMAND juce_add_gui_app)
    set(SURGE_JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/JUCE" CACHE STRING "Path to JUCE library source tree")
    add_subdirectory(${SURGE_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET whisper)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "Build whisper.cpp tests" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "Build whisper.cpp examples" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "Build whisper.cpp server" FORCE)
    set(GGML_BACKEND_DL OFF CACHE BOOL "Build ggml backends as dynamic libraries" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../libs/whisper.cpp
                     ${CMAKE_BINARY_DIR}/whisper.cpp
                     EXCLUDE_FROM_ALL)
endif()

set(BRIDGE_TARGET_SUPPORTS_HAILO FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "QNX" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(BRIDGE_TARGET_SUPPORTS_HAILO TRUE)
endif()

option(SURGE_MIDI_OSC_BRIDGE_ENABLE_HAILO
       "Enable the HailoRT Voice Command API backend for SurgeMidiToOscBridge"
       ${BRIDGE_TARGET_SUPPORTS_HAILO})

if(SURGE_MIDI_OSC_BRIDGE_ENABLE_HAILO AND NOT BRIDGE_TARGET_SUPPORTS_HAILO)
    message(STATUS "SurgeMidiToOscBridge: HailoRT disabled for ${CMAKE_SYSTEM_NAME}; supported on Linux, QNX, and Windows only")
    set(SURGE_MIDI_OSC_BRIDGE_ENABLE_HAILO OFF CACHE BOOL
        "Enable the HailoRT Voice Command API backend for SurgeMidiToOscBridge"
        FORCE)
endif()

set(BRIDGE_HAS_HAILO 0)
if(SURGE_MIDI_OSC_BRIDGE_ENABLE_HAILO)
    find_package(HailoRT REQUIRED)
    set(BRIDGE_HAS_HAILO 1)
else()
    message(STATUS "SurgeMidiToOscBridge: building without HailoRT backend")
endif()
# find_package(CURL REQUIRED)


juce_add_gui_app(SurgeMidiToOscBridge
    PRODUCT_NAME "SurgeMidiToOscBridge"
    BUNDLE_ID org.surge-synth-team.SurgeMidiToOscBridge)

target_sources(SurgeMidiToOscBridge PRIVATE
    Source/Main.cpp)

# Embedded VAD model.
# The embedding script is invoked by CMake at build time via add_custom_command,
# so the generated files live in the build directory and are never committed.
# The script only re-runs when the model file or the script itself is newer than
# the previously generated output — i.e. normal incremental-build semantics.
# To make the embedded VAD available: run tools/prepare-bridge-resources.sh
# (downloads the model), then reconfigure and build.
set(BRIDGE_VAD_MODEL "${CMAKE_SOURCE_DIR}/surge/libs/whisper.cpp/models/ggml-silero-v6.2.0.bin")
set(BRIDGE_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary_as_c.cmake")
set(BRIDGE_VAD_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(BRIDGE_EMBEDDED_VAD_H   "${BRIDGE_VAD_GEN_DIR}/embedded_vad_model.h")
set(BRIDGE_EMBEDDED_VAD_CPP "${BRIDGE_VAD_GEN_DIR}/embedded_vad_model.cpp")

if(EXISTS "${BRIDGE_VAD_MODEL}")
    message(STATUS "SurgeMidiToOscBridge: silero VAD model found — will embed if changed")
    file(MAKE_DIRECTORY "${BRIDGE_VAD_GEN_DIR}")
    add_custom_command(
        OUTPUT  "${BRIDGE_EMBEDDED_VAD_H}" "${BRIDGE_EMBEDDED_VAD_CPP}"
        COMMAND "${CMAKE_COMMAND}"
                    -DINPUT=${BRIDGE_VAD_MODEL}
                    -DOUTPUT_H=${BRIDGE_EMBEDDED_VAD_H}
                    -DOUTPUT_CPP=${BRIDGE_EMBEDDED_VAD_CPP}
                    -DSYMBOL=ggml_silero_vad_model
                    -P "${BRIDGE_EMBED_SCRIPT}"
        DEPENDS "${BRIDGE_VAD_MODEL}" "${BRIDGE_EMBED_SCRIPT}"
        COMMENT "Embedding silero VAD model"
    )
    target_sources(SurgeMidiToOscBridge PRIVATE "${BRIDGE_EMBEDDED_VAD_CPP}")
    target_include_directories(SurgeMidiToOscBridge PRIVATE "${BRIDGE_VAD_GEN_DIR}")
    target_compile_definitions(SurgeMidiToOscBridge PRIVATE BRIDGE_HAS_EMBEDDED_VAD=1)
else()
    message(STATUS "SurgeMidiToOscBridge: no silero VAD model — run tools/prepare-bridge-resources.sh, then reconfigure")
endif()


target_compile_definitions(SurgeMidiToOscBridge PRIVATE
    BRIDGE_HAS_HAILO=${BRIDGE_HAS_HAILO}
    JUCE_USE_CURL=1
    JUCE_WEB_BROWSER=0
    SURGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(SurgeMidiToOscBridge PRIVATE
    NOMINMAX=1)
endif()

target_compile_options(SurgeMidiToOscBridge PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-error=shadow>)

target_link_libraries(SurgeMidiToOscBridge PRIVATE
    juce::juce_audio_devices
    juce::juce_gui_extra
    juce::juce_osc
    whisper
    #CURL::libcurl
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(BRIDGE_HAS_HAILO)
    target_link_libraries(SurgeMidiToOscBridge PRIVATE HailoRT::libhailort)
endif()
