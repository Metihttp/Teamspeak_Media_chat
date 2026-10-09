#include "diagnostics.h"

#include <QDir>
#include <QLocale>
#include <QRegularExpression>

#include <atomic>
#include <iterator>

#include "i18n.h"
#include "medialink.h"

// Every text of the report is built at run time (i18n::t, QString::fromLatin1, arg()), never from
// QStringLiteral data: the report goes to the clipboard, which outlives the plugin DLL.

namespace diag {

namespace {

std::atomic<LogTailProvider> g_logTailProvider{nullptr};

QChar beginMark()
{
    return QChar(kMarkBegin);
}

QChar endMark()
{
    return QChar(kMarkEnd);
}

QString withoutMarkers(QString text)
{
    text.remove(beginMark());
    text.remove(endMark());
    return text;
}

// Text for one line of the report: no line breaks or other control and format characters (a name
// with a line break could fake report lines; bidi marks could reorder them).
QString plainLine(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        const QChar::Category category = c.category();
        if (c == QLatin1Char('\t') || c == QLatin1Char('\n') || c == QLatin1Char('\r'))
            out += QLatin1Char(' ');
        else if (category == QChar::Other_Control || category == QChar::Other_Format || category == QChar::Separator_Line
                 || category == QChar::Separator_Paragraph || c.unicode() == kMarkBegin || c.unicode() == kMarkEnd)
            continue;
        else
            out += c;
    }
    return out;
}

QString capLine(const QString& line, int maxLength)
{
    if (line.size() <= maxLength)
        return line;
    return line.left(maxLength - 1) + QChar(0x2026); // …
}

// Applies fn to the text outside marked values. Marked values and an unbalanced marker with the rest
// of the text after it are left as they are.
template <typename Fn>
QString mapUnmarked(const QString& text, Fn fn)
{
    QString out;
    int     pos = 0;
    while (pos < text.size()) {
        const int begin = text.indexOf(beginMark(), pos);
        if (begin < 0) {
            out += fn(text.mid(pos));
            break;
        }
        out += fn(text.mid(pos, begin - pos));
        const int end = text.indexOf(endMark(), begin + 1);
        if (end < 0) {
            out += text.mid(begin);
            break;
        }
        out += text.mid(begin, end + 1 - begin);
        pos = end + 1;
    }
    return out;
}

// Marks every match of re in plain text (no markers): the whole match, or with toEnd everything from
// the match to the end of the text (for paths, whose end can't be told).
QString markMatches(const QString& plain, const QRegularExpression& re, char kind, bool toEnd)
{
    QString                         out;
    int                             pos = 0;
    QRegularExpressionMatchIterator it  = re.globalMatch(plain);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() < pos)
            continue;
        out += plain.mid(pos, m.capturedStart() - pos);
        if (toEnd)
            return out + mark(kind, plain.mid(m.capturedStart()));
        out += mark(kind, m.captured());
        pos = m.capturedEnd();
    }
    return out + plain.mid(pos);
}

// “name” -> “<marked name>”; an opening quote without its closing one hides the rest.
QString markCurlyQuotes(const QString& plain)
{
    const QChar open(0x201C);
    const QChar close(0x201D);
    QString     out;
    int         pos = 0;
    while (pos < plain.size()) {
        const int start = plain.indexOf(open, pos);
        if (start < 0) {
            out += plain.mid(pos);
            break;
        }
        out += plain.mid(pos, start + 1 - pos);
        const int end = plain.indexOf(close, start + 1);
        if (end < 0) {
            // Nothing after it here: the quoted value is marked already (its closing quote follows it).
            if (start + 1 < plain.size())
                out += mark('n', plain.mid(start + 1));
            break;
        }
        if (end > start + 1)
            out += mark('n', plain.mid(start + 1, end - start - 1));
        out += close;
        pos = end + 1;
    }
    return out;
}

