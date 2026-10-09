#include "settings.h"

#include <QSettings>

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

QString normalizeUploadDirectory(QString dir)
{
    dir = dir.trimmed();
    dir.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!dir.startsWith(QLatin1Char('/')))
        dir.prepend(QLatin1Char('/'));
    while (dir.length() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    return dir;
}

} // namespace

Settings& Settings::instance()
{
    static Settings settings;
    return settings;
}

void Settings::load()
{
    const QSettings s(settingsFile(), QSettings::IniFormat);
    const Settings  d;

    // Receiving
    inlinePreviews      = readBool(s, "inlinePreviews", d.inlinePreviews);
    autoDownloadImages  = readBool(s, "autoDownloadImages", d.autoDownloadImages);
    autoDownloadMaxMB   = readInt(s, "autoDownloadMaxMB", d.autoDownloadMaxMB, autoDownloadMaxMBRange);
    autoplayGifs        = readBool(s, "autoplayGifs", d.autoplayGifs);
    videoAutoDownloadMB = readInt(s, "videoAutoDownloadMB", d.videoAutoDownloadMB, videoAutoDownloadMBRange);
    previewMaxWidth     = readInt(s, "previewMaxWidth", d.previewMaxWidth, previewMaxWidthRange);
    previewMaxHeight    = readInt(s, "previewMaxHeight", d.previewMaxHeight, previewMaxHeightRange);

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
    uploadDirectory = normalizeUploadDirectory(s.value(key("uploadDirectory"), d.uploadDirectory).toString());

    // General
    bool      langOk = false;
    const int lang   = s.value(key("language")).toInt(&langOk);
    language         = langOk && lang >= static_cast<int>(Language::Auto) && lang <= static_cast<int>(Language::Persian) ? static_cast<Language>(lang) : d.language;
    cacheLimitMB     = readInt(s, "cacheLimitMB", d.cacheLimitMB, cacheLimitMBRange);

    i18n::setLanguage(language);
}

void Settings::save() const
{
    // Keys via key() and strings via ownedCopy(): see the note at the top of this file.
    QSettings s(settingsFile(), QSettings::IniFormat);

    s.setValue(key("inlinePreviews"), inlinePreviews);
    s.setValue(key("autoDownloadImages"), autoDownloadImages);
    s.setValue(key("autoDownloadMaxMB"), autoDownloadMaxMB);
    s.setValue(key("autoplayGifs"), autoplayGifs);
    s.setValue(key("videoAutoDownloadMB"), videoAutoDownloadMB);
    s.setValue(key("previewMaxWidth"), previewMaxWidth);
    s.setValue(key("previewMaxHeight"), previewMaxHeight);

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

    s.setValue(key("language"), static_cast<int>(language));
    s.setValue(key("cacheLimitMB"), cacheLimitMB);
    s.sync();
}
