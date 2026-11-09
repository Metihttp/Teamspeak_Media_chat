#include "previewstore.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

#include "core.h"
#include "settings.h"

namespace linkpreview {

namespace {

constexpr int kFormat = 1;

QString str(const QJsonObject& o, const char* key)
{
    return o.value(QLatin1String(key)).toString();
}

qint64 num(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? static_cast<qint64>(v.toDouble()) : 0;
}

QJsonObject toObject(const Announcement& a, qint64 seenSec, qint64 usedSec)
{
    QJsonObject o;
    o.insert(QStringLiteral("m"), a.messageKey);
    o.insert(QStringLiteral("u"), a.urlKey);
    o.insert(QStringLiteral("k"), QString(QLatin1Char(kindCode(a.kind))));
    o.insert(QStringLiteral("s"), a.serverUid);
    o.insert(QStringLiteral("c"), QString::number(a.channelId)); // a string: 64-bit ids don't fit a double
    o.insert(QStringLiteral("p"), a.previewPath);
    o.insert(QStringLiteral("h"), a.previewSha);
    o.insert(QStringLiteral("ps"), static_cast<double>(a.previewSize));
    if (!a.mediaPath.isEmpty()) {
        o.insert(QStringLiteral("f"), a.mediaPath);
        o.insert(QStringLiteral("fh"), a.mediaSha);
        o.insert(QStringLiteral("fs"), static_cast<double>(a.mediaSize));
    }
    if (a.width > 0) {
        o.insert(QStringLiteral("w"), a.width);
        o.insert(QStringLiteral("ht"), a.height);
    }
    if (a.durationMs > 0)
        o.insert(QStringLiteral("d"), static_cast<double>(a.durationMs));
    if (!a.title.isEmpty())
        o.insert(QStringLiteral("t"), a.title);
    if (!a.author.isEmpty())
        o.insert(QStringLiteral("a"), a.author);
    if (a.privateChat)
        o.insert(QStringLiteral("pc"), true);
    o.insert(QStringLiteral("from"), a.senderUid);
    o.insert(QStringLiteral("seen"), static_cast<double>(seenSec));
    o.insert(QStringLiteral("used"), static_cast<double>(usedSec));
    return o;
}

bool fromObject(const QJsonObject& o, Announcement* a)
{
    const QString k = str(o, "k");
    bool          ok = false;
    a->messageKey  = str(o, "m");
    a->urlKey      = str(o, "u");
    a->kind        = k.size() == 1 ? kindOf(k.at(0).toLatin1()) : Kind::None;
    a->serverUid   = str(o, "s");
    a->channelId   = str(o, "c").toULongLong(&ok);
    a->previewPath = str(o, "p");
    a->previewSha  = str(o, "h");
    a->previewSize = num(o, "ps");
    a->mediaPath   = str(o, "f");
    a->mediaSha    = str(o, "fh");
    a->mediaSize   = num(o, "fs");
    a->width       = static_cast<int>(qBound<qint64>(-1, num(o, "w"), kMaxSide + 1));
    a->height      = static_cast<int>(qBound<qint64>(-1, num(o, "ht"), kMaxSide + 1));
    a->durationMs  = num(o, "d");
    a->title       = str(o, "t");
    a->author      = str(o, "a");
    a->privateChat = o.value(QLatin1String("pc")).toBool();
    a->senderUid   = str(o, "from");
    return ok && isValid(*a);
}

bool sameFiles(const Announcement& x, const Announcement& y)
{
    return x.kind == y.kind && x.serverUid == y.serverUid && x.channelId == y.channelId && x.previewPath == y.previewPath && x.previewSha == y.previewSha
           && x.previewSize == y.previewSize && x.mediaPath == y.mediaPath && x.mediaSha == y.mediaSha && x.mediaSize == y.mediaSize && x.width == y.width
           && x.height == y.height && x.durationMs == y.durationMs && x.title == y.title && x.author == y.author;
}

} // namespace

PreviewStore::PreviewStore(QObject* parent)
    : PreviewStore(Limits(), parent)
{
}

PreviewStore::PreviewStore(const Limits& limits, QObject* parent)
    : QObject(parent)
    , m_limits(limits)
{
}

PreviewStore::Entry* PreviewStore::locate(const QString& hash, qint64 timeSec, const QString& urlKey, qint64 toleranceSec)
{
    return const_cast<Entry*>(static_cast<const PreviewStore*>(this)->locate(hash, timeSec, urlKey, toleranceSec));
}

const PreviewStore::Entry* PreviewStore::locate(const QString& hash, qint64 timeSec, const QString& urlKey, qint64 toleranceSec) const
{
    const auto found = m_byHash.constFind(hash);
    if (found == m_byHash.constEnd())
        return nullptr;
    const Entry* best     = nullptr;
    qint64       bestDiff = -1;
    for (const Entry& e : found.value()) {
        if (e.a.urlKey != urlKey)
            continue;
        const qint64 diff = qAbs(e.timeSec - timeSec);
        if (diff > toleranceSec)
            continue;
        // The nearest; between two as near, the newer one.
        if (!best || diff < bestDiff || (diff == bestDiff && e.timeSec > best->timeSec)) {
            best     = &e;
            bestDiff = diff;
        }
    }
    return best;
}

int PreviewStore::countForMessage(const QString& hash, qint64 timeSec) const
{
    int n = 0;
    for (const Entry& e : m_byHash.value(hash))
        n += e.timeSec == timeSec ? 1 : 0;
    return n;
}

PreviewStore::Add PreviewStore::check(const Announcement& a) const
{
    QString hash;
    qint64  timeSec = 0;
    if (!isValid(a) || !textreact::parseLocalKey(a.messageKey, &hash, &timeSec))
        return Add::Rejected;
    if (const Entry* e = locate(hash, timeSec, a.urlKey, 0)) {
        if (e->a.senderUid != a.senderUid)
            return Add::Rejected;
        return sameFiles(e->a, a) ? Add::Unchanged : Add::Replaced;
    }
    return countForMessage(hash, timeSec) >= m_limits.maxPerMessage ? Add::Rejected : Add::Added;
}

PreviewStore::Add PreviewStore::add(const Announcement& a, qint64 nowSec)
{
    QString hash;
    qint64  timeSec = 0;
    if (!isValid(a) || !textreact::parseLocalKey(a.messageKey, &hash, &timeSec))
        return Add::Rejected;
    // The same message (exactly: an announcement names one message) and link.
    if (Entry* e = locate(hash, timeSec, a.urlKey, 0)) {
        if (e->a.senderUid != a.senderUid)
            return Add::Rejected; // only whoever announced it may announce it again
        e->seenSec = nowSec;
        if (sameFiles(e->a, a))
            return Add::Unchanged;
        const quint64 sch = a.sch;
        e->a              = a;
        e->a.sch          = sch;
        emit announced(e->a);
        emit changed();
        return Add::Replaced;
    }
    if (countForMessage(hash, timeSec) >= m_limits.maxPerMessage)
        return Add::Rejected;
    Entry e;
    e.a       = a;
    e.timeSec = timeSec;
    e.seenSec = nowSec;
    e.usedSec = nowSec;
    m_byHash[hash].append(e);
    ++m_count;
    trim(nowSec);
    emit announced(a);
    emit changed();
    return Add::Added;
}

const Announcement* PreviewStore::find(const QString& messageKey, const QString& urlKey) const
{
    QString hash;
    qint64  timeSec = 0;
    if (!textreact::parseLocalKey(messageKey, &hash, &timeSec))
        return nullptr;
    const Entry* e = locate(hash, timeSec, urlKey, m_limits.toleranceSec);
    if (!e)
        return nullptr;
    e->usedSec = QDateTime::currentSecsSinceEpoch();
    return &e->a;
}

QVector<Announcement> PreviewStore::forMessage(const QString& messageKey) const
{
    QVector<Announcement> out;
    QString               hash;
    qint64                timeSec = 0;
    if (!textreact::parseLocalKey(messageKey, &hash, &timeSec))
        return out;
    for (const Entry& e : m_byHash.value(hash)) {
        if (qAbs(e.timeSec - timeSec) <= m_limits.toleranceSec)
            out.append(e.a);
    }
    return out;
}

int PreviewStore::count() const
{
    return m_count;
}

void PreviewStore::clear()
{
    // What had a sender's preview stays known for the session (bounded: a later clear's entries first).
    if (m_clearedCount + m_count > m_limits.maxEntries) {
        m_cleared.clear();
        m_clearedCount = 0;
    }
    for (auto it = m_byHash.constBegin(); it != m_byHash.constEnd(); ++it) {
        for (const Entry& e : it.value()) {
            if (m_clearedCount >= m_limits.maxEntries)
                break;
            m_cleared[it.key()].append({e.timeSec, e.a.urlKey});
            ++m_clearedCount;
        }
    }
    reset();
}

void PreviewStore::reset()
{
    m_byHash.clear();
    m_count = 0;
}

bool PreviewStore::clearedEarlier(const QString& messageKey, const QString& urlKey) const
{
    QString hash;
    qint64  timeSec = 0;
    if (m_clearedCount == 0 || urlKey.isEmpty() || !textreact::parseLocalKey(messageKey, &hash, &timeSec))
        return false;
    for (const Cleared& c : m_cleared.value(hash)) {
        if (c.urlKey == urlKey && qAbs(c.timeSec - timeSec) <= m_limits.toleranceSec)
            return true;
    }
    return false;
}

void PreviewStore::trim(qint64 nowSec)
{
    // Too old without use: gone.
    for (auto it = m_byHash.begin(); it != m_byHash.end();) {
        QVector<Entry>& list = it.value();
        const int       before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const Entry& e) { return nowSec - qMax(e.usedSec, e.seenSec) > m_limits.maxAgeSec; }),
                   list.end());
        m_count -= before - list.size();
        if (list.isEmpty())
            it = m_byHash.erase(it);
        else
            ++it;
    }
    if (m_count <= m_limits.maxEntries)
        return;
    // The least recently used beyond maxEntries.
    QVector<qint64> times;
    times.reserve(m_count);
    for (const QVector<Entry>& list : qAsConst(m_byHash)) {
        for (const Entry& e : list)
            times.append(qMax(e.usedSec, e.seenSec));
    }
    std::sort(times.begin(), times.end());
    const int    drop   = m_count - m_limits.maxEntries;
    const qint64 cutoff = times.at(drop - 1);
    int          left   = drop;
    for (auto it = m_byHash.begin(); it != m_byHash.end() && left > 0;) {
        QVector<Entry>& list = it.value();
        for (int i = list.size() - 1; i >= 0 && left > 0; --i) {
            if (qMax(list.at(i).usedSec, list.at(i).seenSec) <= cutoff) {
                list.removeAt(i);
                --m_count;
                --left;
            }
        }
        if (list.isEmpty())
            it = m_byHash.erase(it);
        else
            ++it;
    }
}