const QRegularExpression& localPathPattern()
{
    // C:\… C:/… \\server\share file:/// and %APPDATA%\…
    static const QRegularExpression re(QString::fromLatin1("(?<![A-Za-z0-9])[A-Za-z]:[\\\\/]|\\\\\\\\[^\\s\\\\]|file:/|%[A-Za-z_]+%[\\\\/]"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression& ipPattern()
{
    static const QRegularExpression re(QString::fromLatin1("(?<![\\w.])(?:\\d{1,3}\\.){3}\\d{1,3}(?::\\d{1,5})?(?![\\w.])"
                                                           "|(?<![\\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\\w:])"));
    return re;
}

const QRegularExpression& uniqueIdPattern()
{
    // Base64 identities as TeamSpeak shows them ("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys="): base64 characters,
    // at least one of + / in them or = at the end.
    static const QRegularExpression re(QString::fromLatin1("(?<![A-Za-z0-9+/=])[A-Za-z0-9+/]{6,}={1,2}(?![A-Za-z0-9+/=])"));
    return re;
}

const QRegularExpression& remotePathPattern()
{
    // A path in a channel's file browser: "/tsmedia/holiday.jpg" (not "1/2" or "and/or").
    static const QRegularExpression re(QString::fromLatin1("(?<![\\w./])/(?=[^\\s/])"));
    return re;
}

const QRegularExpression& fileNamePattern()
{
    // "holiday.jpg", "clip (1).mp4" is caught by its last part; the extension needs a letter, so
    // "2.2.0" and "4.5 MB" stay.
    static const QRegularExpression re(QString::fromLatin1("[^\\s/\\\\:*?\"<>|()\\[\\],;]+\\.[A-Za-z0-9]*[A-Za-z][A-Za-z0-9]{0,7}(?![\\w])"));
    return re;
}

// The plugin's own messages that contain names, by their wording (core.cpp, inlinemedia.cpp,
// plugin.cpp, ts3api.cpp printWarning). One kind letter per capture group: f file or remote path,
// l local path, n other name, - kept (the safety net still runs over it).
struct LegacyPattern {
    const char* regex;
    const char* kinds;
};

const LegacyPattern kLegacyPatterns[] = {
    {"^Discarding damaged cached copy of (.+); it is downloaded again$", "f"},
    {"^Automatic download of (.+) (discarded|stopped): the file is larger than the auto-download limit$", "f-"},
    {"^Downloaded (.+) \\(([^()]*)\\)$", "f-"},
    {"^(.+) was still open for writing 30 s after its transfer completed$", "f"},
    {"^Waited (\\d+) ms for TeamSpeak to finish writing (.+)$", "-f"},
    {"^Download of (.+) failed: (.*)$", "f-"},
    {"^Preview (.+) of (.+) not loaded: (.*)$", "ff-"},
    {"^Resuming the download of (.+)$", "f"},
    {"^Probed (.+): (-?\\d+x-?\\d+, -?\\d+ ms, blurhash \\w+, preview .+)$", "f-"},
    {"^Could not stage the preview of (.+); sending without it$", "f"},
    {"^Preview upload for (.+) (not started|failed) \\((.*)\\); sending without it$", "f--"},
    {"^The name of (.+) \\(or of its preview\\) is taken on the server; uploading as (.+)$", "ff"},
    {"^Uploaded (.+) \\(([^()]*)\\)$", "f-"},
    {"^Could not remove (.+) from the file browser: (.*)$", "f-"},
    {"^Inline video ([0-9a-f]+) cannot be played: (.*)$", "--"},
    {"^Not animating (.+): (\\d+x\\d+, \\d+ frames is too large)$", "f-"},
    {"^Couldn't find (\\d+) files: (.+)\\. They may have been moved or deleted\\.$", "-l"},
    {"^Couldn't find (.+)\\. It may have been moved or deleted\\.$", "l"},
    {"^Couldn't send \\x{201C}(.+)\\x{201D}: (.*)$", "f-"},
    {"^Couldn't save diagnostics to (.+)\\. Check that you can write to that folder\\.$", "l"},
    {"^Diagnostics saved to (.+)\\. Attach this file to your bug report\\.$", "l"},
    {"^Unknown command \\x{201C}(.*)\\x{201D}\\.$", "n"},
};

struct CompiledPattern {
    QRegularExpression re;
    QByteArray         kinds;
};

const QVector<CompiledPattern>& legacyPatterns()
{
    static const QVector<CompiledPattern> patterns = [] {
        QVector<CompiledPattern> list;
        for (const LegacyPattern& p : kLegacyPatterns)
            list.append({QRegularExpression(QString::fromUtf8(p.regex)), QByteArray(p.kinds)});
        return list;
    }();
    return patterns;
}

QString onOff(bool on)
{
    return on ? i18n::t("on") : i18n::t("off");
}

QString yesNo(bool yes)
{
    return yes ? i18n::t("yes") : i18n::t("no");
}

QString errorLabel(int error)
{
    switch (static_cast<MediaError>(error)) {
    case MediaError::Permission:
        return i18n::t("no permission");
    case MediaError::Password:
        return i18n::t("channel password");
    case MediaError::NotFound:
        return i18n::t("not found");
    case MediaError::NotConnected:
        return i18n::t("not connected");
    case MediaError::Quota:
        return i18n::t("quota");
    case MediaError::None:
    case MediaError::Other:
        break;
    }
    return i18n::t("other");
}

QString sessionLength(qint64 ms)
{
    const qint64 minutes = ms / 60000;
    if (minutes < 1)
        return i18n::t("less than a minute");
    if (minutes < 60)
        return i18n::t("%1 min").arg(minutes);
    return i18n::t("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

// showHardware: only for encoders. A decoder's GPU use doesn't show as a transform of its own
// (Microsoft's H.264 decoder uses the GPU through DXVA), so "hardware" there would mislead.
QString codecList(const QVector<Codec>& codecs, bool showHardware)
{
    QStringList parts;
    for (const Codec& c : codecs) {
        QString part = c.label + QLatin1Char(' ') + yesNo(c.found);
        if (showHardware && c.found && c.hardware)
            part += i18n::t(" (hardware)");
        parts << part;
    }
    return parts.join(QLatin1String(", "));
}

QString videoDeviceText(int device)
{
    switch (device) {
    case 1:
        return i18n::t("Direct3D 11 hardware");
    case 2:
        return i18n::t("software (WARP)");
    case 3:
        return i18n::t("couldn't create a Direct3D 11 device");
    default:
        return i18n::t("not used yet");
    }
}

QString utcOffset(const QDateTime& time)
{
    const int seconds = time.offsetFromUtc();
    const int minutes = qAbs(seconds) / 60;
    return QString::fromLatin1("UTC%1%2:%3")
        .arg(seconds < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        .arg(minutes / 60, 2, 10, QLatin1Char('0'))
        .arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QString levelName(QString level)
{
    level = level.trimmed().toUpper();
    if (level == QLatin1String("WARNING"))
        return QString::fromLatin1("WARN");
    if (level == QLatin1String("CRITICAL"))
        return QString::fromLatin1("CRIT");
    QString letters;
    for (const QChar c : qAsConst(level)) {
        if (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            letters += c;
    }
    return letters.left(5);
}

const QRegularExpression& isoTimePattern()
{
    static const QRegularExpression re(QString::fromLatin1("^\\d{4}-(\\d{2}-\\d{2})[T ](\\d{2}:\\d{2}:\\d{2})"));
    return re;
}

// "2026-11-02T17:03:40.123+03:30" or "2026-11-02 17:03:40.123456" -> "11-02 17:03:40".
QString shortTime(const QString& stamp)
{
    const QRegularExpressionMatch m = isoTimePattern().match(stamp.trimmed());
    if (!m.hasMatch())
        return {};
    return m.captured(1) + QLatin1Char(' ') + m.captured(2);
}

} // namespace

// ---- log lines ------------------------------------------------------------------------------------

QString mark(char kind, const QString& value)
{
    return QString(beginMark()) + QLatin1Char(kind) + withoutMarkers(value) + endMark();
}

void setLogTailProvider(LogTailProvider provider)
{
    g_logTailProvider.store(provider);
}

LogTailProvider logTailProvider()
{
    return g_logTailProvider.load();
}

LogLine parsePluginLogLine(const QString& line)
{
    LogLine   result;
    const int space = line.indexOf(QLatin1Char(' '));
    if (space <= 0) {
        result.text = line;
        return result;
    }
    result.time = shortTime(line.left(space));
    if (result.time.isEmpty()) {
        result.text = line;
        return result;
    }
    int pos = space;
    while (pos < line.size() && line.at(pos) == QLatin1Char(' '))
        ++pos;
    int levelEnd = line.indexOf(QLatin1Char(' '), pos);
    if (levelEnd < 0)
        levelEnd = line.size();
    result.level = levelName(line.mid(pos, levelEnd - pos));
    pos          = levelEnd;
    while (pos < line.size() && line.at(pos) == QLatin1Char(' '))
        ++pos;
    result.text = line.mid(pos);
    return result;
}

// A line the legacy ts3::log(QString) wrote is one unclassified span ('u', logtext.h): the wording rules
// for TeamSpeak's log apply to it (fail-closed) instead of hiding the whole line.
QString unwrapUnclassified(const QString& marked)
{
    const QString open = QString(beginMark()) + QLatin1Char('u');
    if (!marked.startsWith(open) || !marked.endsWith(endMark()))
        return marked;
    const QString inner = marked.mid(open.size(), marked.size() - open.size() - 1);
    if (inner.contains(beginMark()) || inner.contains(endMark()))
        return marked;
    return markLegacy(inner);
}

LogTail pluginLogTail(const QStringList& rawLines, int totalLines)
{
    LogTail tail;
    for (const QString& raw : rawLines) {
        if (raw.trimmed().isEmpty())
            continue;
        LogLine line = parsePluginLogLine(raw);
        line.text    = unwrapUnclassified(line.text);
        tail.lines.append(line);
    }
    if (tail.lines.isEmpty())
        return tail; // Source::None: fall back to TeamSpeak's log
    tail.source = LogTail::Source::PluginLog;
    tail.marked = true;
    tail.total  = qMax(totalLines, tail.lines.size());
    return tail;
}

LogTail parseTeamSpeakLog(const QByteArray& utf8, int maxLines)
{
    LogTail tail;
    tail.source = LogTail::Source::TeamSpeakLog;
    tail.marked = false;

    QVector<LogLine> lines;
    const QString    text = QString::fromUtf8(utf8);
    for (const QStringRef& raw : text.splitRef(QLatin1Char('\n'))) {
        // time|LEVEL   |Channel       |id |message (the id column is the server tab, often empty)
        const QString line   = raw.toString().remove(QLatin1Char('\r'));
        const int     first  = line.indexOf(QLatin1Char('|'));
        const int     second = first < 0 ? -1 : line.indexOf(QLatin1Char('|'), first + 1);
        const int     third  = second < 0 ? -1 : line.indexOf(QLatin1Char('|'), second + 1);
        if (third < 0)
            continue;
        if (line.mid(second + 1, third - second - 1).trimmed() != QLatin1String("TSMedia"))
            continue;
        int       messageStart = third + 1;
        const int fourth       = line.indexOf(QLatin1Char('|'), third + 1);
        if (fourth > 0 && fourth - third - 1 <= 12) {
            bool idColumn = true;
            for (const QChar c : line.midRef(third + 1, fourth - third - 1)) {
                if (!c.isDigit() && c != QLatin1Char(' '))
                    idColumn = false;
            }
            if (idColumn)
                messageStart = fourth + 1;
        }
        LogLine entry;
        entry.time  = shortTime(line.left(first));
        entry.level = levelName(line.mid(first + 1, second - first - 1));
        entry.text  = line.mid(messageStart).trimmed();
        lines.append(entry);
    }
    tail.total = lines.size();
    const int keep = qBound(0, maxLines, lines.size());
    tail.lines     = lines.mid(lines.size() - keep);
    return tail;
}

QString markLegacy(const QString& text)
{
    if (text.contains(beginMark()))
        return text; // already marked by the structured log

    for (const CompiledPattern& p : legacyPatterns()) {
        const QRegularExpressionMatch m = p.re.match(text);
        if (!m.hasMatch())
            continue;
        QString out;
        int     pos = 0;
        for (int group = 1; group <= m.lastCapturedIndex() && group <= p.kinds.size(); ++group) {
            if (m.capturedStart(group) < 0)
                continue;
            const char kind = p.kinds.at(group - 1);
            out += text.mid(pos, m.capturedStart(group) - pos);
            // A kept part (sizes, the server's error text) still gets the file rules: fail closed.
            if (kind == '-') {
                const QString kept = markMatches(m.captured(group), remotePathPattern(), 'f', true);
                out += mapUnmarked(kept, [](const QString& s) { return markMatches(s, fileNamePattern(), 'f', false); });
            } else {
                out += mark(kind, m.captured(group));
            }
            pos = m.capturedEnd(group);
        }
        return out + text.mid(pos);
    }

    // Anything else (a newer message, a test line): fail closed.
    QString marked = mapUnmarked(text, markCurlyQuotes);
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, localPathPattern(), 'l', true); });
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, ipPattern(), 'n', false); });
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, uniqueIdPattern(), 'n', false); });
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, remotePathPattern(), 'f', true); });
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, fileNamePattern(), 'f', false); });
    return marked;
}

