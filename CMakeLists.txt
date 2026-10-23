cmake_minimum_required(VERSION 3.21)
project(rttyPad VERSION 0.3.0 LANGUAGES C CXX)

option(WITH_STANDALONE_TX "Standalone FSK TX via miniaudio (no deskHPSDR needed)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets WebSockets Network)

add_library(rtty_core STATIC
    src/Baudot.c
    src/RTTYRx.c
)
target_include_directories(rtty_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(NOT WIN32)
    target_link_libraries(rtty_core PUBLIC m)
endif()

# ---- Standalone TX (miniaudio, header-only — no extra dependency) ----
if(WITH_STANDALONE_TX)
    add_library(rtty_standalone STATIC
        src/rtty_engine_standalone.c
        src/StandaloneRTTYTx.cpp
    )
    target_include_directories(rtty_standalone PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(rtty_standalone PUBLIC HAVE_STANDALONE_TX=1)
    # miniaudio needs dl + pthread on Linux, nothing extra on Windows/macOS
    if(UNIX AND NOT APPLE)
        target_link_libraries(rtty_standalone PUBLIC dl pthread m)
    elseif(APPLE)
        target_link_libraries(rtty_standalone PUBLIC
            "-framework CoreAudio" "-framework AudioToolbox"
            "-framework CoreFoundation" m)
    endif()
    target_link_libraries(rtty_standalone PUBLIC Qt6::Core)
    message(STATUS "Standalone TX: ON  (miniaudio, header-only)")
else()
    message(STATUS "Standalone TX: OFF (TCI/deskHPSDR only)")
endif()

add_executable(rttyPad
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/RTTYScopeWidget.cpp
    src/RTTYScopeWidget.h
    src/RTTYIQPanWidget.cpp
    src/RTTYIQPanWidget.h
    src/TCIClient.cpp
    src/TCIClient.h
    src/NativeRTTYTx.cpp
    src/NativeRTTYTx.h
    Resources/icons.qrc
)
target_link_libraries(rttyPad PRIVATE Qt6::Widgets Qt6::WebSockets Qt6::Network rtty_core
    $<$<BOOL:${WITH_STANDALONE_TX}>:rtty_standalone>)
if(WITH_STANDALONE_TX)
    target_compile_definitions(rttyPad PRIVATE HAVE_STANDALONE_TX=1)
endif()
target_compile_definitions(rttyPad PRIVATE RTTYPAD_VERSION=\"${PROJECT_VERSION}\")

# Use the Windows GUI subsystem (no additional console window).
if(WIN32)
    set_target_properties(rttyPad PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE)
    math(EXPR RTTYPAD_BUNDLE_BUILD_VERSION
        "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
    configure_file(Resources/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/rttyPad-Info.plist" @ONLY)
    set_source_files_properties(Resources/AppIcon.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_source_files_properties(Resources/PkgInfo PROPERTIES MACOSX_PACKAGE_LOCATION ".")
    target_sources(rttyPad PRIVATE Resources/AppIcon.icns Resources/PkgInfo)
    set_target_properties(rttyPad PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/rttyPad-Info.plist"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        MACOSX_BUNDLE_BUNDLE_NAME "rttyPad"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.dl1bz.rttyPad"
        MACOSX_BUNDLE_BUNDLE_VERSION "${RTTYPAD_BUNDLE_BUILD_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()

# Embed the icon in the Windows executable (Explorer/taskbar).
if(WIN32)
    target_sources(rttyPad PRIVATE Resources/appicon.rc)
endif()