QByteArray PreviewStore::toJson(qint64 nowSec) const
{
    struct Item {
        const Entry* e;
        qint64       used;
    };
    QVector<Item> items;
    for (const QVector<Entry>& list : m_byHash) {
        for (const Entry& e : list) {
            if (nowSec - qMax(e.usedSec, e.seenSec) <= m_limits.maxAgeSec)
                items.append({&e, qMax(e.usedSec, e.seenSec)});
        }
    }
    // Most recently used first: what doesn't fit into maxFileBytes is the oldest.
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.used > y.used; });
    QJsonArray entries;
    qint64     bytes = 64;
    for (const Item& item : qAsConst(items)) {
        const QJsonObject o    = toObject(item.e->a, item.e->seenSec, item.e->usedSec);
        const qint64      size = QJsonDocument(o).toJson(QJsonDocument::Compact).size() + 1;
        if (bytes + size > m_limits.maxFileBytes)
            break;
        bytes += size;
        entries.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), kFormat);
    root.insert(QStringLiteral("entries"), entries);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool PreviewStore::fromJson(const QByteArray& json, qint64 nowSec)
{
    reset();
    QJsonParseError     error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String("format")).toInt() != kFormat || !root.value(QLatin1String("entries")).isArray())
        return false;
    for (const QJsonValue& v : root.value(QLatin1String("entries")).toArray()) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        Announcement      a;
        QString           hash;
        qint64            timeSec = 0;
        if (!fromObject(o, &a) || !textreact::parseLocalKey(a.messageKey, &hash, &timeSec))
            continue;
        Entry e;
        e.a       = a;
        e.timeSec = timeSec;
        e.seenSec = qBound<qint64>(0, num(o, "seen"), nowSec);
        e.usedSec = qBound<qint64>(0, num(o, "used"), nowSec);
        if (nowSec - qMax(e.usedSec, e.seenSec) > m_limits.maxAgeSec || locate(hash, timeSec, a.urlKey, 0) || countForMessage(hash, timeSec) >= m_limits.maxPerMessage)
            continue;
        m_byHash[hash].append(e);
        ++m_count;
    }
    trim(nowSec);
    return true;
}

