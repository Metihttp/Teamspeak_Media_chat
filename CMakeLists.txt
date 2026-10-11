cmake_minimum_required(VERSION 3.16)

project(tsmedia VERSION 2.2.1 LANGUAGES CXX RC)

set(PLUGIN_NAME        "TS Media chat")
set(PLUGIN_AUTHOR      "MehdiHttp")
# Shown in TeamSpeak's Addons list; scripts/build.ps1 reads it from here for the package's installer text.
set(PLUGIN_DESCRIPTION "Discord-style images, GIFs, videos, albums, voice messages, replies, reactions and HD emoji in the TeamSpeak chat. Drop or paste files into the chat; everyone with the plugin sees and plays them inline.")

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
    # 2.2 protocol: presence and reactions
    src/peerprotocol.cpp      src/peerprotocol.h
    src/pluginlink.cpp        src/pluginlink.h
    src/peers.cpp             src/peers.h
    src/reactions.cpp         src/reactions.h
    src/reactionart.cpp       src/reactionart.h
    src/reactionpicker.cpp    src/reactionpicker.h
    src/presenceline.cpp      src/presenceline.h
    src/privacysection.cpp    src/privacysection.h
    src/composehooks.cpp      src/composehooks.h
    src/peerhub.cpp           src/peerhub.h
    src/chatreactions.cpp     src/chatreactions.h
    # 2.4 compress: video compression before upload
    src/videocompress.cpp     src/videocompress.h
    src/video/mftranscode.cpp src/video/mftranscode.h
    src/compresssettings.cpp  src/compresssettings.h
    # 2.2 editor: crop & annotate (opened from the send window)
    src/imageeditmodel.cpp    src/imageeditmodel.h
    src/imageeditor.cpp       src/imageeditor.h
    src/imageeditorcanvas.cpp src/imageeditor_p.h
    # 2.2 voice
    src/audio/capture.h
    src/audio/waveform.cpp      src/audio/waveform.h
    src/audio/capturedevice.cpp src/audio/capturedevice.h
    src/audio/wasapicapture.cpp src/audio/wasapicapture.h
    src/audio/fakecapture.cpp   src/audio/fakecapture.h
    src/audio/aacwriter.cpp     src/audio/aacwriter.h
    src/audio/voicerecorder.cpp src/audio/voicerecorder.h
    src/audio/micguard.cpp      src/audio/micguard.h
    src/audio/cues.cpp          src/audio/cues.h
    src/voicecard.cpp           src/voicecard.h
    src/voicepanel.cpp          src/voicepanel.h
    src/voicecontroller.cpp     src/voicecontroller.h
    src/micbutton.cpp           src/micbutton.h # 2.2.1 mic: the chat input's microphone button
    src/holdstrip.cpp           src/holdstrip.h # 2.2.1 hold to record: the strip above the chat input
    # 2.2 reply
    src/replies.cpp           src/replies.h
    src/replydoc.cpp          src/replydoc.h
    src/replyart.cpp          src/replyart.h
    src/replybar.cpp          src/replybar.h
    src/repliespopup.cpp      src/repliespopup.h
    src/chatreplies.cpp       src/chatreplies.h
    # 2.2 emoji: the table, the colour renderer, the picker, HD emoji in the chat
    src/emojidata.cpp         src/emojidata.h       src/emojitable.inc
    src/emojisegment.cpp      src/emojisegment.h
    src/emojirender.cpp       src/emojirender.h     src/emojicache.h
    src/emojiprefs.cpp        src/emojiprefs.h
    src/emojipicker.cpp       src/emojipicker.h
    src/emojiinput.cpp        src/emojiinput.h
    src/chatinput.cpp         src/chatinput.h       # 2.2.1: TeamSpeak's placeholder in the chat input
    src/chatemoji.cpp         src/chatemoji.h       src/emojiformat.h
    src/emojitext.cpp         src/emojitext.h
    src/emojisection.cpp      src/emojisection.h
    # chat redesign: the Cozy / Compact chat layout, the action bar, TeamSpeak's emoji button taken over
                              src/layoutformat.h
    src/layoutdoc.cpp         src/layoutdoc.h
    src/layoutart.cpp         src/layoutart.h
    src/chatlayout.cpp        src/chatlayout.h      src/chatscroll.h
    src/actionbar.cpp         src/actionbar.h
    src/chatlayoutsection.cpp src/chatlayoutsection.h
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
    target_sources(tsmedia PRIVATE src/selftest.cpp src/selftest.h) # the scripted live-test driver
