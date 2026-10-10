#include "emojiprefs.h"

#include <QSettings>

#include "emojidata.h"
#include "peerprotocol.h"

namespace emoji::prefs {

namespace {

struct Store {
    QString      file;
    QVector<int> recent;
    int          tone   = 0;
    bool         loaded = false;
};

Store& store()
{
    static Store s;
    return s;
}

// Keys and values are allocated by Qt (QSettings keeps them in a process-wide cache that outlives the
// DLL; see settings.cpp).
QString key(const char* name)
{
    return QString::fromLatin1(name);
}

void load()
{
    Store& s = store();
    s.loaded = true;
    s.recent.clear();
    s.tone = 0;
    if (s.file.isEmpty())
        return;
    const QSettings ini(s.file, QSettings::IniFormat);
    const QString   codes = ini.value(key("recent")).toString().left(kMaxRecent * 80);
    for (const QString& code : codes.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int id = fromWireCode(code.trimmed().toLatin1());
        if (id >= 0 && !s.recent.contains(id) && s.recent.size() < kMaxRecent)
            s.recent.append(id);
    }
    bool      ok   = false;
    const int tone = ini.value(key("tone")).toInt(&ok);
    s.tone         = ok ? qBound(0, tone, kToneCount) : 0;
}

void save()
{
    const Store& s = store();
    if (s.file.isEmpty())
        return;
    QStringList codes;
    for (int id : s.recent)
        codes.append(QString::fromLatin1(wireCode(id)));
    QSettings ini(s.file, QSettings::IniFormat);
    ini.setValue(key("recent"), codes.join(QLatin1Char(',')));
    ini.setValue(key("tone"), s.tone);
    ini.sync();
}

Store& loadedStore()
{
    Store& s = store();
    if (!s.loaded)
        load();
    return s;
}

QVector<int> defaults()
{
    QVector<int> out;
    // The usual first picks: thumbs up, joy, red heart, fire, open mouth, crying, party popper, 100, pray.
    const char* const codes[] = {"1f44d", "1f602", "2764-fe0f", "1f525", "1f62e", "1f622", "1f389", "1f4af", "1f64f"};
    for (const char* code : codes) {
        const int id = fromWireCode(QByteArray(code));
        if (id >= 0)
            out.append(id);
    }
    return out;
}

} // namespace

void setFile(const QString& path)
{
    Store& s = store();
    s.file   = path;
    load();
}

QVector<int> recent()
{
    const Store& s = loadedStore();
    return s.recent.isEmpty() ? defaults() : s.recent;
}

void noteUsed(int id)
{
    if (!isValid(id))
        return;
    Store& s = loadedStore();
    s.recent.removeAll(id);
    s.recent.prepend(id);
    while (s.recent.size() > kMaxRecent)
        s.recent.removeLast();
    save();
}

void clearRecent()
{
    loadedStore().recent.clear();
    save();
}

int tone()
{
    return loadedStore().tone;
}

void setTone(int t)
{
    Store& s = loadedStore();
    s.tone   = qBound(0, t, kToneCount);
    save();
}

QVector<int> quickReactions()
{
    QVector<int> out;
    for (int i = 0; i < proto::kReactionCount; ++i) {
        const int id = proto::legacyReactionEmoji(i);
        if (id >= 0)
            out.append(id);
    }
    QVector<int> extra = loadedStore().recent;
    extra << fromWireCode("1f389") << fromWireCode("1f4af"); // party popper, 100
    for (int id : qAsConst(extra)) {
        if (out.size() >= 8)
            break;
        if (id >= 0 && !out.contains(id))
            out.append(id);
    }
    return out;
}

} // namespace emoji::prefs
