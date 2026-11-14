cmake_minimum_required(VERSION 3.16)

project(tsmedia VERSION 2.6.0 LANGUAGES CXX RC)

set(PLUGIN_NAME        "TS Media chat")
set(PLUGIN_AUTHOR      "MehdiHttp")
# Shown in TeamSpeak's Addons list; scripts/build.ps1 reads it from here for the package's installer text.
set(PLUGIN_DESCRIPTION "Discord-style images, GIFs, videos, albums, voice messages, replies, reactions, link previews and HD emoji in the TeamSpeak chat. Drop or paste files into the chat; everyone with the plugin sees and plays them inline.")

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
                              src/loopplayers.h     # 2.5: the loop pool of GIF-like link videos
    src/chatintegration.cpp   src/chatintegration.h
    src/previewscale.h        src/chatscale.cpp     src/chatscale.h       # 2.5 zoom: the preview scale, one coordinator per chat
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
    src/composewidgets.cpp    src/composewidgets.h  # 2.3 compose-v2: the send window's own controls
    # 2.2 sha
    src/fileverify.cpp        src/fileverify.h
    src/uploadwait.cpp        src/uploadwait.h # 2.3.2: the upload race (messages wait for the server)
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
    # 2.3 text reactions: reactions on messages without media
    src/textreact.cpp         src/textreact.h
    src/textreactdoc.cpp      src/textreactdoc.h
    src/textreacthub.cpp      src/textreacthub.h
    src/chattextreactions.cpp src/chattextreactions.h
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
    # 2.3 live music: the bars stacked above the input, music into the voice (its paperclip entry: attachmenu)
    src/inputbars.cpp             src/inputbars.h
    src/video/mfmusicreader.cpp   src/video/mfmusicreader.h
    src/music/musicmix.cpp        src/music/musicmix.h
    src/music/musicengine.cpp     src/music/musicengine.h
    src/music/musicbar.cpp        src/music/musicbar.h
    src/music/musiccontroller.cpp src/music/musiccontroller.h
    src/servertabs.cpp            src/servertabs.h # its server tab brought to the front (TeamSpeak's tab bar)
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
    # 2.5 GIFs: the emoji picker's GIFs tab (the list, the rules, the library, the tab's widget)
    src/gifstore.cpp          src/gifstore.h
    src/gifrules.cpp          src/gifrules.h
    src/giflibrary.cpp        src/giflibrary.h
    src/gifpanel.cpp          src/gifpanel.h        src/pickerstyle.h
    # chat redesign: the Cozy / Compact chat layout, the action bar, TeamSpeak's emoji button taken over
                              src/layoutformat.h
    src/layoutdoc.cpp         src/layoutdoc.h
    src/layoutart.cpp         src/layoutart.h
    src/chatlayout.cpp        src/chatlayout.h      src/chatscroll.h
    src/actionbar.cpp         src/actionbar.h
    src/chatlayoutsection.cpp src/chatlayoutsection.h
    # 2.3 attach: the paperclip in the chat input and its menu
    src/attachmenu.cpp        src/attachmenu.h
    src/attachart.cpp         src/attachart.h
    src/attachpopup.cpp       src/attachpopup.h
    src/attachbutton.cpp      src/attachbutton.h
    # 2.3 poll: anonymous polls (format and store, votes over plugin commands, the card, the window, the chat)
    src/polls.cpp             src/polls.h
    src/pollhub.cpp           src/pollhub.h
    src/polldoc.cpp           src/polldoc.h
    src/pollart.cpp           src/pollart.h
    src/polldialog.cpp        src/polldialog.h
    src/chatpolls.cpp         src/chatpolls.h
    # 2.3 chat-visuals: message bubbles, centred system rows, avatars from TeamSpeak
    src/layoutbubbles.cpp     src/layoutbubbles.h
    src/chatavatars.cpp       src/chatavatars.h
    # 2.4 link previews: the LP announcement and its store, the sender pipeline, the hub (sender-made previews);
    # the cards under web links, their document layer and states (chat). linkurl.* and fetcher.* come with the
    # net-core sources below.
    src/linkpreview/announcement.cpp  src/linkpreview/announcement.h
    src/linkpreview/previewstore.cpp  src/linkpreview/previewstore.h
    src/linkpreview/lpmessage.cpp     src/linkpreview/lpmessage.h
    src/linkpreview/previewmaker.cpp  src/linkpreview/previewmaker.h
    src/linkpreview/senderpipeline.cpp src/linkpreview/senderpipeline.h
    src/linkpreview/lphub.cpp         src/linkpreview/lphub.h
    src/linkpreview/linkdoc.cpp       src/linkpreview/linkdoc.h
    src/linkpreview/linkcard.cpp      src/linkpreview/linkcard.h
    src/linkpreview/linkstate.cpp     src/linkpreview/linkstate.h
    src/linkpreview/autoload.cpp      src/linkpreview/autoload.h
    src/chatlinkpreviews.cpp          src/chatlinkpreviews.h
    # end 2.4 link previews
    # 2.5 YouTube player: the page, the state machine, the helper's loader (the helper itself: tsmedia_player below)
    src/ytplayer/ytpage.cpp           src/ytplayer/ytpage.h
    src/ytplayer/ytplayer.cpp         src/ytplayer/ytplayer.h
    src/ytplayer/ytbackend.cpp        src/ytplayer/ytbackend.h
    player/tsmp.h                     player/tsmpmsg.h      player/tsmpnet.h # 2.6: the player's network rules
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc
)

