cmake_minimum_required(VERSION 3.20)
project(chord-analyzer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
endif()

include(FetchContent)
FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(clap)

set(PLUGIN_SOURCES
    src/plugin.cpp
)

if(APPLE)
    list(APPEND PLUGIN_SOURCES src/gui_macos.mm)
endif()

add_library(chord-analyzer MODULE ${PLUGIN_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(chord-analyzer PRIVATE clap Threads::Threads)
target_include_directories(chord-analyzer PRIVATE src)

if(APPLE)
    set_source_files_properties(src/gui_macos.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
endif()

if(APPLE)
    set_target_properties(chord-analyzer PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        OUTPUT_NAME "ChordAnalyzer"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.example.chord-analyzer"
        MACOSX_BUNDLE_BUNDLE_NAME "ChordAnalyzer"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    )
    target_link_libraries(chord-analyzer PRIVATE
        "-framework Cocoa"
        "-framework Foundation"
    )
    target_link_options(chord-analyzer PRIVATE
        "-Wl,-exported_symbol,_clap_entry"
    )
endif()

if(UNIX AND NOT APPLE)
    set_target_properties(chord-analyzer PROPERTIES
        PREFIX ""
        SUFFIX ".clap"
        OUTPUT_NAME "ChordAnalyzer"
    )
endif()

if(WIN32)
    set_target_properties(chord-analyzer PROPERTIES
        PREFIX ""
        SUFFIX ".clap"
        OUTPUT_NAME "ChordAnalyzer"
    )
endif()
