#include "reactions.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

// ---- ReactionView ---------------------------------------------------------------------------------

bool ReactionView::isEmpty() const
{
    for (const Entry& e : per) {
        if (e.count > 0)
            return false;
    }
    return true;
}

int ReactionView::ownMask() const
{
    int mask = 0;
    for (int i = 0; i < proto::kReactionCount; ++i) {
        if (per[i].mine)
            mask |= 1 << i;
    }
    return mask;
}

// ---- ReactionStore --------------------------------------------------------------------------------

ReactionStore::ReactionStore(QObject* parent)
    : ReactionStore(Limits(), parent)
{
}

ReactionStore::ReactionStore(const Limits& limits, QObject* parent)
    : QObject(parent)
    , m_limits(limits)
{
}

void ReactionStore::setKnownKey(KnownKey check)
{
    m_known = std::move(check);
}

bool ReactionStore::isClientUid(const QString& uid)
{
    // TeamSpeak identities are base64 (28 characters); ServerQuery logins are plain words.
    if (uid.isEmpty() || uid.size() > 64)
        return false;
    for (const QChar ch : uid) {
        const ushort c = ch.unicode();
        const bool   ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

QString ReactionStore::cleanName(const QString& name) const
{
    QString out;
    out.reserve(qMin(name.size(), m_limits.maxNameChars));
    for (const QChar ch : name) {
        const QChar::Category category = ch.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            continue;
        out += ch;
        if (out.size() >= m_limits.maxNameChars)
            break;
    }
    return out.trimmed();
}

ReactionStore::Apply ReactionStore::applyRemote(const QString& serverUid, const QString& key, const QString& uid, const QString& name, quint8 mask,
                                                qint64 nowMs)
{
    if (!proto::isMediaKey(key) || !isClientUid(uid))
        return Apply::Rejected;
    mask &= proto::kAllReactions;
    if (m_known && !m_known(key, serverUid)) {
        // Later messages from the same reactor replace their earlier one; the oldest goes first when full.
        for (int i = 0; i < m_orphans.size(); ++i) {
            if (m_orphans.at(i).key == key && m_orphans.at(i).uid == uid) {
                m_orphans.removeAt(i);
                break;
            }
        }
        if (m_orphans.size() >= m_limits.maxOrphans)
            m_orphans.removeFirst();
        m_orphans.append({serverUid, key, uid, cleanName(name), mask, nowMs});
        return Apply::Orphaned;
    }
    return set(key, uid, name, mask, nowMs / 1000);
}

void ReactionStore::setOwn(const QString& key, const QString& ownUid, const QString& ownName, quint8 mask, qint64 nowMs)
{
    if (!proto::isMediaKey(key) || !isClientUid(ownUid))
        return;
    set(key, ownUid, ownName, static_cast<quint8>(mask & proto::kAllReactions), nowMs / 1000);
}

ReactionStore::Apply ReactionStore::set(const QString& key, const QString& uid, const QString& name, quint8 mask, qint64 nowSec)
{
    auto media = m_media.find(key);
    if (mask == 0) {
        if (media == m_media.end() || !media->by.contains(uid))
            return Apply::Unchanged;
        media->by.remove(uid);
        if (media->by.isEmpty())
            m_media.erase(media);
        emit changed(key);
        return Apply::Applied;
    }

    const QString clean = cleanName(name);
    if (media == m_media.end()) {
        if (m_media.size() >= m_limits.maxMedia) {
            // Forget the media that changed longest ago.
            auto oldest = m_media.begin();
            for (auto it = m_media.begin(); it != m_media.end(); ++it) {
                if (it->seenSec < oldest->seenSec)
                    oldest = it;
            }
            const QString gone = oldest.key();
            m_media.erase(oldest);
            emit changed(gone);
        }
        media = m_media.insert(key, Media());
    }
    auto reactor = media->by.find(uid);
    if (reactor == media->by.end()) {
        if (media->by.size() >= m_limits.maxReactors) {
            if (media->by.isEmpty())
                m_media.erase(media);
            return Apply::Rejected;
        }
        reactor = media->by.insert(uid, Reactor());
        reactor->tSec = nowSec;
    } else if (reactor->mask == mask && (clean.isEmpty() || reactor->name == clean)) {
        return Apply::Unchanged;
    }
    if (reactor->mask == 0)
        reactor->tSec = nowSec;
    reactor->mask = mask;
    if (!clean.isEmpty())
        reactor->name = clean;
    media->seenSec = nowSec;
    emit changed(key);
    return Apply::Applied;
}

quint8 ReactionStore::maskOf(const QString& key, const QString& uid) const
{
    const auto media = m_media.constFind(key);
    if (media == m_media.constEnd())
        return 0;
    return media->by.value(uid).mask;
}

ReactionView ReactionStore::view(const QString& key, const QString& ownUid) const
{
    ReactionView view;
    const auto   media = m_media.constFind(key);
    if (media == m_media.constEnd())
        return view;
    struct Who {
        QString name;
        qint64  t;
        quint8  mask;
    };
    QVector<Who> others;
    for (auto it = media->by.constBegin(); it != media->by.constEnd(); ++it) {
        const bool own = !ownUid.isEmpty() && it.key() == ownUid;
        for (int i = 0; i < proto::kReactionCount; ++i) {
            if (!(it->mask & (1u << i)))
                continue;
            ++view.per[i].count;
            if (own)
                view.per[i].mine = true;
        }
        if (!own)
            others.append({it->name, it->tSec, it->mask});
    }
    std::sort(others.begin(), others.end(), [](const Who& a, const Who& b) { return a.t != b.t ? a.t < b.t : a.name < b.name; });
    for (const Who& who : qAsConst(others)) {
        for (int i = 0; i < proto::kReactionCount; ++i) {
            if (who.mask & (1u << i))
                view.per[i].others.append(who.name);
        }
    }
    return view;
}

bool ReactionStore::hasReactions(const QString& key) const
{
    return m_media.contains(key);
}

QStringList ReactionStore::keys() const
{
    return m_media.keys();
}

int ReactionStore::reactorCount(const QString& key) const
{
    return m_media.value(key).by.size();
}

QVector<proto::ReactItem> ReactionStore::syncAnswer(const QStringList& requested, const QString& ownUid) const
{
    QVector<proto::ReactItem> items;
    if (ownUid.isEmpty())
        return items;
    QStringList seen;
    for (const QString& key : requested) {
        if (seen.contains(key) || items.size() >= proto::kMaxItems)
            continue;
        seen.append(key);
        const quint8 mask = maskOf(key, ownUid);
        if (mask != 0)
            items.append({key, mask});
    }
    return items;
}

void ReactionStore::resolveOrphans(const QString& key, qint64 nowMs)
{
    expireOrphans(nowMs);
    QVector<Orphan> ready;
    for (int i = m_orphans.size() - 1; i >= 0; --i) {
        if (m_orphans.at(i).key != key)
            continue;
        if (m_known && !m_known(key, m_orphans.at(i).serverUid))
            continue; // known on another server only
        ready.prepend(m_orphans.at(i));
        m_orphans.removeAt(i);
    }
    for (const Orphan& o : qAsConst(ready))
        set(o.key, o.uid, o.name, o.mask, o.atMs / 1000);
}

void ReactionStore::resolveKnownOrphans(qint64 nowMs)
{
    expireOrphans(nowMs);
    QVector<Orphan> ready;
    for (int i = m_orphans.size() - 1; i >= 0; --i) {
        const Orphan& o = m_orphans.at(i);
        if (m_known && !m_known(o.key, o.serverUid))
            continue;
        ready.prepend(o);
        m_orphans.removeAt(i);
    }
    for (const Orphan& o : qAsConst(ready))
        set(o.key, o.uid, o.name, o.mask, o.atMs / 1000);
}

void ReactionStore::expireOrphans(qint64 nowMs)
{
    m_orphans.erase(std::remove_if(m_orphans.begin(), m_orphans.end(), [&](const Orphan& o) { return nowMs - o.atMs > m_limits.orphanTtlMs || nowMs < o.atMs; }),
                    m_orphans.end());
}

void ReactionStore::clear()
{
    const QStringList gone = m_media.keys();
    m_media.clear();
    m_orphans.clear();
    for (const QString& key : gone)
        emit changed(key);
}

void ReactionStore::trim(qint64 nowSec)
{
    for (auto it = m_media.begin(); it != m_media.end();) {
        if (nowSec - it->seenSec > m_limits.maxAgeSec)
            it = m_media.erase(it);
        else
            ++it;
    }
    if (m_media.size() <= m_limits.maxMedia)
        return;
    QVector<QPair<qint64, QString>> order;
    for (auto it = m_media.constBegin(); it != m_media.constEnd(); ++it)
        order.append({it->seenSec, it.key()});
    std::sort(order.begin(), order.end());
    for (int i = 0; i < order.size() - m_limits.maxMedia; ++i)
        m_media.remove(order.at(i).second);
}

QByteArray ReactionStore::toJson(qint64 nowSec) const
{
    // Newest first, and only as many as fit maxFileBytes: load() refuses a larger file (and starts
    // empty), and a full store (maxMedia x maxReactors) can be larger. The oldest media are left out.
    QVector<QPair<qint64, QString>> order;
    for (auto it = m_media.constBegin(); it != m_media.constEnd(); ++it) {
        if (nowSec - it->seenSec <= m_limits.maxAgeSec)
            order.append({it->seenSec, it.key()});
    }
    std::sort(order.begin(), order.end(), [](const QPair<qint64, QString>& a, const QPair<qint64, QString>& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    constexpr qint64 kFrame = 64; // {"media":{...},"v":1} around the entries, with room to spare
    qint64           used   = kFrame;
    QJsonObject      media;
    for (const auto& item : qAsConst(order)) {
        const Media& m = m_media.constFind(item.second).value();
        QJsonObject  by;
        for (auto r = m.by.constBegin(); r != m.by.constEnd(); ++r) {
            QJsonObject reactor;
            reactor.insert(QStringLiteral("n"), r->name);
            reactor.insert(QStringLiteral("e"), QString::fromLatin1(proto::maskToCodes(r->mask)));
            reactor.insert(QStringLiteral("t"), static_cast<double>(r->tSec));
            by.insert(r.key(), reactor);
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("seen"), static_cast<double>(m.seenSec));
        entry.insert(QStringLiteral("by"), by);
        // Its size in the file: {"<key>":{...}} is the entry with its key plus two braces, and the
        // comma before it takes one of them back.
        const qint64 size = QJsonDocument(QJsonObject{{item.second, entry}}).toJson(QJsonDocument::Compact).size();
        if (used + size > m_limits.maxFileBytes)
            break;
        used += size;
        media.insert(item.second, entry);
    }
    QJsonObject root;
    root.insert(QStringLiteral("v"), 1);
    root.insert(QStringLiteral("media"), media);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool ReactionStore::fromJson(const QByteArray& json, qint64 nowSec)
{
    QJsonParseError     error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("v")).toInt(-1) != 1 || !root.value(QStringLiteral("media")).isObject())
        return false;

    m_media.clear();
    m_orphans.clear();
    const QJsonObject media = root.value(QStringLiteral("media")).toObject();
    for (auto it = media.constBegin(); it != media.constEnd(); ++it) {
        if (!proto::isMediaKey(it.key()) || !it.value().isObject())
            continue;
        const QJsonObject entry = it.value().toObject();
        const qint64      seen  = static_cast<qint64>(entry.value(QStringLiteral("seen")).toDouble(-1));
        if (seen <= 0 || seen > nowSec + 86400 || nowSec - seen > m_limits.maxAgeSec || !entry.value(QStringLiteral("by")).isObject())
            continue;
        Media             m;
        m.seenSec          = seen;
        const QJsonObject by = entry.value(QStringLiteral("by")).toObject();
        for (auto r = by.constBegin(); r != by.constEnd() && m.by.size() < m_limits.maxReactors; ++r) {
            if (!isClientUid(r.key()) || !r.value().isObject())
                continue;
            const QJsonObject reactor = r.value().toObject();
            const QJsonValue  codes   = reactor.value(QStringLiteral("e"));
            quint8            mask    = 0;
            if (!codes.isString() || !proto::codesToMask(codes.toString().toLatin1(), &mask) || codes.toString().size() > 256 || mask == 0)
                continue;
            Reactor value;
            value.mask = mask;
            value.name = cleanName(reactor.value(QStringLiteral("n")).toString());
            value.tSec = qBound<qint64>(0, static_cast<qint64>(reactor.value(QStringLiteral("t")).toDouble(0)), nowSec + 86400);
            m.by.insert(r.key(), value);
        }
        if (!m.by.isEmpty())
            m_media.insert(it.key(), m);
    }
    trim(nowSec);
    return true;
}

bool ReactionStore::load(const QString& path, qint64 nowSec)
{
    clear();
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
        m_media.clear();
    }
    return ok;
}

bool ReactionStore::save(const QString& path, qint64 nowSec) const
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
