#include "textreact.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSet>
#include <QTime>

#include <algorithm>

namespace textreact {

namespace {

bool isHexLower(QChar ch)
{
    const ushort c = ch.unicode();
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool isDigits(const QString& text, int from, int maxDigits)
{
    const int n = text.size() - from;
    if (n <= 0 || n > maxDigits)
        return false;
    for (int i = from; i < text.size(); ++i) {
        if (text.at(i) < QLatin1Char('0') || text.at(i) > QLatin1Char('9'))
            return false;
    }
    return !(n > 1 && text.at(from) == QLatin1Char('0')); // one way to write each number
}

// "<hash>-<digits>": the hash and the number, or false.
bool splitKey(const QString& key, int maxDigits, QString* hash, qint64* number)
{
    if (key.size() < kHashChars + 2 || key.at(kHashChars) != QLatin1Char('-'))
        return false;
    const QString h = key.left(kHashChars);
    if (!isHash(h) || !isDigits(key, kHashChars + 1, maxDigits))
        return false;
    bool         ok    = false;
    const qint64 value = key.mid(kHashChars + 1).toLongLong(&ok);
    if (!ok)
        return false;
    if (hash)
        *hash = h;
    if (number)
        *number = value;
    return true;
}

bool isWireRef(const QByteArray& ref)
{
    return parseWireRef(ref, nullptr, nullptr);
}

std::optional<proto::Scope> scopeOf(const proto::Message& m)
{
    const QByteArray s = m.value("s");
    if (s == "c")
        return proto::Scope::Channel;
    if (s == "p")
        return proto::Scope::Private;
    return std::nullopt; // missing, the server chat or anything else
}

QByteArray scopeText(proto::Scope scope)
{
    return scope == proto::Scope::Private ? QByteArray("p") : QByteArray("c");
}

} // namespace

// ---- identity ---------------------------------------------------------------------------------------

const QStringList& emoticonCodes()
{
    // gfx/default*.zip emoticons/emoticons.txt of TeamSpeak 3.6.2 (the longer form of each first).
    static const QStringList codes = [] {
        QStringList list;
        for (const char* code : {":-)", ":)", ":-D", ":D", "8-)", "8)", ";-)", ";)", ":-(", ":(", ":-C", ":C", ":-0", ":0", ":-/", ":/", ":-x", ":x",
                                 ":-P", ":P"})
            list << QString::fromLatin1(code);
        return list;
    }();
    return codes;
}

QString normalized(const QString& text)
{
    QString work = text.left(kMaxTextChars);
    // Emoticons first (case-insensitive: ":p" and ":P" alike), as TeamSpeak may have turned them into
    // pictures on one client and not on another.
    for (const QString& code : emoticonCodes())
        work.remove(code, Qt::CaseInsensitive);
    const QVector<uint> codes = work.toUcs4();
    QVector<uint>       kept;
    kept.reserve(codes.size());
    for (const uint code : codes) {
        if (QChar::isLetterOrNumber(code)) {
            kept.append(QChar::toCaseFolded(code));
            continue;
        }
        const QChar::Category category = QChar::category(code);
        if (category == QChar::Symbol_Other || category == QChar::Symbol_Modifier)
            kept.append(code); // emoji, skin tones
    }
    return QString::fromUcs4(kept.constData(), kept.size());
}

QString contentHash(const QString& senderUid, const QString& text)
{
    if (senderUid.isEmpty() || senderUid.size() > 128)
        return {};
    QByteArray input("tsm-text-1");
    input += '\x1f';
    input += senderUid.toUtf8();
    input += '\x1f';
    input += normalized(text).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex().left(kHashChars));
}

bool isHash(const QString& hash)
{
    if (hash.size() != kHashChars)
        return false;
    for (const QChar ch : hash) {
        if (!isHexLower(ch))
            return false;
    }
    return true;
}

QString localKey(const QString& hash, qint64 timeSec)
{
    if (!isHash(hash) || timeSec <= 0 || timeSec > 99999999999LL)
        return {};
    return hash + QLatin1Char('-') + QString::number(timeSec);
}

bool parseLocalKey(const QString& key, QString* hash, qint64* timeSec)
{
    qint64 t = 0;
    if (!splitKey(key, 11, hash, &t) || t <= 0)
        return false;
    if (timeSec)
        *timeSec = t;
    return true;
}

bool isLocalKey(const QString& key)
{
    return parseLocalKey(key, nullptr, nullptr);
}

QByteArray wireRef(const QString& hash, qint64 ageSec)
{
    if (!isHash(hash) || ageSec < 0 || ageSec > kMaxAgeSec)
        return {};
    return hash.toLatin1() + '-' + QByteArray::number(ageSec);
}

bool parseWireRef(const QByteArray& ref, QString* hash, qint64* ageSec)
{
    if (ref.size() > kHashChars + 1 + 7)
        return false;
    qint64 age = -1;
    if (!splitKey(QString::fromLatin1(ref), 7, hash, &age) || age > kMaxAgeSec)
        return false;
    if (ageSec)
        *ageSec = age;
    return true;
}

QByteArray wireRefOf(const QString& key, qint64 nowSec)
{
    QString hash;
    qint64  t = 0;
    if (!parseLocalKey(key, &hash, &t) || t > nowSec + kFutureSec)
        return {};
    return wireRef(hash, qMax<qint64>(0, nowSec - t));
}

qint64 epochOf(const QDate& date, int secondsOfDay)
{
    if (!date.isValid() || secondsOfDay < 0 || secondsOfDay >= 86400)
        return 0;
    return QDateTime(date, QTime(0, 0).addSecs(secondsOfDay), Qt::LocalTime).toSecsSinceEpoch();
}

qint64 epochOf(const QDate& date, int secondsOfDay, int utcOffsetSec)
{
    if (!date.isValid() || secondsOfDay < 0 || secondsOfDay >= 86400)
        return 0;
    return QDateTime(date, QTime(0, 0).addSecs(secondsOfDay), Qt::OffsetFromUTC, utcOffsetSec).toSecsSinceEpoch();
}

// ---- messages ---------------------------------------------------------------------------------------

std::optional<proto::React> readReact(const proto::Message& message)
{
    if (message.type != "TR")
        return std::nullopt;
    const std::optional<proto::Scope> scope = scopeOf(message);
    if (!scope)
        return std::nullopt;
    proto::React react;
    react.scope      = *scope;
    react.syncAnswer = message.value("y") == "1";
    const QByteArray list = message.value("i");
    if (list.isEmpty())
        return std::nullopt;
    const QList<QByteArray> items = list.split(',');
    if (items.size() > proto::kMaxItems)
        return std::nullopt;
    for (const QByteArray& item : items) {
        const int colon = item.indexOf(':');
        if (colon < 0)
            return std::nullopt;
        const QByteArray ref  = item.left(colon);
        quint8           mask = 0;
        if (!isWireRef(ref) || !proto::codesToMask(item.mid(colon + 1), &mask))
            return std::nullopt;
        const QString refText = QString::fromLatin1(ref);
        for (const proto::ReactItem& seen : qAsConst(react.items)) {
            if (seen.key == refText)
                return std::nullopt; // contradicting states in one message
        }
        react.items.append({refText, mask, proto::setFromMask(mask)});
    }
    // Whole sets for items of i= (all of e= is valid and belongs to i=, or none of it counts).
    const QByteArray extended = message.value("e");
    if (!extended.isEmpty()) {
        QHash<QString, proto::ReactionSet> sets;
        const QList<QByteArray>            parts = extended.split(',');
        bool                               valid = parts.size() <= proto::kMaxItems;
        for (int p = 0; valid && p < parts.size(); ++p) {
            const QByteArray&  part  = parts.at(p);
            const int          colon = part.indexOf(':');
            const QString      ref   = QString::fromLatin1(part.left(qMax(0, colon)));
            proto::ReactionSet set;
            valid = colon > 0 && isWireRef(part.left(colon)) && !sets.contains(ref) && proto::emojiCodesToSet(part.mid(colon + 1), &set);
            if (valid)
                sets.insert(ref, set);
        }
        for (auto it = sets.constBegin(); valid && it != sets.constEnd(); ++it) {
            bool listed = false;
            for (const proto::ReactItem& item : qAsConst(react.items))
                listed = listed || item.key == it.key();
            valid = listed;
        }
        if (valid) {
            for (proto::ReactItem& item : react.items) {
                const auto found = sets.constFind(item.key);
                if (found == sets.constEnd())
                    continue;
                item.set  = proto::cleanSet(found.value());
                item.mask = proto::legacyMask(item.set);
            }
        }
    }
    return react;
}

QVector<proto::Message> makeReacts(proto::Scope scope, bool syncAnswer, const QVector<proto::ReactItem>& items)
{
    QVector<proto::Message> out;
    proto::Message          base;
    base.type = "TR";
    base.set("s", scopeText(scope));
    if (syncAnswer)
        base.set("y", "1");
    const int baseBytes = proto::serialize(base).size() + 3; // " i=" / " e=" and the lists

    QByteArray list;
    QByteArray extended;
    int        count = 0;
    auto       flush = [&] {
        if (count == 0)
            return;
        proto::Message m = base;
        m.set("i", list);
        if (!extended.isEmpty())
            m.set("e", extended);
        out.append(m);
        list.clear();
        extended.clear();
        count = 0;
    };
    for (const proto::ReactItem& item : items) {
        const QByteArray ref = item.key.toLatin1();
        if (!isWireRef(ref))
            continue;
        const proto::ReactionSet set   = proto::cleanSet(item.reactions());
        const quint8             mask  = proto::legacyMask(set);
        const QByteArray         text  = ref + ':' + proto::maskToCodes(mask);
        const bool               other = !set.isEmpty() && proto::setFromMask(mask).size() != set.size();
        const QByteArray         more  = other ? ref + ':' + proto::setToEmojiCodes(set) : QByteArray();
        const int                added = text.size() + (count > 0 ? 1 : 0);
        const int addedMore            = more.isEmpty() ? 0 : more.size() + (extended.isEmpty() ? 3 : 1);
        if (count > 0
            && (count >= proto::kMaxItems || list.size() + added > proto::kMaxValueChars || extended.size() + addedMore > proto::kMaxValueChars
                || baseBytes + list.size() + added + extended.size() + addedMore > proto::kMaxSendBytes))
            flush();
        if (!list.isEmpty())
            list += ',';
        list += text;
        if (!more.isEmpty()) {
            if (!extended.isEmpty())
                extended += ',';
            extended += more;
        }
        ++count;
    }
    flush();
    return out;
}

std::optional<proto::Sync> readSync(const proto::Message& message)
{
    if (message.type != "TSYNC")
        return std::nullopt;
    const std::optional<proto::Scope> scope = scopeOf(message);
    if (!scope)
        return std::nullopt;
    proto::Sync sync;
    sync.scope            = *scope;
    const QByteArray list = message.value("m");
    if (list.isEmpty())
        return std::nullopt;
    const QList<QByteArray> refs = list.split(',');
    if (refs.size() > proto::kMaxItems)
        return std::nullopt;
    for (const QByteArray& ref : refs) {
        if (!isWireRef(ref))
            return std::nullopt;
        const QString text = QString::fromLatin1(ref);
        if (!sync.keys.contains(text))
            sync.keys.append(text);
    }
    return sync;
}

proto::Message makeSync(proto::Scope scope, const QStringList& refs)
{
    proto::Message m;
    m.type = "TSYNC";
    m.set("s", scopeText(scope));
    QByteArray  list;
    QStringList used;
    for (const QString& ref : refs) {
        if (used.size() >= proto::kMaxItems)
            break;
        const QByteArray bytes = ref.toLatin1();
        if (!isWireRef(bytes) || used.contains(ref) || list.size() + bytes.size() + 1 > proto::kMaxValueChars)
            continue;
        used.append(ref);
        if (!list.isEmpty())
            list += ',';
        list += bytes;
    }
    if (!list.isEmpty())
        m.set("m", list);
    return m;
}

// ---- reading chats again ------------------------------------------------------------------------------

QVector<int> carryOver(const QVector<quint64>& before, const QVector<quint64>& now, bool* moved)
{
    QVector<int> out(now.size(), -1);
    bool         anyMoved = false;
    QHash<quint64, int> bySignature; // built when first needed
    bool                built = false;
    for (int n = 0; n < now.size(); ++n) {
        const quint64 sig = now.at(n);
        if (sig == 0)
            continue;
        if (n < before.size() && before.at(n) == sig) {
            out[n] = n;
            continue;
        }
        if (!built) {
            bySignature.reserve(before.size());
            for (int i = before.size() - 1; i >= 0; --i) { // the first of equal ones wins
                if (before.at(i) != 0)
                    bySignature.insert(before.at(i), i);
            }
            built = true;
        }
        const auto found = bySignature.constFind(sig);
        if (found != bySignature.constEnd()) {
            out[n]   = found.value();
            anyMoved = true;
        }
    }
    if (moved)
        *moved = anyMoved;
    return out;
}

int movedUp(const QVector<int>& from)
{
    QHash<int, int> votes; // how far up -> how many readings moved that far
    for (int n = 0; n < from.size(); ++n) {
        if (from.at(n) >= 0)
            ++votes[from.at(n) - n];
    }
    int best = -1, count = 0;
    for (auto it = votes.constBegin(); it != votes.constEnd(); ++it) {
        if (it.value() > count || (it.value() == count && it.key() < best)) {
            best  = it.key();
            count = it.value();
        }
    }
    return count > 0 ? qMax(0, best) : -1;
}

QVector<QDate> keepDays(QVector<QDate> now, const QVector<QDate>& kept, int up)
{
    if (up < 0)
        return now;
    for (int n = 0; n < now.size() && n + up < kept.size(); ++n) {
        if (!now.at(n).isValid())
            now[n] = kept.at(n + up);
    }
    return now;
}

ChatKind kindOfTab(int index)
{
    if (index < 0)
        return ChatKind::Unknown;
    return index == 0 ? ChatKind::Server : index == 1 ? ChatKind::Channel : ChatKind::Private;
}

// ---- the index ----------------------------------------------------------------------------------------

void Index::setChat(quintptr id, const IndexedChat& chat)
{
    removeChat(id);
    m_chats.insert(id, chat);
    const QVector<IndexedMessage>& messages = m_chats.value(id).messages;
    for (int i = 0; i < messages.size(); ++i)
        m_byHash[messages.at(i).hash].append({id, i});
}

void Index::removeChat(quintptr id)
{
    const auto found = m_chats.constFind(id);
    if (found == m_chats.constEnd())
        return;
    QSet<QString> hashes;
    for (const IndexedMessage& m : found->messages)
        hashes.insert(m.hash);
    for (const QString& hash : qAsConst(hashes)) {
        auto list = m_byHash.find(hash);
        if (list == m_byHash.end())
            continue;
        list->erase(std::remove_if(list->begin(), list->end(), [id](const QPair<quintptr, int>& at) { return at.first == id; }), list->end());
        if (list->isEmpty())
            m_byHash.erase(list);
    }
    m_chats.remove(id);
}

void Index::clear()
{
    m_chats.clear();
    m_byHash.clear();
}

int Index::messageCount() const
{
    int n = 0;
    for (const IndexedChat& chat : m_chats)
        n += chat.messages.size();
    return n;
}

QString Index::resolve(const Query& q) const
{
    if (!isHash(q.hash) || q.reactorUid.isEmpty())
        return {};
    const auto found = m_byHash.constFind(q.hash);
    if (found == m_byHash.constEnd())
        return {};
    qint64 bestDiff = -1;
    qint64 bestTime = 0;
    for (const QPair<quintptr, int>& at : found.value()) {
        const auto chat = m_chats.constFind(at.first);
        if (chat == m_chats.constEnd() || at.second < 0 || at.second >= chat->messages.size())
            continue;
        if (!chat->serverUid.isEmpty() && !q.serverUid.isEmpty() && chat->serverUid != q.serverUid)
            continue;
        if (chat->kind == ChatKind::Server)
            continue;
        const IndexedMessage& m = chat->messages.at(at.second);
        if (q.scope == proto::Scope::Channel) {
            if (chat->kind == ChatKind::Private)
                continue;
        } else {
            // Anyone on the server can address a private plugin command to us, so s=p alone proves nothing.
            if (chat->kind == ChatKind::Channel)
                continue;
            if (chat->kind == ChatKind::Private && !chat->partnerUid.isEmpty() && chat->partnerUid != q.reactorUid)
                continue;
            if (m.sender != q.reactorUid) {
                // Ours: only in the private chat known to be with the reactor (a chat not identified yet may
                // be the channel's, or a private one with someone else).
                if (q.ownUid.isEmpty() || m.sender != q.ownUid || chat->kind != ChatKind::Private || chat->partnerUid != q.reactorUid)
                    continue;
            }
            // Theirs: a private chat holds the partner's messages and ours only, so a message of the reactor
            // in a private chat (or one not identified yet) is in their chat with us.
        }
        const qint64 diff = qAbs(m.timeSec - q.estimatedSec);
        if (diff > q.toleranceSec)
            continue;
        // The nearest; between two as near, the newer one.
        if (bestDiff < 0 || diff < bestDiff || (diff == bestDiff && m.timeSec > bestTime)) {
            bestDiff = diff;
            bestTime = m.timeSec;
        }
    }
    return bestDiff < 0 ? QString() : localKey(q.hash, bestTime);
}

QStringList Index::recentChannelKeys(const QString& serverUid, int max) const
{
    QVector<QPair<qint64, QString>> all;
    for (const IndexedChat& chat : m_chats) {
        if (chat.kind != ChatKind::Channel || (!serverUid.isEmpty() && chat.serverUid != serverUid))
            continue;
        for (const IndexedMessage& m : chat.messages)
            all.append({m.timeSec, localKey(m.hash, m.timeSec)});
    }
    std::sort(all.begin(), all.end(), [](const QPair<qint64, QString>& a, const QPair<qint64, QString>& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    QStringList out;
    for (const auto& item : qAsConst(all)) {
        if (out.size() >= max)
            break;
        if (!item.second.isEmpty() && !out.contains(item.second))
            out.append(item.second);
    }
    return out;
}

bool Index::knows(const QString& key) const
{
    QString hash;
    qint64  t = 0;
    if (!parseLocalKey(key, &hash, &t))
        return false;
    const auto found = m_byHash.constFind(hash);
    if (found == m_byHash.constEnd())
        return false;
    for (const QPair<quintptr, int>& at : found.value()) {
        const auto chat = m_chats.constFind(at.first);
        if (chat != m_chats.constEnd() && at.second >= 0 && at.second < chat->messages.size() && chat->messages.at(at.second).timeSec == t)
            return true;
    }
    return false;
}

} // namespace textreact
