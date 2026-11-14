cmake_minimum_required(VERSION 3.22)

# Runtime estatico: el plugin no necesita instalar nada extra en Windows
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(VocalChain VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(VocalChain
    COMPANY_NAME "VocalChain"
    PLUGIN_MANUFACTURER_CODE Vcha
    PLUGIN_CODE Vch1
    FORMATS VST3
    PRODUCT_NAME "VocalChain"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx)

target_sources(VocalChain PRIVATE src/PluginProcessor.cpp)

target_compile_definitions(VocalChain PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(VocalChain
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
