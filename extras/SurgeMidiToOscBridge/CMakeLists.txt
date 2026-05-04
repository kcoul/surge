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

find_package(HailoRT REQUIRED)
find_package(CURL REQUIRED)

set(HAILO_APPS_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/hailo-apps/hailo_apps/cpp)
set(HAILO_APPS_EXTERNAL_DIR ${HAILO_APPS_CPP_DIR}/external)

if(NOT TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    add_subdirectory(${HAILO_APPS_EXTERNAL_DIR}/yaml-cpp
                     ${CMAKE_BINARY_DIR}/hailo-apps-yaml-cpp
                     EXCLUDE_FROM_ALL)
endif()

if(TARGET yaml-cpp::yaml-cpp)
    set(HAILO_APPS_YAMLCPP_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(HAILO_APPS_YAMLCPP_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "yaml-cpp target missing for Hailo Apps ResourcesManager")
endif()

juce_add_gui_app(SurgeMidiToOscBridge
    PRODUCT_NAME "SurgeMidiToOscBridge"
    BUNDLE_ID org.surge-synth-team.SurgeMidiToOscBridge)

juce_generate_juce_header(SurgeMidiToOscBridge)

target_sources(SurgeMidiToOscBridge PRIVATE
    Source/Main.cpp
    ${HAILO_APPS_CPP_DIR}/common/resources_manager.cpp)

set_source_files_properties(${HAILO_APPS_CPP_DIR}/common/resources_manager.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-error=deprecated-declarations;-Wno-error=unused-variable;-Wno-error=unused-function;-Wno-error=sign-conversion>")

target_compile_definitions(SurgeMidiToOscBridge PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    SURGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_include_directories(SurgeMidiToOscBridge PRIVATE
    ${HAILO_APPS_CPP_DIR}/common)

target_compile_options(SurgeMidiToOscBridge PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-error=shadow>)

target_link_libraries(SurgeMidiToOscBridge PRIVATE
    juce::juce_audio_devices
    juce::juce_gui_extra
    juce::juce_osc
    whisper
    HailoRT::libhailort
    ${HAILO_APPS_YAMLCPP_TARGET}
    CURL::libcurl
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
