cmake_minimum_required(VERSION 3.16)

project(tsmedia VERSION 2.1.0 LANGUAGES CXX RC)

set(PLUGIN_NAME        "TS Media chat")
set(PLUGIN_AUTHOR      "MehdiHttp")
# Shown in TeamSpeak's Addons list; scripts/build.ps1 reads it from here for the package's installer text.
set(PLUGIN_DESCRIPTION "Discord-style images, GIFs, videos and files in the TeamSpeak chat. Drop or paste files into the chat; everyone with the plugin sees and plays them inline.")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# TeamSpeak 3.6 ships Qt 5.15.2 (MSVC 2019 x64). The plugin must be built against the same Qt version
# and may only use modules TeamSpeak ships (Core, Gui, Widgets, ...; NOT Multimedia or Concurrent).
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)

# TeamSpeak loads <name>_win64.dll in the 64-bit client and <name>_win32.dll in the 32-bit one.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TSMEDIA_ARCH win64)
else()
    set(TSMEDIA_ARCH win32)
endif()

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)
configure_file(src/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc @ONLY)

# _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR: binaries from VS 2022 17.10+ crash in std::mutex when the machine has
# an older msvcp140.dll (Microsoft's documented workaround). Users' Visual C++ runtimes come with TeamSpeak
# and may be older than our compiler.
set(TSMEDIA_COMMON_OPTIONS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
set(TSMEDIA_MF_LIBS mfplat mfreadwrite mfuuid mf d3d11 dxgi ole32 oleaut32 propsys)

# Windows N editions have no Media Foundation unless the Media Feature Pack is installed. Delay-load it
# so the plugin still loads there (without video); src/video/mfvideo.cpp checks for the DLLs first.
function(tsmedia_delay_load_media_foundation target)
    if(MSVC)
        target_link_options(${target} PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
        target_link_libraries(${target} PRIVATE delayimp)
    endif()
endfunction()

add_library(tsmedia SHARED
    src/plugin.cpp
    src/ts3api.cpp            src/ts3api.h
    src/i18n.cpp              src/i18n.h
    src/uiutil.cpp            src/uiutil.h
    src/blurhash.cpp          src/blurhash.h
    src/medialink.cpp         src/medialink.h
    src/mediaprobe.cpp        src/mediaprobe.h
    src/settings.cpp          src/settings.h
    src/core.cpp              src/core.h
    src/previewrenderer.cpp   src/previewrenderer.h
    src/inlinemedia.cpp       src/inlinemedia.h
    src/chatintegration.cpp   src/chatintegration.h
    src/uploadtoast.cpp       src/uploadtoast.h
    src/mediaviewer.cpp       src/mediaviewer.h
    src/settingsdialog.cpp    src/settingsdialog.h
    src/video/mfvideo.cpp     src/video/mfvideo.h
    # 2.2 drag-out and per-server settings
    src/filenames.cpp         src/filenames.h
    src/filedrag.cpp          src/filedrag.h
    src/dragpixmap.cpp        src/dragpixmap.h
    src/serversettings.cpp    src/serversettings.h
    src/serverssection.cpp    src/serverssection.h
    src/datasaver.cpp         src/datasaver.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc
)

target_include_directories(tsmedia PRIVATE
    src
    third_party/ts3client-pluginsdk/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_definitions(tsmedia PRIVATE ${TSMEDIA_COMMON_OPTIONS})
if(MSVC)
    target_compile_options(tsmedia PRIVATE /W4 /utf-8 /permissive- /wd4100)
endif()
target_link_libraries(tsmedia PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets ${TSMEDIA_MF_LIBS})
tsmedia_delay_load_media_foundation(tsmedia)

# Extra logging, chat snapshots and self-test hooks for automated testing. Never ship with this on.
option(TSMEDIA_TESTHOOKS "Build with test hooks" OFF)
if(TSMEDIA_TESTHOOKS)
    target_compile_definitions(tsmedia PRIVATE TSMEDIA_TESTHOOKS)
endif()

set_target_properties(tsmedia PROPERTIES
    OUTPUT_NAME "tsmedia_${TSMEDIA_ARCH}"
    PREFIX ""
)

# ---- unit tests (pure logic: link format, chat message, blurhash, texts, UI helpers, settings checks) ---------------
option(TSMEDIA_BUILD_TESTS "Build unit tests" ON)
if(TSMEDIA_BUILD_TESTS)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    enable_testing()
    add_executable(tsmedia_tests
        tests/tst_tsmedia.cpp
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
        src/uiutil.cpp
        src/settings.cpp # ts3::dataDir() is stubbed in the test
        src/serversettings.cpp # 2.2 per-server settings (settings.cpp reads the server groups through it)
    )
    target_include_directories(tsmedia_tests PRIVATE src third_party/ts3client-pluginsdk/include)
    target_compile_definitions(tsmedia_tests PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(tsmedia_tests PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(tsmedia_tests PRIVATE Qt5::Core Qt5::Gui Qt5::Test)
    get_target_property(_qmake Qt5::qmake IMPORTED_LOCATION)
    get_filename_component(_qtbin "${_qmake}" DIRECTORY)
    add_test(NAME tsmedia_tests COMMAND tsmedia_tests)
    # The Qt DLLs must be found when ctest runs the test (PATH is a ';' list: escape it for ENVIRONMENT).
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        set_tests_properties(tsmedia_tests PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin}")
    else()
        string(REPLACE ";" "\\;" _path "$ENV{PATH}")
        set_tests_properties(tsmedia_tests PROPERTIES ENVIRONMENT "PATH=${_qtbin}\\;${_path}")
    endif()

    # 2.2 drag-out and per-server settings: export names, server groups in settings.ini, data saver rules.
    add_executable(tsmedia_dragout_servers_tests
        tests/tst_dragout_servers.cpp
        src/filenames.cpp
        src/serversettings.cpp
        src/settings.cpp
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
    )
    target_include_directories(tsmedia_dragout_servers_tests PRIVATE src third_party/ts3client-pluginsdk/include)
    target_compile_definitions(tsmedia_dragout_servers_tests PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(tsmedia_dragout_servers_tests PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(tsmedia_dragout_servers_tests PRIVATE Qt5::Core Qt5::Gui Qt5::Test)
    add_test(NAME tsmedia_dragout_servers_tests COMMAND tsmedia_dragout_servers_tests)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        set_tests_properties(tsmedia_dragout_servers_tests PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin}")
    else()
        set_tests_properties(tsmedia_dragout_servers_tests PROPERTIES ENVIRONMENT "PATH=${_qtbin}\\;${_path}")
    endif()
endif()

# ---- developer tools -----------------------------------------------------------------------------
option(TSMEDIA_BUILD_TOOLS "Build developer tools (Media Foundation smoke test)" ON)
if(TSMEDIA_BUILD_TOOLS)
    # mfvideo_smoketest <video> <outdir>: probes a file, plays it headless and dumps frames as PNG.
    add_executable(mfvideo_smoketest
        tools/mfvideo_smoketest.cpp
        src/video/mfvideo.cpp
        src/i18n.cpp
    )
    target_include_directories(mfvideo_smoketest PRIVATE src)
    target_compile_definitions(mfvideo_smoketest PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(mfvideo_smoketest PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(mfvideo_smoketest PRIVATE Qt5::Core Qt5::Gui ${TSMEDIA_MF_LIBS})
    tsmedia_delay_load_media_foundation(mfvideo_smoketest) # exercises the same loading path as the plugin

    # render_gallery <outdir>: renders every preview/card/player state (light + dark) to PNG files.
    add_executable(render_gallery
        tools/render_gallery.cpp
        src/previewrenderer.cpp
        src/dragpixmap.cpp # 2.2 drag-out
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
        src/uiutil.cpp
    )
    target_include_directories(render_gallery PRIVATE src third_party/ts3client-pluginsdk/include)
    target_compile_definitions(render_gallery PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(render_gallery PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(render_gallery PRIVATE Qt5::Core Qt5::Gui)
endif()
