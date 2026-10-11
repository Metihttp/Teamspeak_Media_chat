#include "settings.h"

#include <QSettings>
#include <QUrl>

#include "ts3api.h"

namespace {

// Never hand QStringLiteral data to QSettings. Qt5Core keeps every QSettings file it has written in a
// process-wide cache (keys and values included) that is only destroyed at process exit, long after
// TeamSpeak has unloaded this DLL. QStringLiteral data lives in the DLL image, so a key or value backed
// by it dangles after FreeLibrary and ~QString crashes TeamSpeak on exit (or on the next load() after
// the plugin was re-enabled). Keys are therefore built with key() and strings copied with ownedCopy(),
// which both allocate through Qt5Core. The same rule applies wherever a plugin string reaches Qt or
// TeamSpeak state that can outlive ts3plugin_shutdown: the clipboard, QTextDocument resources and
// formats, TeamSpeak's own widgets, and style sheets (TeamSpeak's QStyleSheetStyle keeps the last
// style sheet text it parsed until it is destroyed at exit; see uploadtoast.cpp / mediaviewer.cpp).
QString key(const char* name)
{
    return QString::fromLatin1(name);
}

QString ownedCopy(const QString& text)
{
    return text.isNull() ? QString() : QString(text.constData(), text.size());
}

QString settingsFile()
{
    // QSettings also keeps the file name (as its cache key).
    return ts3::dataDir() + QLatin1String("/settings.ini");
}

bool readBool(const QSettings& s, const char* name, bool fallback)
{
    const QVariant v = s.value(key(name));
    return v.isValid() ? v.toBool() : fallback;
}

// Missing or garbage values fall back to the default; numbers are clamped to the setting's range.
int readInt(const QSettings& s, const char* name, int fallback, Settings::Range range)
{
    bool      ok    = false;
    const int value = s.value(key(name)).toInt(&ok);
    return qBound(range.min, ok ? value : fallback, range.max);
}

} // namespace

QString Settings::normalizeUploadDirectory(const QString& input)
{
    QString dir = input.trimmed();
    if (dir.isEmpty()) // as the dialog's placeholder says; "/" is the way to pick the top level
        return QString::fromLatin1(defaultUploadDirectory);
    dir.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!dir.startsWith(QLatin1Char('/')))
        dir.prepend(QLatin1Char('/'));
    while (dir.length() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    return dir;
}

int Settings::normalizeVideoQuality(int shortSide) // 2.4 compress
{
    return shortSide == 480 || shortSide == 1080 ? shortSide : 720;
}

Settings::DownloadUrlProblem Settings::checkDownloadUrl(const QString& input, QString* normalized)
{
    normalized->clear();
    QString text = input.trimmed();
    if (text.isEmpty())
        return DownloadUrlProblem::None;
    // Checked before parsing: QUrl rejects a bracket in the path as a whole, which would hide the reason.
    if (text.contains(QLatin1Char('[')) || text.contains(QLatin1Char(']')))
        return DownloadUrlProblem::Brackets;
    if (!text.contains(QLatin1String("://")))
        text.prepend(QLatin1String("https://"));
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid())
        return DownloadUrlProblem::NotWebAddress;
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return DownloadUrlProblem::Scheme;
    if (url.host().isEmpty())
        return DownloadUrlProblem::NotWebAddress;
    const QString encoded = QString::fromLatin1(url.toEncoded());
    if (encoded.length() > maxDownloadUrlLength)
        return DownloadUrlProblem::TooLong;
    *normalized = encoded;
    return DownloadUrlProblem::None;
}

Settings& Settings::instance()
{
    static Settings settings;
    return settings;
}

void Settings::load()
{
    load(settingsFile());
}

