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

# 2.2 updater (docs/UPDATES.md). The install/rollback/manifest parts are always built (a DLL installed by
# an update must be able to settle or roll back its state). The network part and the checks are only
# built with TSMEDIA_UPDATER=ON, which scripts/build.ps1 sets for signed release builds: a copy you build
# yourself never replaces itself with the author's build, and never loads winhttp.dll.
option(TSMEDIA_UPDATER "Build the GitHub update check (official signed releases)" OFF)
set(TSMEDIA_UPDATE_SOURCES
    src/crypto.cpp                    src/crypto.h
    src/update/updatekeys.h
    src/update/updatemanifest.cpp     src/update/updatemanifest.h
    src/update/updatepolicy.cpp       src/update/updatepolicy.h
    src/update/pecheck.cpp            src/update/pecheck.h
    src/update/updatefiles.cpp        src/update/updatefiles.h
    src/update/updateinstaller.cpp    src/update/updateinstaller.h
)
target_sources(tsmedia PRIVATE ${TSMEDIA_UPDATE_SOURCES}
    src/update/updatesettings.cpp       src/update/updatesettings.h
    src/update/updater.cpp              src/update/updater.h
    src/update/updatedialog.cpp         src/update/updatedialog.h
    src/update/updatesettingsgroup.cpp  src/update/updatesettingsgroup.h
)
target_link_libraries(tsmedia PRIVATE bcrypt)
if(TSMEDIA_UPDATER)
    target_sources(tsmedia PRIVATE
        src/update/httpclient.cpp     src/update/httpclient.h
        src/update/updateworker.cpp   src/update/updateworker.h
    )
    target_compile_definitions(tsmedia PRIVATE TSMEDIA_UPDATER)
endif()

# The restart helper (tools/update_helper): Win32 only, static CRT, published as a signed release asset
# (TSMedia-<ver>-helper-<arch>.update), never embedded in the DLL.
add_executable(tsmedia_update_helper WIN32 tools/update_helper/main.cpp)
target_compile_definitions(tsmedia_update_helper PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(tsmedia_update_helper PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(tsmedia_update_helper PRIVATE /W4 /utf-8 /permissive-)
    # Load DLLs it depends on from System32 only (not from the user-writable folder it runs in).
    target_link_options(tsmedia_update_helper PRIVATE /DEPENDENTLOADFLAG:0x800)
endif()
target_link_libraries(tsmedia_update_helper PRIVATE bcrypt)

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

    # 2.2 updater tests: manifest, signatures, schedule, PE checks, installer (own executable).
    add_executable(tsmedia_update_tests tests/tst_updater.cpp ${TSMEDIA_UPDATE_SOURCES})
    target_include_directories(tsmedia_update_tests PRIVATE src)
    target_compile_definitions(tsmedia_update_tests PRIVATE ${TSMEDIA_COMMON_OPTIONS}
        TSMEDIA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/update")
    if(MSVC)
        target_compile_options(tsmedia_update_tests PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(tsmedia_update_tests PRIVATE Qt5::Core Qt5::Test bcrypt)
    add_test(NAME tsmedia_update_tests COMMAND tsmedia_update_tests)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        set_tests_properties(tsmedia_update_tests PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin}")
    else()
        set_tests_properties(tsmedia_update_tests PROPERTIES ENVIRONMENT "PATH=${_qtbin}\\;${_path}")
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