endif()

# 2.2 updater (docs/UPDATES.md). The install/rollback/manifest parts are always built (a DLL installed by
# an update must be able to settle or roll back its state). The network part and the checks are only
# built with TSMEDIA_UPDATER=ON, which scripts/build.ps1 sets for release builds (-Release): a copy you
# build yourself never replaces itself with the author's build, and never loads winhttp.dll.
option(TSMEDIA_UPDATER "Build the GitHub update check (official releases)" OFF)
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

# The restart helper (tools/update_helper): Win32 only, static CRT, published as a release asset
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
if(TSMEDIA_TESTHOOKS)
    target_compile_definitions(tsmedia_update_helper PRIVATE TSMEDIA_TESTHOOKS) # the local update test's relaunch link
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
    # One executable: tests/testmain.h runs every test class (TestTsMedia first, then the classes the
    # other files register with TSMEDIA_REGISTER_TEST). "-o file,format" writes all of them to one file.
    add_executable(tsmedia_tests
        tests/testmain.h
        tests/tst_tsmedia.cpp     # TeamSpeak's config folder is a temporary one for every test file
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
        # 2.2 protocol: presence and reactions
        tests/tst_protocol.cpp
        src/peerprotocol.cpp
        src/peers.cpp
        src/pluginlink.cpp
        src/reactions.cpp
        src/reactionart.cpp
        # 2.2 reply: the format, matching, TeamSpeak-like chat documents, the reply line's size
        tests/tst_replies.cpp
        src/replies.cpp
        src/replydoc.cpp
        src/replyart.cpp
        # 2.2 emoji: table, segmentation, picture cache, renderer
        tests/tst_emoji.cpp
        tests/tst_emojireact.cpp  # any-emoji reactions, preferences, the chat document
        tests/tst_emojireview.cpp # review regressions: TeamSpeak-like chat widgets, picker keys, worker queue
        src/emojidata.cpp
        src/emojisegment.cpp
        src/emojirender.cpp
        src/emojiprefs.cpp
        src/chatemoji.cpp         # the document part (QTextDocument only in the tests)
        src/emojiinput.cpp
        src/micbutton.cpp         # 2.2.1 mic: in the input's right-edge slot (EmojiInput places it)
        src/emojipicker.cpp
        src/emojitext.cpp         # HD emoji in the reply line, bar and popup
        # 2.2.1: TeamSpeak's "Enter Chat Message..." placeholder: inserting into and reading the chat input
        tests/tst_chatinput.cpp
        src/chatinput.cpp
        tests/tst_replyemoji.cpp  # 2.2 integration: replies and HD emoji in the same chat
        # chat redesign: the chat layout on TeamSpeak-like documents (tests/tschat.h)
        tests/tst_chatlayout.cpp  tests/tschat.h  src/chatscroll.h
        tests/tst_emojibutton.cpp # TeamSpeak's emoticon button taken over (a fake MainWindowChatWidget)
        src/layoutdoc.cpp
        src/layoutart.cpp
        # 2.2 sha: Core itself (the real core.cpp and ts3api.cpp) against a fake TeamSpeak (tests/fakets3.*):
        # downloads checked against their sha, mismatches blocked, made-up hashes refused, sent links hashed
        tests/tst_coresha.cpp
        # 2.2 sending through Core: pacing of folder and upload requests, post order, retries, captions
        tests/tst_coresend.cpp
        # 2.2 wave 2 through Core: compressed videos (sha of the MP4, albums, Retry), voice messages, own files
        tests/tst_coremedia.cpp
        tests/fakets3.cpp         tests/fakets3.h
        src/core.cpp
        src/ts3api.cpp
        src/mediaprobe.cpp
        src/video/mfvideo.cpp
        src/video/mfcommon.cpp
        # 2.4 compress: the compression planner (and the transcoder, which the real core.cpp calls)
        tests/tst_videocompress.cpp
        src/videocompress.cpp
        src/video/mftranscode.cpp
        # 2.2 editor
        tests/tst_imageedit.cpp
        src/imageeditmodel.cpp
        # 2.2 voice: waveform, MicGuard (fake TeamSpeak), microphone mapping, cues
        tests/tst_voice.cpp
        src/audio/waveform.cpp
        src/audio/micguard.cpp
        src/audio/capturedevice.cpp
        src/audio/cues.cpp
        # 2.2 voice: the encoder (generated sound -> .m4a -> probe), the recorder with FakeCapture (never a
        # real microphone) and the voice card (audiocard.cpp hands voice messages over to it)
        tests/tst_voicemedia.cpp
        src/audio/aacwriter.cpp
        src/audio/voicerecorder.cpp
        src/audio/fakecapture.cpp
        src/audio/wasapicapture.cpp # only lists endpoints in the tests; never opens one
        src/voicecard.cpp
        # 2.2.1 voice: the recorder's flow (send at once, cancel, too short, 5:00) with a fake chat and
        # generated sound, its window, and the chat input's mic button
        tests/tst_voiceflow.cpp   tests/voicefakes.h
        tests/tst_voicehold.cpp   # 2.2.1 hold to record: press, release, tap, cancel zone, Esc, 5:00, lost grab, unload
        src/voicecontroller.cpp
        src/voicepanel.cpp
        src/holdstrip.cpp         # 2.2.1 hold to record: the strip above the chat input
    )
    target_include_directories(tsmedia_tests PRIVATE src tests third_party/ts3client-pluginsdk/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated) # 2.2 servergroup: groupicon_data.h
    target_compile_definitions(tsmedia_tests PRIVATE ${TSMEDIA_COMMON_OPTIONS}
        TSMEDIA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/update" # 2.2 updater: test manifests
        TSMEDIA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}") # chat redesign: the sources (one header parser)
    if(MSVC)
        target_compile_options(tsmedia_tests PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(tsmedia_tests PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Test bcrypt ${TSMEDIA_MF_LIBS})
    tsmedia_delay_load_media_foundation(tsmedia_tests) # Core probes media like the plugin does
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

    # 2.4 compress: mftranscode_smoketest <video> <outdir> [options]: plans and transcodes a video like the
    # plugin does and prints the encoder, realtime factor, CPU time, peak memory and size vs. estimate.
    add_executable(mftranscode_smoketest
        tools/mftranscode_smoketest.cpp
        src/video/mftranscode.cpp
        src/video/mfvideo.cpp
        src/video/mfcommon.cpp
        src/videocompress.cpp
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
    )
    target_include_directories(mftranscode_smoketest PRIVATE src)
    target_compile_definitions(mftranscode_smoketest PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(mftranscode_smoketest PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(mftranscode_smoketest PRIVATE Qt5::Core Qt5::Gui ${TSMEDIA_MF_LIBS})
    tsmedia_delay_load_media_foundation(mftranscode_smoketest)

    # render_gallery <outdir>: renders every preview/card/player state (light + dark) to PNG files.
    add_executable(render_gallery
        tools/render_gallery.cpp
        src/previewrenderer.cpp
        src/audiocard.cpp # 2.2 audio
        src/dragpixmap.cpp # 2.2 drag-out
        src/voicecard.cpp src/audio/waveform.cpp # 2.2 voice
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
        src/uiutil.cpp
        src/spoiler.cpp # 2.2 spoiler
        src/albums.cpp # 2.2 album
        # 2.2 protocol: reaction rows
        src/reactionart.cpp
        src/reactions.cpp
        src/peerprotocol.cpp
        src/replyart.cpp # 2.2 reply: reply lines
        # 2.2 emoji: HD reaction pictures, HD emoji in reply lines
        src/emojirender.cpp
        src/emojidata.cpp
        src/emojisegment.cpp
        src/emojitext.cpp
        src/layoutdoc.cpp src/layoutart.cpp # chat redesign: the chat layout pages
    )
    target_include_directories(render_gallery PRIVATE src tests third_party/ts3client-pluginsdk/include)
    target_compile_definitions(render_gallery PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(render_gallery PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(render_gallery PRIVATE Qt5::Core Qt5::Gui)

    # 2.2 emoji: emoji_probe <candidates> <probe> asks DirectWrite which emoji Segoe UI Emoji draws in colour
    # (tools/emoji/gen_emoji.py builds src/emojitable.inc from the answer); --sheet draws them all.
    add_executable(emoji_probe
        tools/emoji_probe.cpp
        src/emojirender.cpp src/emojidata.cpp src/i18n.cpp
    )
    target_include_directories(emoji_probe PRIVATE src)
    target_compile_definitions(emoji_probe PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(emoji_probe PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(emoji_probe PRIVATE Qt5::Core Qt5::Gui)
endif()