void Settings::load(const QString& file)
{
    const QSettings s(file, QSettings::IniFormat);
    const Settings  d;

    // Receiving
    inlinePreviews      = readBool(s, "inlinePreviews", d.inlinePreviews);
    autoDownloadImages  = readBool(s, "autoDownloadImages", d.autoDownloadImages);
    autoDownloadMaxMB   = readInt(s, "autoDownloadMaxMB", d.autoDownloadMaxMB, autoDownloadMaxMBRange);
    autoplayGifs        = readBool(s, "autoplayGifs", d.autoplayGifs);
    videoAutoDownloadMB = readInt(s, "videoAutoDownloadMB", d.videoAutoDownloadMB, videoAutoDownloadMBRange);
    previewMaxWidth     = readInt(s, "previewMaxWidth", d.previewMaxWidth, previewMaxWidthRange);
    previewMaxHeight    = readInt(s, "previewMaxHeight", d.previewMaxHeight, previewMaxHeightRange);
    dataSaver           = readBool(s, "dataSaver", d.dataSaver); // 2.2 data saver
    revealSpoilers      = readBool(s, "revealSpoilers", d.revealSpoilers); // 2.2 spoiler

    // Playback
    videoVolume      = readInt(s, "videoVolume", d.videoVolume, videoVolumeRange);
    videosStartMuted = readBool(s, "videosStartMuted", d.videosStartMuted);
    loopVideos       = readBool(s, "loopVideos", d.loopVideos);

    // Sending
    interceptDragDrop     = readBool(s, "interceptDragDrop", d.interceptDragDrop);
    interceptPaste        = readBool(s, "interceptPaste", d.interceptPaste);
    convertLargePngToJpeg = readBool(s, "convertLargePngToJpeg", d.convertLargePngToJpeg);
    generatePreviews      = readBool(s, "generatePreviews", d.generatePreviews);
    addRequiredNotice     = readBool(s, "addRequiredNotice", d.addRequiredNotice);
    pluginDownloadUrl     = s.value(key("pluginDownloadUrl"), d.pluginDownloadUrl).toString().trimmed();
    // Empty means "use the default"; a cut-off link would point somewhere else, so it is replaced too.
    if (pluginDownloadUrl.isEmpty() || pluginDownloadUrl.length() > maxDownloadUrlLength)
        pluginDownloadUrl = QString::fromLatin1(defaultDownloadUrl);
    uploadMaxMB     = readInt(s, "uploadMaxMB", d.uploadMaxMB, uploadMaxMBRange);
    // Empty means the default folder, like an empty field in the dialog ("/" is the top level).
    uploadDirectory = normalizeUploadDirectory(s.value(key("uploadDirectory"), d.uploadDirectory).toString());
    dropOpensSendWindow = readBool(s, "dropOpensSendWindow", d.dropOpensSendWindow); // 2.2 compose
    sendAsAlbum         = readBool(s, "sendAsAlbum", d.sendAsAlbum);
    // 2.4 compress
    compressVideos          = readBool(s, "compressVideos", d.compressVideos);
    compressVideosOverMB    = readInt(s, "compressVideosOverMB", d.compressVideosOverMB, compressVideosOverMBRange);
    compressVideoQuality    = normalizeVideoQuality(s.value(key("compressVideoQuality")).toInt());
    convertUnplayableVideos = readBool(s, "convertUnplayableVideos", d.convertUnplayableVideos);
    compressUseGpu          = readBool(s, "compressUseGpu", d.compressUseGpu);
    editorColor         = readInt(s, "editorColor", d.editorColor, editorColorRange); // 2.2 editor
    editorStroke        = readInt(s, "editorStroke", d.editorStroke, editorStrokeRange);
    editorHideMode      = readInt(s, "editorHideMode", d.editorHideMode, editorHideModeRange);

    // Media cache (a "language" key written by older versions is ignored)
    cacheLimitMB = readInt(s, "cacheLimitMB", d.cacheLimitMB, cacheLimitMBRange);

    // 2.2 per-server settings: the [server_<id>] groups, validated.
    servers = serversettings::readAll(s);

    // 2.2 protocol
    showReactions = readBool(s, "showReactions", d.showReactions);
    sharePresence = readBool(s, "sharePresence", d.sharePresence);
    // 2.2.1 voice: voiceMicrophone, voiceMuteTeamSpeakMic, voiceReview and voiceSounds (2.2.0) are ignored

    // 2.2 emoji
    hdEmoji     = readBool(s, "hdEmoji", d.hdEmoji);
    emojiButton = readBool(s, "emojiButton", d.emojiButton);
    jumboEmoji  = readBool(s, "jumboEmoji", d.jumboEmoji);

    // chat redesign
    chatLayout           = readInt(s, "chatLayout", d.chatLayout, chatLayoutRange);
    chatGroupMessages    = readBool(s, "chatGroupMessages", d.chatGroupMessages);
    chatHoverActions     = readBool(s, "chatHoverActions", d.chatHoverActions);
    chatAvatars          = readBool(s, "chatAvatars", d.chatAvatars);
    chatMentions         = readBool(s, "chatMentions", d.chatMentions);
    chatCollapseEvents   = readBool(s, "chatCollapseEvents", d.chatCollapseEvents);
    chatLayoutIntroShown = readBool(s, "chatLayoutIntroShown", d.chatLayoutIntroShown);
}

