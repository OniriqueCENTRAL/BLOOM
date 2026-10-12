cmake_minimum_required(VERSION 3.22)

project(Bloom VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # Universal binary, and the oldest macOS the JUCE 9 runtime supports.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# JUCE is looked for next to this file first, then in the PitchBox checkout
# beside this one, so the plugins of the range can share a single copy.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    set(BLOOM_JUCE_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
else()
    set(BLOOM_JUCE_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../PitchBox/JUCE")
endif()

set(BLOOM_JUCE_DIR "${BLOOM_JUCE_DEFAULT}" CACHE PATH "Path to JUCE")

if(NOT EXISTS "${BLOOM_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${BLOOM_JUCE_DIR}.\n"
        "Run:  git clone --depth 1 --branch 9.0.3 https://github.com/juce-framework/JUCE.git")
endif()

add_subdirectory("${BLOOM_JUCE_DIR}" JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The plugin. The codes are the ones BLOOM has always had, so that a project
# saved with an earlier build finds it again.
set(BLOOM_FORMATS VST3)

if(APPLE)
    list(APPEND BLOOM_FORMATS AU)
endif()

# The system VST3 folder on Windows is under Program Files, which a normal build
# cannot write to, so installing after the build is opt-in there.
if(WIN32)
    option(BLOOM_COPY_AFTER_BUILD "Install the plugin after building" OFF)
else()
    option(BLOOM_COPY_AFTER_BUILD "Install the plugin after building" ON)
endif()

juce_add_plugin(Bloom
    COMPANY_NAME "Relic Plugins"
    COMPANY_WEBSITE "www.RelicPlugins.com"
    BUNDLE_ID "com.relicplugins.bloom"
    PLUGIN_MANUFACTURER_CODE Rahs
    PLUGIN_CODE Blm1
    FORMATS ${BLOOM_FORMATS}
    PRODUCT_NAME "BLOOM"
    PLUGIN_NAME "BLOOM"
    DESCRIPTION "BLOOM"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx EQ
    COPY_PLUGIN_AFTER_BUILD ${BLOOM_COPY_AFTER_BUILD})

# The sources include <JuceHeader.h>, as a Projucer project does.
juce_generate_juce_header(Bloom)

target_sources(Bloom PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/BloomScene.cpp)

# The licence every plugin on ONIRIQUE CENTRAL carries: the folder is copied as it is from
# plugin-license/ in the ONIRIQUE CENTRAL repository, and is only ever changed there.
include(License/OniriqueLicense.cmake)
onirique_add_license(Bloom)

target_compile_definitions(Bloom PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

# The sources carry accented text as UTF-8 bytes, and MSVC has to be told. The scene's data is one very large file.
if(MSVC)
    target_compile_options(Bloom PRIVATE /utf-8 /bigobj)
endif()

target_link_libraries(Bloom
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

enable_testing()

# A development tool, not part of the product: renders the editor to a PNG
# without a host, and puts the plugin through what a host does to it.
# Built only when asked for by name.
add_executable(bloom_snapshot EXCLUDE_FROM_ALL tools/snapshot.cpp)

target_include_directories(bloom_snapshot PRIVATE
    $<TARGET_PROPERTY:Bloom,INCLUDE_DIRECTORIES>)

target_compile_definitions(bloom_snapshot PRIVATE
    $<TARGET_PROPERTY:Bloom,COMPILE_DEFINITIONS>)

target_link_libraries(bloom_snapshot PRIVATE Bloom)