# 2.6 chat proxy: the route snapshot, the saved state and its DPAPI password, the wire protocols, route health, the
# Windows settings, the loopback bridge and its upstream connections, the Test and Find jobs (ws2_32.dll, crypt32.dll
# and winhttp.dll loaded at run time, never linked). proxy::Control (control.cpp) is the plugin's and the tests' only.
set(TSMEDIA_PROXY_SOURCES
    src/proxy/failure.cpp       src/proxy/failure.h
    src/proxy/proxyconfig.cpp   src/proxy/proxyconfig.h
    src/proxy/protect.cpp       src/proxy/protect.h
    src/proxy/wire.cpp          src/proxy/wire.h
                                src/proxy/records.h
    src/proxy/router.cpp        src/proxy/router.h
    src/proxy/health.cpp        src/proxy/health.h
    src/proxy/windowsproxy.cpp  src/proxy/windowsproxy.h
    src/proxy/linkparse.cpp     src/proxy/linkparse.h
    src/proxy/winsock.cpp       src/proxy/winsock.h
    src/proxy/upstream.cpp      src/proxy/upstream.h
    src/proxy/bridge.cpp        src/proxy/bridge.h
    src/proxy/testrun.cpp       src/proxy/testrun.h
    src/proxy/findrun.cpp       src/proxy/findrun.h
)

# 2.4 link previews (net-core): which links get a preview (linkurl), the network rules (netpolicy), what a
# website sent (content), the fetcher and its WinHTTP transport (winhttp.dll / ws2_32.dll loaded at run time).
set(TSMEDIA_LINKPREVIEW_NET_SOURCES
    src/linkpreview/linkurl.cpp           src/linkpreview/linkurl.h
    src/linkpreview/netpolicy.cpp         src/linkpreview/netpolicy.h
    src/linkpreview/content.cpp           src/linkpreview/content.h
    src/linkpreview/fetcher.cpp           src/linkpreview/fetcher.h
                                          src/linkpreview/transport.h
    src/linkpreview/winhttptransport.cpp
    # 2.5 link previews (engine): page links, site cards, web cards
    src/linkpreview/textclean.cpp         src/linkpreview/textclean.h
    src/linkpreview/providers.cpp         src/linkpreview/providers.h
    src/linkpreview/pagemeta.cpp          src/linkpreview/pagemeta.h
                                          src/linkpreview/carddata.h
    src/linkpreview/hostgate.cpp          src/linkpreview/hostgate.h
    src/linkpreview/shares.cpp            src/linkpreview/shares.h # 2.5 Nextcloud and ownCloud shares
    src/safeimage.cpp                     src/safeimage.h
    ${TSMEDIA_PROXY_SOURCES} # 2.6 chat proxy (the Fetcher decides its route from the snapshot)
)
# 2.6 chat proxy, the plugin's and the tests' only (Qt widgets, ts3api): the one owner (GUI thread), its texts, the menu,
# hotkey and command glue, the Chat proxy window and its Settings group, the label roles they share with Settings.
set(TSMEDIA_PROXY_UI_SOURCES
    src/proxy/control.cpp       src/proxy/control.h
    src/proxy/proxytexts.cpp    src/proxy/proxytexts.h
    src/proxy/proxycommands.cpp src/proxy/proxycommands.h
    src/proxy/proxywindow.cpp   src/proxy/proxywindow.h   src/proxy/proxyui.h
    src/proxy/proxygroup.cpp    src/proxy/proxygroup.h
    src/proxy/proxydiag.cpp     src/proxy/proxydiag.h     # 2.6 I3: the diagnostic info's section
    src/settingstheme.cpp       src/settingstheme.h
                                src/linkpreview/routereport.h
)
target_sources(tsmedia PRIVATE ${TSMEDIA_LINKPREVIEW_NET_SOURCES} ${TSMEDIA_PROXY_UI_SOURCES}
    src/update/sessionplan.cpp  src/update/sessionplan.h # 2.6 chat proxy: the updater's route (pure)
)
# end 2.4 link previews (net-core)

