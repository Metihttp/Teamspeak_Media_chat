#pragma once

#include <QString>

struct Settings {
    // Valid values of the numeric settings, defined once: load() clamps to them and SettingsDialog
    // offers exactly these ranges.
    struct Range {
        int min;
        int max;
    };
    static constexpr Range autoDownloadMaxMBRange   = {1, 500};
    static constexpr Range videoAutoDownloadMBRange = {0, 4096};
    static constexpr Range previewMaxWidthRange     = {120, 1200};
    static constexpr Range previewMaxHeightRange    = {80, 1200};
    static constexpr Range videoVolumeRange         = {0, 100};
    static constexpr Range uploadMaxMBRange         = {1, 4096};
    static constexpr Range cacheLimitMBRange        = {100, 1024 * 1024};
    static constexpr int   maxDownloadUrlLength     = 512;

    // Receiving
    bool inlinePreviews      = true; // render images / players / file cards inside the TeamSpeak chat
    bool autoDownloadImages  = true; // fetch images automatically so they show without a click
    int  autoDownloadMaxMB   = 15;   // images and GIFs up to this size load automatically
    bool autoplayGifs        = true; // otherwise GIFs animate while hovered
    int  videoAutoDownloadMB = 0;    // videos up to this size download automatically; 0 = when you press play
    int  previewMaxWidth     = 400;
    int  previewMaxHeight    = 300;

    // Playback
    int  videoVolume      = 80; // percent
    bool videosStartMuted = false;
    bool loopVideos       = false;

    // Sending
    bool    interceptDragDrop     = true; // drop files on the chat to send them (hold Shift for TeamSpeak's default)
    bool    interceptPaste        = true; // Ctrl+V a screenshot in the chat line to send it
    bool    convertLargePngToJpeg = true;
    bool    generatePreviews      = true; // upload a small preview / poster with large images and videos
    bool    addRequiredNotice     = true; // tell people without the plugin that it is needed
    // Link behind "TS Media chat" in that notice. Empty in the ini means this default.
    static constexpr const char* defaultDownloadUrl = "https://github.com/Metihttp/Teamspeak_Media_chat";
    QString pluginDownloadUrl = QString::fromLatin1(defaultDownloadUrl);
    int     uploadMaxMB     = 100;
    // Not QStringLiteral: settings values must never share data with the DLL image (see settings.cpp).
    QString uploadDirectory = QString::fromLatin1("/tsmedia");

    // General
    int cacheLimitMB = 1024; // oldest cached media is deleted beyond this

    static Settings& instance();
    void             load();
    void             save() const;
};
