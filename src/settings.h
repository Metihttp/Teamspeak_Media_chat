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
    bool    interceptPaste        = true; // Ctrl+V a screenshot or copied files in the chat input to send them
    bool    convertLargePngToJpeg = true;
    bool    generatePreviews      = true; // upload a small preview / poster with large images and videos
    bool    addRequiredNotice     = true; // tell people without the plugin that it is needed
    // Link behind "TS Media chat" in that notice. Empty in the ini means this default.
    static constexpr const char* defaultDownloadUrl = "https://github.com/Metihttp/Teamspeak_Media_chat";
    QString pluginDownloadUrl = QString::fromLatin1(defaultDownloadUrl);
    int     uploadMaxMB     = 100;
    // Folder in the channel's file browser. Empty means this default; "/" is the top level.
    static constexpr const char* defaultUploadDirectory = "/tsmedia";
    // Not QStringLiteral: settings values must never share data with the DLL image (see settings.cpp).
    QString uploadDirectory = QString::fromLatin1(defaultUploadDirectory);

    // Media cache
    int cacheLimitMB = 1024; // the least recently used media is deleted beyond this

    // 2.2 voice: voice messages
    static constexpr int maxVoiceMicrophoneLength = 512;
    QString voiceMicrophone;               // Windows endpoint id of the microphone; empty = the one TeamSpeak uses
    bool    voiceMuteTeamSpeakMic = true;  // mute the TeamSpeak microphone while recording (always given back)
    bool    voiceReview           = true;  // the hotkey's second press stops for a listen; off: it sends
    bool    voiceSounds           = true;  // a short sound when recording starts and stops
    // A stored microphone id as load() keeps it: printable ASCII up to maxVoiceMicrophoneLength, else empty.
    static QString validVoiceMicrophone(const QString& value);

    static Settings& instance();
    void             load();
    void             save() const;

    // " a\b/ " -> "/a/b"; empty -> defaultUploadDirectory.
    static QString normalizeUploadDirectory(const QString& dir);

    // Why a link for the note can't be used (SettingsDialog shows a message for each).
    enum class DownloadUrlProblem { None, NotWebAddress, Scheme, Brackets, TooLong };
    // Checks a link typed for the note. "example.com/x" is taken as https. On None, *normalized is the
    // encoded link, safe inside [URL=...], or empty for empty input (which means the default link).
    static DownloadUrlProblem checkDownloadUrl(const QString& input, QString* normalized);
};