target_include_directories(tsmedia PRIVATE
    src
    player # 2.5 YouTube player: the helper's C API (no WebView2 header is ever included by the plugin)
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
    target_sources(tsmedia PRIVATE src/selftest.cpp src/selftest.h src/selftestecho.h) # the scripted live-test driver
    target_sources(tsmedia PRIVATE src/selftestmusic.cpp) # 2.3 live music: its live-test commands
endif()

# 2.2 updater (docs/UPDATES.md). The install/rollback/manifest parts are always built (a DLL installed by
# an update must be able to settle or roll back its state). The network part and the checks are only
# built with TSMEDIA_UPDATER=ON, which scripts/build.ps1 sets for release builds (-Release): a copy you
# build yourself never replaces itself with the author's build, and never contacts GitHub (link previews still load winhttp.dll).
option(TSMEDIA_UPDATER "Build the GitHub update check (official releases)" OFF)
set(TSMEDIA_UPDATE_SOURCES
    src/crypto.cpp                    src/crypto.h
    src/update/updatekeys.h
    src/update/updatemanifest.cpp     src/update/updatemanifest.h
    src/update/updatepolicy.cpp       src/update/updatepolicy.h
    src/update/pecheck.cpp            src/update/pecheck.h
    src/update/updatefiles.cpp        src/update/updatefiles.h
    src/update/updateinstaller.cpp    src/update/updateinstaller.h
    src/update/updatehint.cpp         src/update/updatehint.h # 2.3.4: the newer-version hint
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

# ---- 2.5 YouTube player: the helper DLL (design2.md 3.2, 3.13, 3.15) ------------------------------------------------
# tsmedia_player_<arch>.dll: pure Win32 + COM + the WebView2 SDK's static loader, static CRT, no Qt. The plugin DLL
# embeds it as RCDATA "TSMP" and extracts it only inside Play. Without the SDK (TSMEDIA_WEBVIEW2_DIR empty) there is no
# helper and no resource: Play opens the browser. 2.5.0 builds it for x64 only (TSMEDIA_PLAYER_WIN32 OFF).
set(TSMEDIA_WEBVIEW2_DIR "" CACHE PATH "The unpacked Microsoft.Web.WebView2 NuGet package (empty: no YouTube player)")
option(TSMEDIA_PLAYER_WIN32 "Build the YouTube player helper for 32-bit TeamSpeak too (off in 2.5.0)" OFF)
set(TSMEDIA_PLAYER OFF)
if(TSMEDIA_WEBVIEW2_DIR)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(TSMEDIA_PLAYER ON)
        set(_wv2arch x64)
    elseif(TSMEDIA_PLAYER_WIN32)
        set(TSMEDIA_PLAYER ON)
        set(_wv2arch x86)
    endif()
endif()
if(TSMEDIA_PLAYER)
    set(_wv2inc "${TSMEDIA_WEBVIEW2_DIR}/build/native/include")
    set(_wv2lib "${TSMEDIA_WEBVIEW2_DIR}/build/native/${_wv2arch}/WebView2LoaderStatic.lib")
    if(NOT EXISTS "${_wv2inc}/WebView2.h" OR NOT EXISTS "${_wv2lib}")
        message(FATAL_ERROR "TSMEDIA_WEBVIEW2_DIR (${TSMEDIA_WEBVIEW2_DIR}) has no build/native/include/WebView2.h or ${_wv2arch}/WebView2LoaderStatic.lib")
    endif()
    add_library(tsmedia_player SHARED player/tsmp.cpp player/tsmp.h player/tsmpmsg.h player/tsmpnet.h)
    set_target_properties(tsmedia_player PROPERTIES OUTPUT_NAME "tsmedia_player_${TSMEDIA_ARCH}" PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_include_directories(tsmedia_player PRIVATE player "${_wv2inc}")
    target_compile_definitions(tsmedia_player PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(tsmedia_player PRIVATE /W4 /utf-8 /permissive-)
        # The DLLs it depends on come from System32 only.
        target_link_options(tsmedia_player PRIVATE /DEPENDENTLOADFLAG:0x800)
    endif()
    target_link_libraries(tsmedia_player PRIVATE "${_wv2lib}" ole32 oleaut32 shlwapi user32 gdi32 advapi32 version shell32 uuid)
    # Only system DLLs (its own allowlist): anything else fails the build.
    get_filename_component(_tsmedia_linkdir "${CMAKE_LINKER}" DIRECTORY)
    add_custom_command(TARGET tsmedia_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DDUMPBIN=${_tsmedia_linkdir}/dumpbin.exe" "-DDLL=$<TARGET_FILE:tsmedia_player>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_player_imports.cmake"
        VERBATIM)
    # The plugin DLL carries it (an in-app update replaces only the plugin DLL, and brings its helper with it).
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/player.rc"
        CONTENT "TSMP RCDATA \"$<TARGET_FILE:tsmedia_player>\"\n")
    target_sources(tsmedia PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/player.rc")
    add_dependencies(tsmedia tsmedia_player)
    target_compile_definitions(tsmedia PRIVATE TSMEDIA_PLAYER_RESOURCE)
endif()

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
        # 2.3 compose-v2: the send window itself with a fake host (Add files, keys, tiles, drops, colours)
        tests/tst_composedialog.cpp
        src/composedialog.cpp
        src/composewidgets.cpp
        src/imageeditor.cpp
        src/imageeditorcanvas.cpp
        # 2.2 sha
        tests/tst_fileverify.cpp
        src/fileverify.cpp src/fileverify.h
        # 2.3.2: the upload race: when a message may go, what a short or mismatching download does next
        tests/tst_uploadwait.cpp
        src/uploadwait.cpp src/uploadwait.h
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
        # 2.5 GIFs: the list and its rules, the library (records, copies, sends through the real Core), the tab
        tests/tst_gifs.cpp        tests/tst_gifpanel.cpp    tests/tst_gifsend.cpp
        src/gifstore.cpp
        src/gifrules.cpp
        src/giflibrary.cpp
        src/gifpanel.cpp          src/pickerstyle.h
        # 2.2.1: TeamSpeak's "Enter Chat Message..." placeholder: inserting into and reading the chat input
        tests/tst_chatinput.cpp
        src/chatinput.cpp
        tests/tst_replyemoji.cpp  # 2.2 integration: replies and HD emoji in the same chat
        # chat redesign: the chat layout on TeamSpeak-like documents (tests/tschat.h)
        tests/tst_chatlayout.cpp  tests/tschat.h  src/chatscroll.h
        tests/tst_emojibutton.cpp # TeamSpeak's emoticon button taken over (a fake MainWindowChatWidget)
        src/layoutdoc.cpp
        src/layoutart.cpp
        # 2.3 chat-visuals: bubbles and pills on TeamSpeak-like documents, avatar lookups (fake TeamSpeak)
        tests/tst_chatvisuals.cpp
        src/layoutbubbles.cpp
        src/chatavatars.cpp
        src/chatlayoutsection.cpp src/emojisection.cpp src/settingssection.cpp # the General tab's access keys
        # 2.3 text reactions: message keys across clients, TR and TSYNC, the store, rows in the chat document
        tests/tst_textreact.cpp
        src/textreact.cpp
        src/textreactdoc.cpp
        tests/tst_chatdoc23.cpp   # 2.3 cross review: every layer in one chat (keys, jumbo, copying, replies, restore)
        tests/tst_ownright.cpp    # 2.3.1: your messages on the right, the chat's room at its end
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
        # 2.3 attach: the paperclip (EmojiInput places it) and its menu
        tests/tst_attach.cpp
        src/attachmenu.cpp
        src/attachart.cpp
        src/attachpopup.cpp
        src/attachbutton.cpp
        # 2.3 poll: the format, ballots and store, the hub with a fake transport, the chat document, the card, the window
        tests/tst_polls.cpp
        src/polls.cpp
        src/pollhub.cpp
        src/polldoc.cpp
        src/pollart.cpp
        src/polldialog.cpp
        # 2.3 live music: decode -> 48 kHz, the mixing math, the edited flags, the engine with generated
        # sound (nothing is played, no microphone), the bar's states, the stack above the input, the controller
        tests/tst_music.cpp       tests/musicfakes.h
        tests/tst_musicbar.cpp    # the bar, the stack above the input, the controller (fake TeamSpeak)
        tests/tst_holdtab.cpp     # the hold strip on another server tab (fixed in 2.3)
        src/inputbars.cpp
        src/video/mfmusicreader.cpp
        src/music/musicmix.cpp
        src/music/musicengine.cpp
        src/music/musicbar.cpp
        src/music/musiccontroller.cpp
        src/servertabs.cpp
        # 2.3 integration: the features together (bubbles with cards, polls and rows; poll reaction keys;
        # system rows; the bars above the input; the send window's presence line)
        tests/tst_wiring23.cpp
        tests/tst_unload23.cpp    # 2.3 cross review (lifetime): every layer given back, menus and music at unload
        # 2.4 link previews (sender-previews): LP grammar and hostile input, sender matching, the store, the
        # sender pipeline end to end through the real Core against the fake TeamSpeak (a fake fetcher)
        tests/tst_linkpreview_snd.cpp
        src/linkpreview/announcement.cpp
        src/linkpreview/previewstore.cpp
        src/linkpreview/lpmessage.cpp
        src/linkpreview/previewmaker.cpp
        src/linkpreview/senderpipeline.cpp
        src/linkpreview/lphub.cpp
        # 2.4 link previews (chat): links found in TeamSpeak-like chats, the slots given back exactly, the cards'
        # geometry and hit tests, what a slot shows, the made-up links, the settings
        tests/tst_linkui.cpp
        src/linkpreview/linkdoc.cpp
        src/linkpreview/linkcard.cpp
        src/linkpreview/linkstate.cpp
        src/linkpreview/autoload.cpp
        src/privacysection.cpp
        # 2.4 review (lifetime): copying, the reader's place and a long chat with the slots under links
        tests/tst_linklife.cpp
        # 2.5 link previews (integration): what TS Media 2.4.0 does with every command 2.5 sends (a frozen copy)
        tests/tst_compat24.cpp tests/compat24.cpp tests/compat24.h
        # 2.5 link previews (integration): the chat's rules per link class, busy sites, the Embed card
        tests/tst_linkui25.cpp
        # end 2.4 link previews
        # 2.5 zoom: the scale formula, Preview size, the coordinator (one pass, the reader's place), the picture budget,
        # the GIF frame rule, layouts in du at every scale
        tests/tst_previewscale.cpp src/previewscale.h src/chatscale.cpp src/chatscale.h
        # 2.5 YouTube player: the page, the state machine with a fake backend, the card states; the helper's extraction,
        # loading and pinning with stub helpers (never WebView2), the runtime check
        tests/tst_ytplayer.cpp tests/tst_playerload.cpp
        src/ytplayer/ytpage.cpp src/ytplayer/ytplayer.cpp src/ytplayer/ytbackend.cpp
    )
    # 2.4 link previews (net-core): the classifier table, address ranges, redirects, hostile oEmbed JSON, caps,
    # cancel and shutdown, all through a scripted transport (never the internet)
    target_sources(tsmedia_tests PRIVATE tests/tst_linkpreview_net.cpp ${TSMEDIA_LINKPREVIEW_NET_SOURCES})
    # 2.5 link previews (engine): the text cleaner, the provider table, the page reader, HostGate, the page plans
    target_sources(tsmedia_tests PRIVATE tests/tst_textclean.cpp tests/tst_providers.cpp tests/tst_pagemeta.cpp tests/tst_hostgate.cpp
        tests/tst_linkpage_net.cpp tests/tst_safeimage.cpp tests/linkfixtures.cpp tests/linkfixtures.h
        tests/tst_shares.cpp) # 2.5 Nextcloud and ownCloud shares: classify, the page's markers, the decision, the card text
    # end 2.4 link previews (net-core)
    target_include_directories(tsmedia_tests PRIVATE src tests player third_party/ts3client-pluginsdk/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated) # 2.2 servergroup: groupicon_data.h
    target_compile_definitions(tsmedia_tests PRIVATE ${TSMEDIA_COMMON_OPTIONS}
        TSMEDIA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/update" # 2.2 updater: test manifests
        TSMEDIA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" # chat redesign: the sources (one header parser)
        # Qt 5.15's QVector / QList operator== uses stdext::checked_array_iterator, which MSVC 14.44
        # deprecates (STL4043, C4996 in tst_polls / tst_ownright): Qt's code, not ours.
        _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
    if(MSVC)
        target_compile_options(tsmedia_tests PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    # 2.6 chat proxy: the proxy core and the bridge against in-test fake proxies (ws2_32 only for the tests' own sockets)
    target_sources(tsmedia_tests PRIVATE tests/tst_proxycore.cpp tests/tst_proxybridge.cpp tests/proxyfakes.cpp tests/proxyfakes.h
        ${TSMEDIA_PROXY_UI_SOURCES} src/update/sessionplan.cpp src/update/sessionplan.h
        tests/tst_proxyui.cpp # 2.6 I2b: the Chat proxy window, the Settings group, the menu / hotkey / command, the notices
        tests/tst_proxydiag.cpp src/selftestecho.h) # 2.6 I3: the diagnostic info's section, no secrets in it or the logs
    target_link_libraries(tsmedia_tests PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Test bcrypt ws2_32 ${TSMEDIA_MF_LIBS})
    # 2.5 YouTube player: stand-ins for the helper (tst_playerload): the C API without WebView2, and one with another
    # API version. With the SDK, the real helper is loaded too (only tsmp_apiVersion, tsmp_init and tsmp_shutdown).
    foreach(_stub IN ITEMS tsmedia_player_stub tsmedia_player_stub_v99)
        add_library(${_stub} SHARED player/tsmp_stub.cpp player/tsmp.h player/tsmpnet.h)
        set_target_properties(${_stub} PROPERTIES PREFIX "" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_include_directories(${_stub} PRIVATE player)
        target_compile_definitions(${_stub} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(${_stub} PRIVATE /W4 /utf-8 /permissive-)
        endif()
        add_dependencies(tsmedia_tests ${_stub})
    endforeach()
    target_compile_definitions(tsmedia_player_stub_v99 PRIVATE TSMP_STUB_API_VERSION=99)
    target_compile_definitions(tsmedia_tests PRIVATE TSMP_STUB_DLL="$<TARGET_FILE:tsmedia_player_stub>"
        TSMP_STUB_V99_DLL="$<TARGET_FILE:tsmedia_player_stub_v99>")
    if(TSMEDIA_PLAYER)
        target_compile_definitions(tsmedia_tests PRIVATE TSMP_REAL_DLL="$<TARGET_FILE:tsmedia_player>")
        add_dependencies(tsmedia_tests tsmedia_player)
    endif()
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
        # 2.4 link previews: every card state, and the cards in chats (Classic, Cozy with bubbles, Compact)
        src/layoutbubbles.cpp
        src/linkpreview/linkurl.cpp src/linkpreview/linkdoc.cpp src/linkpreview/linkcard.cpp
        src/linkpreview/textclean.cpp src/linkpreview/providers.cpp src/linkpreview/netpolicy.cpp # 2.5
        src/linkpreview/shares.cpp src/linkpreview/pagemeta.cpp # 2.5 the share rows (providers), their card text
        # end 2.4 link previews
        src/pollart.cpp # 2.5 zoom: poll cards at every scale
        src/ytplayer/ytpage.cpp # 2.5 YouTube player: the card's states (Consent, Starting, Popup, Hidden, notes)
    )
    target_include_directories(render_gallery PRIVATE src player tests third_party/ts3client-pluginsdk/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(render_gallery PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(render_gallery PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(render_gallery PRIVATE Qt5::Core Qt5::Gui)

    # 2.4 link previews (net-core): linkfetch_smoke <url> <outdir> [--max-bytes N] [--cancel-after ms] fetches one
    # link through the real path (WinHTTP, every network rule) for the live test. Never run by the unit tests.
    add_executable(linkfetch_smoke
        tools/linkfetch_smoke.cpp
        ${TSMEDIA_LINKPREVIEW_NET_SOURCES}
        src/mediaprobe.cpp
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
        src/video/mfvideo.cpp
        src/video/mfcommon.cpp
    )
    target_include_directories(linkfetch_smoke PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(linkfetch_smoke PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(linkfetch_smoke PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(linkfetch_smoke PRIVATE Qt5::Core Qt5::Gui bcrypt ${TSMEDIA_MF_LIBS})
    tsmedia_delay_load_media_foundation(linkfetch_smoke)
    # end 2.4 link previews (net-core)

    # 2.6 chat proxy: proxy_smoke --socks5|--http host:port [--user u --password p] [--test] [--find] [--fetch URL]
    # [--verbose] runs a bridge to the given proxy and the Test, Find or one Fetcher load through it; prints the
    # classification and the bridge's record, never a secret. Never run by the unit tests.
    add_executable(proxy_smoke
        tools/proxy_smoke.cpp
        ${TSMEDIA_LINKPREVIEW_NET_SOURCES}
        src/mediaprobe.cpp
        src/medialink.cpp
        src/blurhash.cpp
        src/i18n.cpp
        src/video/mfvideo.cpp
        src/video/mfcommon.cpp
    )
    target_include_directories(proxy_smoke PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(proxy_smoke PRIVATE ${TSMEDIA_COMMON_OPTIONS})
    if(MSVC)
        target_compile_options(proxy_smoke PRIVATE /W4 /utf-8 /permissive- /wd4100)
    endif()
    target_link_libraries(proxy_smoke PRIVATE Qt5::Core Qt5::Gui bcrypt ${TSMEDIA_MF_LIBS})
    tsmedia_delay_load_media_foundation(proxy_smoke)

    # 2.5 YouTube player: player_smoke [--youtube <id>] loads the helper like the plugin does and drives it (a fake IFrame
    # API from memory, the Referer recorded, commands, clipping, owner minimize/restore/destroy, Esc, release). Muted.
    # Never run by the unit tests, never during a live test (it is CPU-heavy for a few seconds).
    # 2.6 chat proxy: --youtube <id> --bridge socks5|http host:port plays through the plugin's own bridge (the F3 check of
    # design 3.6.5), so the bridge's sources (and the link-preview rules it applies) are linked in too.
    if(TSMEDIA_PLAYER)
        add_executable(player_smoke tools/player_smoke.cpp player/tsmp.h player/tsmpnet.h src/ytplayer/ytpage.cpp src/i18n.cpp
            ${TSMEDIA_LINKPREVIEW_NET_SOURCES} src/mediaprobe.cpp src/medialink.cpp src/blurhash.cpp src/video/mfvideo.cpp src/video/mfcommon.cpp)
        target_include_directories(player_smoke PRIVATE src player ${CMAKE_CURRENT_BINARY_DIR}/generated)
        target_compile_definitions(player_smoke PRIVATE ${TSMEDIA_COMMON_OPTIONS} TSMP_HELPER_FILE="tsmedia_player_${TSMEDIA_ARCH}.dll")
        if(MSVC)
            target_compile_options(player_smoke PRIVATE /W4 /utf-8 /permissive- /wd4100)
        endif()
        target_link_libraries(player_smoke PRIVATE Qt5::Core Qt5::Gui ole32 user32 gdi32 bcrypt ${TSMEDIA_MF_LIBS})
        tsmedia_delay_load_media_foundation(player_smoke)
        add_dependencies(player_smoke tsmedia_player)
    endif()

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