bool PreviewStore::load(const QString& path, qint64 nowSec)
{
    reset();
    QFile file(path);
    if (!file.exists())
        return false;
    bool ok = false;
    if (file.size() <= m_limits.maxFileBytes && file.open(QIODevice::ReadOnly)) {
        const QByteArray data = file.readAll();
        file.close();
        ok = fromJson(data, nowSec);
    } else {
        file.close();
    }
    if (!ok) {
        // Kept for a look, out of the way of the next save.
        const QString bad = path + QStringLiteral(".bad");
        QFile::remove(bad);
        QFile::rename(path, bad);
        reset();
    }
    return ok;
}

bool PreviewStore::save(const QString& path, qint64 nowSec) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = toJson(nowSec);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

// ---- Core ---------------------------------------------------------------------------------------------

MediaLink linkFor(const Announcement& a)
{
    MediaLink link;
    link.serverUid = a.serverUid;
    link.channelId = a.channelId;
    link.protocol  = MediaLink::kProtocol;
    link.width     = a.width;
    link.height    = a.height;
    const QString file = a.mediaPath.isEmpty() ? a.previewPath : a.mediaPath;
    const int     slash = file.lastIndexOf(QLatin1Char('/'));
    link.path           = slash > 0 ? file.left(slash) : QStringLiteral("/");
    link.fileName       = file.mid(slash + 1);
    if (a.mediaPath.isEmpty()) {
        link.size   = static_cast<quint64>(qMax<qint64>(0, a.previewSize));
        link.sha256 = shaBytes(a.previewSha);
    } else {
        link.size        = static_cast<quint64>(qMax<qint64>(0, a.mediaSize));
        link.sha256      = shaBytes(a.mediaSha);
        link.durationMs  = a.durationMs;
        link.previewFile = a.previewPath;
        link.previewSha  = shaBytes(a.previewSha).left(16); // MediaLink's "ph": the first 16 bytes
    }
    link.dropInvalidMetadata();
    return link;
}

void requestFiles(Core* core, const Announcement& a)
{
    // Nothing is downloaded for a preview nobody sees: with "Show images, videos and file cards in the chat" off
    // Core downloads no media by itself and the chat shows no card (ChatLinkPreviews::enabled); the announcement
    // stays stored and the chat asks for the files itself once it is on again.
    const Settings& s = Settings::instance();
    if (!core || !s.showLinkPreviews || !s.inlinePreviews || !isValid(a))
        return;
    const MediaLink link = linkFor(a);
    if (!link.isValid())
        return;
    core->ensure(link); // the preview at once, the media file by the automatic download rules
    // Only the preview is on the server: it is the "file" here, and a preview always loads (also with the
    // data saver on).
    if (a.mediaPath.isEmpty())
        core->download(link.key(), false);
}

} // namespace linkpreview
