cmake_minimum_required(VERSION 3.16)

project(tsmedia VERSION 2.2.0 LANGUAGES CXX RC)

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

# 2.2 updater tests: a test build can report another version, to play the "next release" on the local
# test server (docs/UPDATES.md). Refused without TSMEDIA_TESTHOOKS.
set(TSMEDIA_VERSION_OVERRIDE "" CACHE STRING "Test builds only: the version to build instead of the project version")
if(TSMEDIA_VERSION_OVERRIDE)
    if(NOT TSMEDIA_TESTHOOKS)
        message(FATAL_ERROR "TSMEDIA_VERSION_OVERRIDE needs -DTSMEDIA_TESTHOOKS=ON")
    endif()
    if(NOT TSMEDIA_VERSION_OVERRIDE MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR "TSMEDIA_VERSION_OVERRIDE must look like 2.1.9")
    endif()
    set(PROJECT_VERSION ${TSMEDIA_VERSION_OVERRIDE})
    set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
    set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
endif()

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)
configure_file(src/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc @ONLY)

# 2.2 servergroup: the "tsmediachat" group icon as byte arrays. Its id on the server is the CRC-32 of
# these exact bytes (2038059696), so the PNGs in assets/ must never be re-encoded.
include(cmake/embed_files.cmake)
tsmedia_embed_files(${CMAKE_CURRENT_BINARY_DIR}/generated/groupicon_data.h
    tsmediachat_group_16 assets/tsmediachat_group_16.png
    tsmediachat_group_32 assets/tsmediachat_group_32.png)

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
                              src/ownedtimer.h
    src/core.cpp              src/core.h
    src/previewrenderer.cpp   src/previewrenderer.h
    src/previewpaint.h        src/audiocard.cpp     src/audiocard.h       # 2.2 audio
    src/audioplayback.cpp     src/audioplayback.h                         # 2.2 audio
    src/inlinemedia.cpp       src/inlinemedia.h
    src/chatintegration.cpp   src/chatintegration.h
    src/uploadtoast.cpp       src/uploadtoast.h
    src/mediaviewer.cpp       src/mediaviewer.h
    src/settingsdialog.cpp    src/settingsdialog.h
    src/video/mfvideo.cpp     src/video/mfvideo.h
    # 2.2 foundation
    src/logtext.cpp           src/logtext.h
    src/pluginlog.cpp         src/pluginlog.h
    src/hashing.cpp           src/hashing.h
    src/floodgovernor.cpp     src/floodgovernor.h
    src/settingssection.cpp   src/settingssection.h
    # 2.2 servergroup: Settings > Server access, the Give / Remove access menu items
    src/accessgroupplan.cpp   src/accessgroupplan.h
    src/accessgroupjob.cpp    src/accessgroupjob.h
    src/accessgroupview.cpp   src/accessgroupview.h
    src/accessgroup.cpp       src/accessgroup.h
    src/accessgroupbox.cpp    src/accessgroupbox.h
    src/accessgroupexports.cpp
    src/video/mfcommon.cpp    src/video/mfcommon.h  # 2.2 audio: shared Media Foundation helpers
    # 2.2 drag-out and per-server settings
    src/filenames.cpp         src/filenames.h
    src/filedrag.cpp          src/filedrag.h
    src/dragpixmap.cpp        src/dragpixmap.h
    src/serversettings.cpp    src/serversettings.h
    src/serverssection.cpp    src/serverssection.h
    src/datasaver.cpp         src/datasaver.h
    # 2.2 diagnostics
    src/diagnostics.cpp         src/diagnostics.h
    src/diagnosticscollect.cpp  src/diagnosticscollect.h
    src/diagnosticsdialog.cpp   src/diagnosticsdialog.h
    # 2.2 compose: the send window
    src/composehooks.cpp      src/composehooks.h
    src/composemodel.cpp      src/composemodel.h
    src/composedialog.cpp     src/composedialog.h
    src/composesettings.cpp   src/composesettings.h
    # 2.2 sha
    src/fileverify.cpp        src/fileverify.h
    # 2.2 spoiler
    src/spoiler.cpp           src/spoiler.h
    src/spoilercover.cpp      src/spoilercover.h
    src/spoilersection.cpp    src/spoilersection.h
    src/chatintegration_spoiler.cpp
    # 2.2 album
    src/albums.cpp            src/albums.h
    src/chatalbums.cpp
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
    # One executable: tests/testmain.h runs every test class (TestTsMedia first, then the classes the
    # other files register with TSMEDIA_REGISTER_TEST). "-o file,format" writes all of them to one file.
    add_executable(tsmedia_tests
        tests/testmain.h
        tests/tst_tsmedia.cpp     # ts3::dataDir() is stubbed here for every test file
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
        src/uiutil.cpp
        src/settings.cpp
        src/serversettings.cpp    # 2.2 per-server settings (settings.cpp reads the server groups through it)
        # 2.2 foundation: link schema, composer, logging, hashing, flood governor
        tests/tst_link22.cpp
        tests/tst_foundation.cpp
        src/logtext.cpp
        src/pluginlog.cpp
        src/hashing.cpp
        src/floodgovernor.cpp
        # 2.2 servergroup: plan, job state machine, texts
        tests/tst_accessgroup.cpp
        src/accessgroupplan.cpp
        src/accessgroupjob.cpp
        src/accessgroupview.cpp
        # 2.2 updater: manifest, signatures, schedule, PE checks, installer
        tests/tst_updater.cpp
        ${TSMEDIA_UPDATE_SOURCES}
        src/update/updatesettings.cpp
        # 2.2 audio: the audio card's geometry / hit tests / states and the audio auto-download rule
        tests/tst_audio.cpp
        src/audiocard.cpp
        src/audioplayback.cpp
        src/previewrenderer.cpp
        # 2.2 drag-out and per-server settings: export names, server groups in settings.ini, data saver rules
        tests/tst_dragout_servers.cpp
        src/filenames.cpp
        # 2.2 diagnostics: the report, its redaction and the log parsers
        tests/tst_diagnostics.cpp
        src/diagnostics.cpp
        # 2.2 compose
        tests/tst_compose.cpp
        src/composemodel.cpp
        src/composehooks.cpp
        # 2.2 sha
        tests/tst_fileverify.cpp
        src/fileverify.cpp src/fileverify.h
        # 2.2 spoiler: reveal state, cover picture, crossfade (the drawing is checked by render_gallery)
        tests/tst_spoiler.cpp
        src/spoiler.cpp
        # 2.2 album
        tests/tst_albums.cpp
        src/albums.cpp
    )
    target_include_directories(tsmedia_tests PRIVATE src third_party/ts3client-pluginsdk/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated) # 2.2 servergroup: groupicon_data.h
    target_compile_definitions(tsmedia_tests PRIVATE ${TSMEDIA_COMMON_OPTIONS}
        TSMEDIA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/update") # 2.2 updater: signed test vectors
    if(MSVC)
        target_compile_options(tsmedia_tests PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(tsmedia_tests PRIVATE Qt5::Core Qt5::Gui Qt5::Test bcrypt)
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

    # 2.2 sha: Core itself against a fake TeamSpeak (tests/fakets3.*): downloads are checked against their
    # sha, mismatches blocked, made-up hashes refused, sent links hashed. Its own executable: it links the
    # real ts3api.cpp, which tsmedia_tests stubs.
    add_executable(tsmedia_core_tests
        tests/tst_coresha.cpp
        tests/fakets3.cpp         tests/fakets3.h
        tests/testmain.h
        src/core.cpp              src/core.h
        src/fileverify.cpp        src/fileverify.h
        src/ts3api.cpp
        src/settings.cpp
        src/i18n.cpp
        src/medialink.cpp
        src/blurhash.cpp
        src/mediaprobe.cpp
        src/video/mfvideo.cpp
        src/video/mfcommon.cpp    # 2.2 audio
        src/audioplayback.cpp     # 2.2 audio: the audio auto-download rule
        src/filenames.cpp         # 2.2 drag-out
        src/serversettings.cpp    # 2.2 per-server settings
        src/spoiler.cpp           # 2.2 spoiler
        src/albums.cpp            # 2.2 album
        src/logtext.cpp
        src/pluginlog.cpp
        src/hashing.cpp
        src/floodgovernor.cpp
    )
    target_include_directories(tsmedia_core_tests PRIVATE src tests third_party/ts3client-pluginsdk/include)
    target_compile_definitions(tsmedia_core_tests PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(tsmedia_core_tests PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(tsmedia_core_tests PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Test ${TSMEDIA_MF_LIBS})
    tsmedia_delay_load_media_foundation(tsmedia_core_tests)
    add_test(NAME tsmedia_core_tests COMMAND tsmedia_core_tests)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        set_tests_properties(tsmedia_core_tests PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin}")
    else()
        set_tests_properties(tsmedia_core_tests PROPERTIES ENVIRONMENT "PATH=${_qtbin}\\;${_path}")
    endif()
endif()

# ---- developer tools -----------------------------------------------------------------------------
option(TSMEDIA_BUILD_TOOLS "Build developer tools (Media Foundation smoke test)" ON)
if(TSMEDIA_BUILD_TOOLS)
    # mfvideo_smoketest <video> <outdir>: probes a file, plays it headless and dumps frames as PNG.
    add_executable(mfvideo_smoketest
        tools/mfvideo_smoketest.cpp
        src/video/mfvideo.cpp
        src/video/mfcommon.cpp # 2.2 audio
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
        src/audiocard.cpp # 2.2 audio
        src/dragpixmap.cpp # 2.2 drag-out
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
        src/uiutil.cpp
        src/spoiler.cpp # 2.2 spoiler
        src/albums.cpp # 2.2 album
    )
    target_include_directories(render_gallery PRIVATE src third_party/ts3client-pluginsdk/include)
    target_compile_definitions(render_gallery PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(render_gallery PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(render_gallery PRIVATE Qt5::Core Qt5::Gui)
endif()