void Settings::save() const
{
    save(settingsFile());
}

void Settings::save(const QString& file) const
{
    // Keys via key() and strings via ownedCopy(): see the note at the top of this file.
    QSettings s(file, QSettings::IniFormat);

    s.setValue(key("inlinePreviews"), inlinePreviews);
    s.setValue(key("autoDownloadImages"), autoDownloadImages);
    s.setValue(key("autoDownloadMaxMB"), autoDownloadMaxMB);
    s.setValue(key("autoplayGifs"), autoplayGifs);
    s.setValue(key("videoAutoDownloadMB"), videoAutoDownloadMB);
    s.setValue(key("previewMaxWidth"), previewMaxWidth);
    s.setValue(key("previewMaxHeight"), previewMaxHeight);
    s.setValue(key("dataSaver"), dataSaver); // 2.2 data saver
    s.setValue(key("revealSpoilers"), revealSpoilers); // 2.2 spoiler

    s.setValue(key("videoVolume"), videoVolume);
    s.setValue(key("videosStartMuted"), videosStartMuted);
    s.setValue(key("loopVideos"), loopVideos);

    s.setValue(key("interceptDragDrop"), interceptDragDrop);
    s.setValue(key("interceptPaste"), interceptPaste);
    s.setValue(key("convertLargePngToJpeg"), convertLargePngToJpeg);
    s.setValue(key("generatePreviews"), generatePreviews);
    s.setValue(key("addRequiredNotice"), addRequiredNotice);
    s.setValue(key("pluginDownloadUrl"), ownedCopy(pluginDownloadUrl));
    s.setValue(key("uploadMaxMB"), uploadMaxMB);
    // QSettings' INI writer drops backslashes inside values, so store the normalised form.
    s.setValue(key("uploadDirectory"), ownedCopy(normalizeUploadDirectory(uploadDirectory)));
    s.setValue(key("dropOpensSendWindow"), dropOpensSendWindow); // 2.2 compose
    s.setValue(key("sendAsAlbum"), sendAsAlbum);
    s.setValue(key("compressVideos"), compressVideos); // 2.4 compress
    s.setValue(key("compressVideosOverMB"), compressVideosOverMB);
    s.setValue(key("compressVideoQuality"), normalizeVideoQuality(compressVideoQuality));
    s.setValue(key("convertUnplayableVideos"), convertUnplayableVideos);
    s.setValue(key("compressUseGpu"), compressUseGpu);
    s.setValue(key("editorColor"), editorColor); // 2.2 editor
    s.setValue(key("editorStroke"), editorStroke);
    s.setValue(key("editorHideMode"), editorHideMode);

    s.setValue(key("cacheLimitMB"), cacheLimitMB);

    // 2.2 per-server settings: servers without own values are not stored (forgotten ones are removed).
    serversettings::writeAll(s, servers);

    // 2.2 protocol
    s.setValue(key("showReactions"), showReactions);
    s.setValue(key("sharePresence"), sharePresence);

    // 2.2 emoji
    s.setValue(key("hdEmoji"), hdEmoji);
    s.setValue(key("emojiButton"), emojiButton);
    s.setValue(key("jumboEmoji"), jumboEmoji);
    // chat redesign
    s.setValue(key("chatLayout"), qBound(chatLayoutRange.min, chatLayout, chatLayoutRange.max));
    s.setValue(key("chatGroupMessages"), chatGroupMessages);
    s.setValue(key("chatHoverActions"), chatHoverActions);
    s.setValue(key("chatAvatars"), chatAvatars);
    s.setValue(key("chatMentions"), chatMentions);
    s.setValue(key("chatCollapseEvents"), chatCollapseEvents);
    s.setValue(key("chatLayoutIntroShown"), chatLayoutIntroShown);
    s.sync();
}

// 2.2 per-server settings
Settings Settings::forServer(const QString& serverUid) const
{
    Settings result = *this;
    if (const ServerOverrides* own = overridesFor(serverUid))
        serversettings::applyOverrides(result, *own);
    return result;
}

const ServerOverrides* Settings::overridesFor(const QString& serverUid) const
{
    if (serverUid.isEmpty() || servers.isEmpty())
        return nullptr;
    const auto it = servers.constFind(serversettings::serverKey(serverUid));
    return it == servers.constEnd() ? nullptr : &it.value();
}
