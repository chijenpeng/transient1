cmake_minimum_required(VERSION 3.22)

project(Transient1 VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# The product name appears here, in the editor title and in README.md.
set(TRANSIENT_NAME "Transient1")
option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(MSVC)
    set(TRANSIENT_DSP_OPTIONS /W4)
    set(TRANSIENT_TEST_OPTIONS /W4)
else()
    set(TRANSIENT_DSP_OPTIONS -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    set(TRANSIENT_TEST_OPTIONS -Wall -Wextra)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Prefer a local JUCE checkout for day-to-day development. A fresh clone remains buildable:
# when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The DSP is JUCE-free: the engine, its tests and the renderer's core build without a host or a screen.
add_library(transient1_dsp STATIC Source/dsp/TransientShaper.cpp)
target_include_directories(transient1_dsp PUBLIC Source)
target_compile_options(transient1_dsp PRIVATE ${TRANSIENT_DSP_OPTIONS})

juce_add_plugin(Transient1
    PRODUCT_NAME "${TRANSIENT_NAME}"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.transient1"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE Trn1
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

# Inter and JetBrains Mono, both SIL Open Font License (licences are next to the fonts).
juce_add_binary_data(Transient1Assets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)
set_target_properties(Transient1Assets PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(Transient1 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Transient1 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Transient1
    PRIVATE
        transient1_dsp
        Transient1Assets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Helper for the console tools that include the processor and editor sources.
function(transient1_console_app target)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")
    target_sources(${target} PRIVATE ${ARGN})
    target_include_directories(${target} PRIVATE Source)
    target_compile_definitions(${target} PRIVATE
        JucePlugin_Name="${TRANSIENT_NAME}"
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(${target} PRIVATE
        transient1_dsp
        Transient1Assets
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endfunction()

# Renders the editor to a PNG with no host or screen (see tests/snapshot.cpp).
transient1_console_app(transient1_snapshot tests/snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)

# Tests: prints PASS/FAIL per check, non-zero exit on any failure. Includes the processor.
transient1_console_app(transient1_test tests/transient1_test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_options(transient1_test PRIVATE ${TRANSIENT_TEST_OPTIONS})
enable_testing()
add_test(NAME transient1_test COMMAND transient1_test)
