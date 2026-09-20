cmake_minimum_required(VERSION 3.21)
project(Singularity VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Multimedia OpenGL OpenGLWidgets)
qt_standard_project_setup()

# Where the finished program goes.
#
# Deliberately not the build folder. CMake, Ninja and moc between them leave
# forty odd files and directories wherever they work, and an exe sitting in the
# middle of that is hard to find and impossible to hand to anybody.
if(NOT DEFINED SINGULARITY_DIST_DIR)
    set(SINGULARITY_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")
endif()

qt_add_executable(Singularity WIN32
    src/main.cpp

    src/core/AppConfig.h
    src/core/AppConfig.cpp
    src/core/Logger.h
    src/core/Logger.cpp
    src/core/TokenStore.h
    src/core/TokenStore.cpp
    src/core/DiscordIdentity.h
    src/core/DiscordIdentity.cpp
    src/core/AuthClient.h
    src/core/AuthClient.cpp
    src/core/RestClient.h
    src/core/RestClient.cpp
    src/core/GatewayClient.h
    src/core/GatewayClient.cpp
    src/core/VoiceConnection.h
    src/core/VoiceConnection.cpp
    src/core/DaveSession.h
    src/core/DaveSession.cpp
    src/core/MessageStore.h
    src/core/MessageStore.cpp

    src/plugin/Plugin.h
    src/plugin/PluginHost.h
    src/plugin/PluginHost.cpp
    src/plugin/builtin/SilentTypingPlugin.h
    src/plugin/builtin/SilentTypingPlugin.cpp
    src/plugin/builtin/MessageLoggerPlugin.h
    src/plugin/builtin/MessageLoggerPlugin.cpp
    src/plugin/builtin/TimestampsPlugin.h
    src/plugin/builtin/TimestampsPlugin.cpp
    src/plugin/builtin/QuickTextPlugin.h
    src/plugin/builtin/QuickTextPlugin.cpp
    src/plugin/builtin/PresenceHintsPlugin.h
    src/plugin/builtin/PresenceHintsPlugin.cpp
    src/plugin/builtin/AnonymousPlugin.h
    src/plugin/builtin/AnonymousPlugin.cpp
    src/plugin/builtin/NitroWatchPlugin.h
    src/plugin/builtin/NitroWatchPlugin.cpp
    src/plugin/builtin/MicShaperPlugin.h
    src/plugin/builtin/MicShaperPlugin.cpp

    src/ui/Theme.h
    src/ui/Theme.cpp
    src/ui/AuroraWidget.h
    src/ui/AuroraWidget.cpp
    src/ui/LoginDialog.h
    src/ui/LoginDialog.cpp
    src/ui/PluginsDialog.h
    src/ui/PluginsDialog.cpp
    src/ui/LogDialog.h
    src/ui/LogDialog.cpp
    src/ui/ChatView.h
    src/ui/ChatView.cpp
    src/ui/CallView.h
    src/ui/CallView.cpp
    src/core/VideoDecoder.h
    src/core/VideoDecoder.cpp
    src/ui/MediaCache.h
    src/ui/MediaCache.cpp
    src/ui/ListDelegates.h
    src/ui/ListDelegates.cpp
    src/ui/AnimatedImage.h
    src/ui/AnimatedImage.cpp
    src/ui/AudioMeter.h
    src/ui/AudioMeter.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsDialog.cpp
    src/ui/FlowLayout.h
    src/ui/FlowLayout.cpp
    src/ui/ImageViewer.h
    src/ui/ImageViewer.cpp
    src/ui/FriendsPage.h
    src/ui/FriendsPage.cpp
    src/ui/RailLayout.h
    src/ui/RailLayout.cpp
    src/ui/ProfileDialog.h
    src/ui/ProfileDialog.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

set_target_properties(Singularity PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SINGULARITY_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SINGULARITY_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SINGULARITY_DIST_DIR}"
)

qt_add_resources(Singularity brand
    PREFIX "/brand"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/brand"
    FILES brand/singularity.png
)

if(WIN32)
    set(SINGULARITY_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/brand/singularity.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/brand/singularity.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/singularity.rc"
        @ONLY
    )
    target_sources(Singularity PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/singularity.rc")
endif()

# Opus compresses the sound, libsodium encrypts it. Both are built or shipped
# under third_party and linked statically, so the exe stays self contained.
set(SINGULARITY_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(OPUS_INSTALL "${SINGULARITY_THIRD_PARTY}/opus-install")
set(SODIUM_ROOT "${SINGULARITY_THIRD_PARTY}/libsodium")

# Discord requires end-to-end encrypted voice on every call since March 2026,
# so joining one at all needs their DAVE library. It is built from
# third_party/libdave; see the README for the exact command.
set(DAVE_ROOT "${SINGULARITY_THIRD_PARTY}/libdave/cpp")
set(DAVE_LIBS "${DAVE_ROOT}/build/vcpkg_installed/x64-windows-static-md/lib")
set(SINGULARITY_HAVE_DAVE OFF)

if(EXISTS "${DAVE_ROOT}/build/libdave.lib")
    set(SINGULARITY_HAVE_DAVE ON)
    message(STATUS "Singularity: end-to-end encrypted voice is available")
else()
    message(WARNING "Singularity: libdave is missing, so voice calls cannot be joined. "
                    "Everything else still builds.")
endif()

if(NOT EXISTS "${OPUS_INSTALL}/lib/opus.lib")
    message(FATAL_ERROR "Opus is missing. Build it into third_party/opus-install first.")
endif()
if(NOT EXISTS "${SODIUM_ROOT}/x64/Release/v143/static/libsodium.lib")
    message(FATAL_ERROR "libsodium is missing from third_party/libsodium.")
endif()

target_include_directories(Singularity PRIVATE
    src
    "${OPUS_INSTALL}/include"
    "${SODIUM_ROOT}/include"
)

# The prebuilt libsodium is a static library, which its headers must be told.
target_compile_definitions(Singularity PRIVATE SODIUM_STATIC=1)

target_link_libraries(Singularity PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    "${OPUS_INSTALL}/lib/opus.lib"
    "${SODIUM_ROOT}/x64/Release/v143/static/libsodium.lib"
    # Crypt32 seals the saved session with the Windows account's own key.
    Crypt32
    # Dwmapi colours the title bar, which belongs to Windows rather than Qt.
    Dwmapi
)

# ---------------------------------------------------------------------------
# FFmpeg, for decoding other people's cameras and screens.
#
# Singularity brings its own rather than borrowing the one Qt ships. Qt's is built for
# Qt Multimedia and comes with no headers or link libraries, and its version
# moves whenever Qt does. These are the LGPL builds, linked dynamically, so
# nothing here imposes the GPL on this project.
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "${SINGULARITY_THIRD_PARTY}/ffmpeg")

if(EXISTS "${FFMPEG_ROOT}/lib/avcodec.lib")
    set(SINGULARITY_HAVE_VIDEO ON)
    message(STATUS "Singularity: video decoding is available")

    target_compile_definitions(Singularity PRIVATE SINGULARITY_HAVE_VIDEO=1)
    target_include_directories(Singularity PRIVATE "${FFMPEG_ROOT}/include")
    target_link_libraries(Singularity PRIVATE
        "${FFMPEG_ROOT}/lib/avcodec.lib"
        "${FFMPEG_ROOT}/lib/avutil.lib"
        "${FFMPEG_ROOT}/lib/swscale.lib"
    )

    # The runtime has to sit beside the exe. These are named avcodec-62 and so
    # on, which is not what Qt ships, so the two cannot be confused.
    add_custom_command(TARGET Singularity POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_ROOT}/bin/avcodec-62.dll"
            "${FFMPEG_ROOT}/bin/avutil-60.dll"
            "${FFMPEG_ROOT}/bin/swscale-9.dll"
            "${FFMPEG_ROOT}/bin/swresample-6.dll"
            "$<TARGET_FILE_DIR:Singularity>"
        COMMENT "Copying the FFmpeg runtime beside Singularity")
else()
    message(STATUS "Singularity: no FFmpeg in third_party, so video cannot be shown. "
                   "See the README for how to fetch it.")
endif()

if(SINGULARITY_HAVE_DAVE)
    target_compile_definitions(Singularity PRIVATE SINGULARITY_HAVE_DAVE=1)
    target_include_directories(Singularity PRIVATE "${DAVE_ROOT}/includes")

    # Order matters for a static link: libdave leans on mlspp, mlspp on hpke
    # and the syntax helpers, and all of them on OpenSSL underneath.
    target_link_libraries(Singularity PRIVATE
        "${DAVE_ROOT}/build/libdave.lib"
        "${DAVE_LIBS}/mlspp.lib"
        "${DAVE_LIBS}/hpke.lib"
        "${DAVE_LIBS}/tls_syntax.lib"
        "${DAVE_LIBS}/bytes.lib"
        "${DAVE_LIBS}/libssl.lib"
        "${DAVE_LIBS}/libcrypto.lib"
        # OpenSSL's own Windows dependencies.
        ws2_32 crypt32 user32 advapi32
    )
endif()

set_target_properties(Singularity PROPERTIES
    OUTPUT_NAME "Singularity"
)
