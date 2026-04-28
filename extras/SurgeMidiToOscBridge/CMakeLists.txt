cmake_minimum_required(VERSION 3.15)

project(SurgeMidiToOscBridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SURGE_JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/JUCE" CACHE STRING "Path to JUCE library source tree")

add_subdirectory(${SURGE_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)

juce_add_gui_app(SurgeMidiToOscBridge
    PRODUCT_NAME "Surge MIDI To OSC Bridge"
    BUNDLE_ID org.surge-synth-team.SurgeMidiToOscBridge)

juce_generate_juce_header(SurgeMidiToOscBridge)

target_sources(SurgeMidiToOscBridge PRIVATE
    Source/Main.cpp)

target_compile_definitions(SurgeMidiToOscBridge PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)

target_link_libraries(SurgeMidiToOscBridge PRIVATE
    juce::juce_audio_devices
    juce::juce_gui_extra
    juce::juce_osc
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
