cmake_minimum_required(VERSION 3.21)
project(rttyPad VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets WebSockets Network SerialPort)

add_library(rtty_core STATIC
    src/Baudot.c
    src/RTTYRx.c
)
target_include_directories(rtty_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(NOT WIN32)
    target_link_libraries(rtty_core PUBLIC m)
endif()

# ---- Standalone audio engine (miniaudio, header-only — no extra dependency) ----
add_library(rtty_standalone STATIC
    src/rtty_engine_standalone.c
    src/StandaloneRTTYTx.cpp
    src/StandaloneRTTYRx.cpp
)
target_include_directories(rtty_standalone PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# miniaudio needs dl + pthread on Linux, nothing extra on Windows/macOS
if(UNIX AND NOT APPLE)
    target_link_libraries(rtty_standalone PUBLIC dl pthread m)
elseif(APPLE)
    target_link_libraries(rtty_standalone PUBLIC
        "-framework CoreAudio" "-framework AudioToolbox"
        "-framework CoreFoundation" m)
endif()
target_link_libraries(rtty_standalone PUBLIC Qt6::Core)

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
    src/RigctlPtt.cpp
    src/RigctlPtt.h
    src/SerialPtt.cpp
    src/SerialPtt.h
    Resources/icons.qrc
)
target_link_libraries(rttyPad PRIVATE Qt6::Widgets Qt6::WebSockets Qt6::Network Qt6::SerialPort rtty_core rtty_standalone)
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