QString markSafetyNet(const QString& text)
{
    QString marked = mapUnmarked(text, markCurlyQuotes);
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, localPathPattern(), 'l', true); });
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, ipPattern(), 'n', false); });
    marked         = mapUnmarked(marked, [](const QString& s) { return markMatches(s, uniqueIdPattern(), 'n', false); });
    return marked;
}

Redactor::Redactor(bool includeNames, const QString& homeDir)
    : m_includeNames(includeNames)
    , m_home(QDir::cleanPath(homeDir))
{
    if (m_home == QLatin1String("."))
        m_home.clear();
}

QString Redactor::token(char kind, const QString& rawValue, bool reveal)
{
    if (kind != 'f' && kind != 'l')
        kind = 'n';
    QString value = plainLine(rawValue);
    if (reveal && m_includeNames && kind != 'n') {
        if (!m_home.isEmpty() && m_home.size() > 3) { // never a bare drive
            value.replace(m_home, QString::fromLatin1("%USERPROFILE%"), Qt::CaseInsensitive);
            value.replace(QDir::toNativeSeparators(m_home), QString::fromLatin1("%USERPROFILE%"), Qt::CaseInsensitive);
        }
        return value;
    }
    const QString key    = QLatin1Char(kind) + value;
    int           number = m_numbers.value(key);
    if (number == 0) {
        number = ++m_counts[kind];
        m_numbers.insert(key, number);
    }
    switch (kind) {
    case 'f':
        return i18n::t("<file %1>").arg(number);
    case 'l':
        return i18n::t("<path %1>").arg(number);
    default:
        return i18n::t("<name %1>").arg(number);
    }
}

