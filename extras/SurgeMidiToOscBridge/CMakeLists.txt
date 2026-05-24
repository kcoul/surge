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
find_package(CURL REQUIRED)

option(SURGE_MIDI_OSC_BRIDGE_DOWNLOAD_MODELS
       "Download model files during configure if missing" ON)

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
# Models are downloaded automatically at configure time when
# SURGE_MIDI_OSC_BRIDGE_DOWNLOAD_MODELS is ON (default).  For manual control,
# disable the option and use tools/prepare-bridge-resources.sh instead.
set(BRIDGE_VAD_MODEL "${CMAKE_SOURCE_DIR}/surge/libs/whisper.cpp/models/ggml-silero-v6.2.0.bin")
set(BRIDGE_WHISPER_MODELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/whisper.cpp/models")

# ---------- download models at configure time ----------------------------
if(SURGE_MIDI_OSC_BRIDGE_DOWNLOAD_MODELS)
    # Helper macro: download a file if it doesn't already exist.
    macro(_bridge_download _url _dest)
        if(NOT EXISTS "${_dest}")
            get_filename_component(_fname "${_dest}" NAME)
            message(STATUS "SurgeMidiToOscBridge: downloading ${_fname} ...")
            file(DOWNLOAD "${_url}" "${_dest}"
                 INACTIVITY_TIMEOUT 30
                 TIMEOUT 300
                 STATUS _dl_status)
            list(GET _dl_status 0 _dl_code)
            if(_dl_code EQUAL 0)
                message(STATUS "SurgeMidiToOscBridge: downloaded ${_fname}")
            else()
                list(GET _dl_status 1 _dl_msg)
                message(WARNING "SurgeMidiToOscBridge: failed to download ${_fname}: ${_dl_msg}")
                file(REMOVE "${_dest}")
            endif()
        endif()
    endmacro()

    # VAD model (embedded into binary)
    _bridge_download(
        "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin"
        "${BRIDGE_VAD_MODEL}")

    # Whisper CPU/GPU models (loaded at runtime)
    foreach(_model_name tiny base small)
        _bridge_download(
            "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${_model_name}.bin"
            "${BRIDGE_WHISPER_MODELS_DIR}/ggml-${_model_name}.bin")
    endforeach()

    # Hailo HEF models (only when HailoRT is enabled)
    if(BRIDGE_HAS_HAILO)
        set(_bridge_hailo_models_dir "${CMAKE_CURRENT_SOURCE_DIR}/Resources/models/hailo10h")
        file(MAKE_DIRECTORY "${_bridge_hailo_models_dir}")
        foreach(_hef_name Tiny Base Small)
            _bridge_download(
                "https://dev-public.hailo.ai/v5.3.0/blob/Whisper-${_hef_name}.hef"
                "${_bridge_hailo_models_dir}/Whisper-${_hef_name}.hef")
        endforeach()
    endif()
endif()

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
    message(STATUS "SurgeMidiToOscBridge: no silero VAD model — enable SURGE_MIDI_OSC_BRIDGE_DOWNLOAD_MODELS or run tools/prepare-bridge-resources.sh, then reconfigure")
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
    CURL::libcurl
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(BRIDGE_HAS_HAILO)
    target_link_libraries(SurgeMidiToOscBridge PRIVATE HailoRT::libhailort)
endif()

# ---------- copy runtime models next to binary ----------------------------
foreach(_model_name tiny base small)
    set(_src "${BRIDGE_WHISPER_MODELS_DIR}/ggml-${_model_name}.bin")
    if(EXISTS "${_src}")
        add_custom_command(TARGET SurgeMidiToOscBridge POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "$<TARGET_FILE_DIR:SurgeMidiToOscBridge>/models"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${_src}"
                    "$<TARGET_FILE_DIR:SurgeMidiToOscBridge>/models/ggml-${_model_name}.bin"
            COMMENT "Copying ggml-${_model_name}.bin next to binary")
    endif()
endforeach()

if(BRIDGE_HAS_HAILO)
    set(_bridge_hailo_models_dir "${CMAKE_CURRENT_SOURCE_DIR}/Resources/models/hailo10h")
    foreach(_hef_name Tiny Base Small)
        set(_src "${_bridge_hailo_models_dir}/Whisper-${_hef_name}.hef")
        if(EXISTS "${_src}")
            add_custom_command(TARGET SurgeMidiToOscBridge POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory
                        "$<TARGET_FILE_DIR:SurgeMidiToOscBridge>/models/hailo10h"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${_src}"
                        "$<TARGET_FILE_DIR:SurgeMidiToOscBridge>/models/hailo10h/Whisper-${_hef_name}.hef"
                COMMENT "Copying Whisper-${_hef_name}.hef next to binary")
        endif()
    endforeach()
endif()
