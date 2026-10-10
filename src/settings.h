#pragma once

#include <QHash>
#include <QString>

#include "serversettings.h" // 2.2 per-server settings

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
    // 2.2 data saver: no automatic downloads of full files (previews still load); the limits above are kept.
    bool dataSaver = false;
    bool revealSpoilers      = false; // 2.2 spoiler: "Show spoilers without blurring" (no covers at all)

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
    // 2.2 compose: a drop opens the send window (Ctrl held: sends right away); false: the other way round.
    bool dropOpensSendWindow = true;
    bool sendAsAlbum         = true; // the send window's "Send as an album", as last chosen

    // Media cache
    int cacheLimitMB = 1024; // the least recently used media is deleted beyond this

    // 2.2 per-server settings: own values of single servers, by serversettings::serverKey(uid).
    // "Restore defaults" never clears them.
    QHash<QString, ServerOverrides> servers;

    // 2.2 protocol: Privacy (both global, never per server)
    bool showReactions = true; // reaction rows, the add button and "Add reaction"; off: none sent or shown
    bool sharePresence = true; // HELLO / HI to the channel and private-chat partners you send to

    // 2.2 emoji (global)
    bool hdEmoji     = true; // emoji (and TeamSpeak's emoticons) in the chat as HD pictures
    bool emojiButton = true; // the emoji button in the chat input (the picker's shortcut works either way)
    bool jumboEmoji  = true; // messages of only emoji (up to 27) show them large

    static Settings& instance();
    void             load();
    void             save() const;
    // 2.2 per-server settings: the same with another ini file (tests).
    void load(const QString& file);
    void save(const QString& file) const;

    // 2.2 per-server settings: these settings with the own values of the server with this unique
    // identifier applied (data saver, upload folder, upload size limit, the note). Unknown or empty
    // uid: the settings for all servers.
    Settings               forServer(const QString& serverUid) const;
    const ServerOverrides* overridesFor(const QString& serverUid) const; // nullptr: none

    // " a\b/ " -> "/a/b"; empty -> defaultUploadDirectory.
    static QString normalizeUploadDirectory(const QString& dir);

    // Why a link for the note can't be used (SettingsDialog shows a message for each).
    enum class DownloadUrlProblem { None, NotWebAddress, Scheme, Brackets, TooLong };
    // Checks a link typed for the note. "example.com/x" is taken as https. On None, *normalized is the
    // encoded link, safe inside [URL=...], or empty for empty input (which means the default link).
    static DownloadUrlProblem checkDownloadUrl(const QString& input, QString* normalized);
};