QString Redactor::apply(const QString& marked)
{
    QString out;
    int     pos = 0;
    while (pos < marked.size()) {
        const int begin = marked.indexOf(beginMark(), pos);
        QString   plain = marked.mid(pos, begin < 0 ? -1 : begin - pos);
        plain.remove(endMark()); // a stray end marker
        out += plain;
        if (begin < 0)
            break;
        char kind  = 'n';
        int  start = begin + 1;
        if (start < marked.size() && marked.at(start).unicode() < 0x80 && marked.at(start).isLetter()) {
            kind = static_cast<char>(marked.at(start).toLatin1());
            ++start;
        }
        const int end = marked.indexOf(endMark(), start);
        if (end < 0) {
            // Unbalanced: the rest of the line is hidden, names included or not.
            out += token(kind, withoutMarkers(marked.mid(start)), false);
            break;
        }
        out += token(kind, withoutMarkers(marked.mid(start, end - start)), true);
        pos = end + 1;
    }
    return out;
}

// ---- the facts ------------------------------------------------------------------------------------

QString windowsName(const QString& product, int build, const QString& displayVersion)
{
    QString name = cleanValue(product, 60);
    if (name.isEmpty())
        name = QString::fromLatin1("Windows");
    // Windows 11 kept "Windows 10" in ProductName.
    if (build >= 22000 && name.startsWith(QLatin1String("Windows 10")))
        name.replace(0, 10, QString::fromLatin1("Windows 11"));
    const QString version = cleanValue(displayVersion, 12);
    if (!version.isEmpty())
        name += QLatin1Char(' ') + version;
    return name;
}

