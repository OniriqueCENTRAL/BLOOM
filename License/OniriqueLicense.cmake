# Module de licence ONIRIQUE. Dans le CMakeLists.txt du plugin, après juce_add_plugin :
#
#     include(License/OniriqueLicense.cmake)
#     onirique_add_license(MonPlugin)
#
# Le projet doit déclarer le langage C (project(... LANGUAGES C CXX), ou project(...) sans LANGUAGES).

set(ONIRIQUE_LICENSE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(onirique_add_license target)
    set(dir "${ONIRIQUE_LICENSE_DIR}")

    # Les polices de la page d'activation, sous un nom qui ne heurte pas le BinaryData du plugin.
    juce_add_binary_data(${target}LicenseData
        HEADER_NAME OniriqueLicenseData.h
        NAMESPACE OniriqueLicenseData
        SOURCES
            "${dir}/Fonts/Unbounded800.ttf"
            "${dir}/Fonts/HankenGrotesk600.ttf"
            "${dir}/Fonts/JetBrainsMono500.ttf"
            "${dir}/Fonts/JetBrainsMono600.ttf")
    set_target_properties(${target}LicenseData PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

    set(sources
        "${dir}/License.cpp"
        "${dir}/LicenseCore.cpp"
        "${dir}/LicenseOverlay.cpp"
        "${dir}/tweetnacl.c")
    target_sources(${target} PRIVATE ${sources})
    target_include_directories(${target} PRIVATE "${dir}/..")

    # Les textes sont en UTF-8 dans les sources.
    if(MSVC)
        set_source_files_properties(${sources} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "/utf-8")
    endif()

    target_link_libraries(${target} PRIVATE
        ${target}LicenseData
        juce::juce_audio_basics
        juce::juce_gui_basics)
endfunction()

# Pour un plugin qui n'utilise pas JUCE : la vérification et le bruit du mode démo seulement
# (LicenseCore.h). La pastille et la page d'activation sont à dessiner avec l'interface du plugin.
function(onirique_add_license_core target)
    set(dir "${ONIRIQUE_LICENSE_DIR}")
    target_sources(${target} PRIVATE "${dir}/LicenseCore.cpp" "${dir}/tweetnacl.c")
    target_include_directories(${target} PRIVATE "${dir}/..")
    if(APPLE)
        target_link_libraries(${target} PRIVATE "-framework IOKit" "-framework CoreFoundation")
    elseif(WIN32)
        target_link_libraries(${target} PRIVATE advapi32 shell32)
    endif()
endfunction()