bool isNEdition(const QString& editionId)
{
    const QString id = editionId.trimmed();
    return id.size() > 1 && id.endsWith(QLatin1Char('N'));
}

QString cleanValue(const QString& value, int maxLength)
{
    QString out;
    for (const QChar c : value) {
        if (c.unicode() >= 0x20 && c.unicode() <= 0x7E)
            out += c;
        else if (c.isSpace())
            out += QLatin1Char(' ');
    }
    out = out.simplified();
    if (out.size() > maxLength)
        out = out.left(qMax(0, maxLength - 3)) + QLatin1String("...");
    return out;
}

QString format(const Facts& f, bool includeNames, const QString& homeDir)
{
    QStringList head;
    const auto  add  = [&head](const QString& line) { head << line; };
    const auto  item = [&head](const QString& line) { head << QString::fromLatin1("  ") + line; };

    const QString product = f.pluginName.isEmpty() ? QString::fromLatin1("TS Media chat") : cleanValue(f.pluginName, 40);
    add(i18n::t("%1 diagnostic info").arg(product));
    const QDateTime created = f.created.isValid() ? f.created : QDateTime::currentDateTime();
    add(i18n::t("Created: %1 (%2)").arg(QLocale::c().toString(created, QString::fromLatin1("yyyy-MM-dd HH:mm")), utcOffset(created)));
    add(includeNames ? i18n::t("Privacy: file names included (your user folder shows as %USERPROFILE%). "
                               "No server addresses, server or channel names, unique IDs or nicknames.")
                     : i18n::t("Privacy: file names hidden. No server addresses, server or channel names, unique IDs or nicknames."));
    add(QString());

    // Versions
    const QString bits = f.pluginBits > 0 ? i18n::t("%1-bit").arg(f.pluginBits) : i18n::t("unknown bitness");
    add(i18n::t("Versions"));
    item(i18n::t("Plugin: %1 %2, %3, Qt %4 (built with %5), plugin API %6")
             .arg(product, cleanValue(f.pluginVersion, 20), bits, cleanValue(f.qtRuntime, 20), cleanValue(f.qtBuilt, 20))
             .arg(f.pluginApi));
    QString teamSpeak = cleanValue(f.teamSpeakVersion, 60);
    if (teamSpeak.isEmpty())
        teamSpeak = i18n::t("unknown version");
    else if (f.teamSpeakVersionFromLib)
        teamSpeak += i18n::t(" (client library)");
    const QString config = f.configFolder == 1 ? i18n::t("standard") : f.configFolder == 2 ? i18n::t("portable") : i18n::t("unknown");
    item(i18n::t("TeamSpeak: %1, %2, config folder: %3").arg(teamSpeak, bits, config));
    QString windows = windowsName(f.windowsProduct, f.windowsBuild, f.windowsDisplayVersion);
    if (f.windowsBuild > 0)
        windows += i18n::t(", build %1.%2").arg(f.windowsBuild).arg(f.windowsUbr);
    if (!f.nativeArch.isEmpty())
        windows += QString::fromLatin1(", ") + cleanValue(f.nativeArch, 12);
    if (f.wow64)
        windows += i18n::t(" (32-bit TeamSpeak on 64-bit Windows)");
    windows += i18n::t(", N edition: %1").arg(f.windowsEdition.isEmpty() ? i18n::t("unknown") : yesNo(isNEdition(f.windowsEdition)));
    item(i18n::t("Windows: %1").arg(windows));

    // Media
    add(i18n::t("Media"));
    if (!f.mediaFoundationPresent)
        item(i18n::t("Media Foundation: missing (Windows N editions need the Media Feature Pack)"));
    else if (!f.mediaFoundationStarted)
        item(i18n::t("Media Foundation: present, but it didn't start"));
    else
        item(i18n::t("Media Foundation: available"));
    if (f.codecs.checked) {
        if (!f.codecs.error.isEmpty()) {
            item(i18n::t("Codecs: couldn't check (%1)").arg(cleanValue(f.codecs.error, 40)));
        } else {
            item(i18n::t("Video decoders: %1").arg(codecList(f.codecs.videoDecoders, false)));
            item(i18n::t("Audio decoders: %1").arg(codecList(f.codecs.audioDecoders, false)));
            item(i18n::t("Encoders: %1 [checked in %2 ms]").arg(codecList(f.codecs.encoders, true)).arg(f.codecs.elapsedMs));
        }
    }
    item(i18n::t("Video output: %1").arg(videoDeviceText(f.videoDevice)));
    QStringList display;
    if (f.scalePercent > 0)
        display << i18n::t("%1% scaling (device pixel ratio %2)").arg(f.scalePercent).arg(f.devicePixelRatio, 0, 'g', 3);
    if (f.screens > 0)
        display << (f.screens == 1 ? i18n::t("1 screen") : i18n::t("%1 screens").arg(f.screens));
    display << i18n::t("animations %1").arg(onOff(f.animations));
    if (f.theme > 0)
        display << i18n::t("TeamSpeak theme %1").arg(f.theme == 2 ? i18n::t("dark") : i18n::t("light"));
    item(i18n::t("Display: %1").arg(display.join(QLatin1String(", "))));

    // Connections: versions only, never addresses or names
    add(i18n::t("Connections"));
    QString servers = i18n::t("Connected servers: %1").arg(f.connections);
    QStringList versions;
    for (const QString& v : f.serverVersions)
        versions << cleanValue(v, 60);
    if (!versions.isEmpty())
        servers += QString::fromLatin1(" (") + versions.join(QLatin1String("; ")) + QLatin1Char(')');
    item(servers);
    if (f.chatViews >= 0)
        item(i18n::t("Chat hooks: %1 chat views, %2 input lines").arg(f.chatViews).arg(qMax(0, f.chatInputs)));

    // Settings: values and on/off only; the folder and the link only say whether they were changed
    const Settings& s = f.settings;
    add(i18n::t("Settings"));
    item(i18n::t("Show media in chat: %1. Images and GIFs: %2. Videos: %3. Autoplay GIFs: %4. Preview size: %5 × %6")
             .arg(onOff(s.inlinePreviews), s.autoDownloadImages ? i18n::t("automatic up to %1 MB").arg(s.autoDownloadMaxMB) : i18n::t("on click"),
                  s.videoAutoDownloadMB > 0 ? i18n::t("automatic up to %1 MB").arg(s.videoAutoDownloadMB) : i18n::t("when played"), onOff(s.autoplayGifs))
             .arg(s.previewMaxWidth)
             .arg(s.previewMaxHeight));
    item(i18n::t("Volume %1%, start muted %2, loop %3").arg(s.videoVolume).arg(onOff(s.videosStartMuted), onOff(s.loopVideos)));
    item(i18n::t("Send by drop %1, by paste %2, PNG to JPEG %3, previews %4")
             .arg(onOff(s.interceptDragDrop), onOff(s.interceptPaste), onOff(s.convertLargePngToJpeg), onOff(s.generatePreviews)));
    const bool defaultLink   = s.pluginDownloadUrl == QLatin1String(Settings::defaultDownloadUrl);
    const bool defaultFolder = Settings::normalizeUploadDirectory(s.uploadDirectory) == QLatin1String(Settings::defaultUploadDirectory);
    item(i18n::t("Note %1, link %2. Upload limit %3 MB, folder %4. Cache limit %5 MB")
             .arg(onOff(s.addRequiredNotice), defaultLink ? i18n::t("default") : i18n::t("changed"))
             .arg(s.uploadMaxMB)
             .arg(defaultFolder ? i18n::t("default") : i18n::t("changed"))
             .arg(s.cacheLimitMB));
    // 2.2: data saver and per-server settings (how many servers, never which)
    item(i18n::t("Data saver %1, %2 servers with their own settings").arg(onOff(s.dataSaver)).arg(s.servers.size()));

    // Activity
    const SessionCounts& c = f.session;
    add(c.sessionMs >= 0 ? i18n::t("Activity this session (%1)").arg(sessionLength(c.sessionMs)) : i18n::t("Activity this session"));
    QString downloads = i18n::t("Downloads: %1 finished, %2 failed").arg(c.downloadsOk).arg(c.downloadsFailed);
    QStringList causes;
    for (auto it = c.downloadErrors.cbegin(); it != c.downloadErrors.cend(); ++it) {
        if (it.value() > 0)
            causes << i18n::t("%1 %2").arg(errorLabel(it.key())).arg(it.value());
    }
    if (!causes.isEmpty())
        downloads += QString::fromLatin1(" (") + causes.join(QLatin1String(", ")) + QLatin1Char(')');
    item(downloads);
    item(i18n::t("Uploads: %1 finished, %2 failed, %3 canceled").arg(c.uploadsOk).arg(c.uploadsFailed).arg(c.uploadsCanceled));
    if (f.cacheKnown)
        item(i18n::t("Cache: %1 of %2 MB, %3 files").arg(formatSize(f.cacheBytes)).arg(s.cacheLimitMB).arg(f.cacheFiles));
    else
        item(i18n::t("Cache: couldn't be measured"));

    // Other features (counts and states only)
    for (const Section& section : f.extra) {
        add(plainLine(section.title));
        for (const QString& line : section.lines)
            item(plainLine(line));
    }

    for (QString& line : head)
        line = capLine(plainLine(line), kMaxLineLength);
    QString report = head.join(QLatin1Char('\n'));
    if (report.size() > kMaxReportLength)
        return report.left(kMaxReportLength);

    // Recent log: as many of the newest lines as fit, numbered in reading order.
    const QString sourceName = f.log.source == LogTail::Source::PluginLog ? i18n::t("the plugin log") : i18n::t("the TeamSpeak client log");
    const int     available  = f.log.lines.size();
    for (int count = qMin(kMaxLogLines, available); count >= 0; --count) {
        Redactor    redactor(includeNames, homeDir);
        QStringList lines;
        for (int i = available - count; i < available; ++i) {
            const LogLine& l      = f.log.lines.at(i);
            const QString  raw    = l.text.left(2000);
            const QString  text   = markSafetyNet(f.log.marked ? raw : markLegacy(raw));
            QString        prefix = QString::fromLatin1("  ");
            if (!l.time.isEmpty())
                prefix += plainLine(l.time).left(20) + QLatin1Char(' ');
            prefix += levelName(l.level).leftJustified(5, QLatin1Char(' ')) + QLatin1Char(' ');
            lines << capLine(prefix + plainLine(redactor.apply(text)), kMaxLineLength);
        }
        QString title;
        if (f.log.source == LogTail::Source::None || (available == 0 && !f.log.problem.isEmpty()))
            title = i18n::t("Recent log: not available (%1)").arg(f.log.problem.isEmpty() ? i18n::t("no log found") : plainLine(f.log.problem).left(120));
        else if (available == 0)
            title = i18n::t("Recent log: no lines yet (%1)").arg(sourceName);
        else if (!f.log.partial && count == f.log.total)
            title = i18n::t("Recent log (all %1 lines, from %2)").arg(count).arg(sourceName);
        else if (f.log.partial)
            title = i18n::t("Recent log (last %1 of at least %2 lines, from %3)").arg(count).arg(qMax(f.log.total, available)).arg(sourceName);
        else
            title = i18n::t("Recent log (last %1 of %2 lines, from %3)").arg(count).arg(qMax(f.log.total, available)).arg(sourceName);
        const QString full = report + QLatin1Char('\n') + title + (lines.isEmpty() ? QString() : QLatin1Char('\n') + lines.join(QLatin1Char('\n')));
        if (full.size() <= kMaxReportLength || count == 0)
            return full.left(kMaxReportLength);
    }
    return report;
}

QUrl bugReportUrl(const Facts& f)
{
    const QString product = f.pluginName.isEmpty() ? QString::fromLatin1("TS Media chat") : cleanValue(f.pluginName, 40);
    const QString version = cleanValue(f.pluginVersion, 20);

    QString    query = QString::fromLatin1("template=bug_report.yml");
    const auto field = [&query](const char* id, const QString& value) {
        if (value.isEmpty())
            return;
        query += QLatin1Char('&') + QString::fromLatin1(id) + QLatin1Char('=') + QString::fromLatin1(QUrl::toPercentEncoding(value));
    };
    field("plugin-version", product + QLatin1Char(' ') + version);
    field("teamspeak-version", cleanValue(f.teamSpeakVersion, 60));
    if (f.pluginBits == 64 || f.pluginBits == 32)
        field("client-arch", QString::fromLatin1("%1-bit").arg(f.pluginBits)); // exactly the dropdown's option
    QString windows = windowsName(f.windowsProduct, f.windowsBuild, f.windowsDisplayVersion);
    if (f.windowsBuild > 0)
        windows += QString::fromLatin1(" (build %1.%2)").arg(f.windowsBuild).arg(f.windowsUbr);
    field("windows-version", windows);
    field("log-line", product + QLatin1Char(' ') + version + QString::fromLatin1(" loaded"));

    QUrl url(QString::fromLatin1(Settings::defaultDownloadUrl) + QString::fromLatin1("/issues/new"));
    url.setQuery(query, QUrl::StrictMode);
    return url;
}

} // namespace diag
