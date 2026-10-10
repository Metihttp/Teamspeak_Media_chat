#include "peerprotocol.h"

#include <QHash>

#include <cstring>

#include "emojidata.h" // 2.2 emoji

namespace proto {

namespace {

constexpr char kMagic[]   = "tsm1";
constexpr int  kMagicLen  = 4;
constexpr int  kMaxTypeLen = 8;
constexpr int  kMaxKeyLen  = 8;
constexpr int  kMaxCodeLen = 8;
constexpr int  kMaxCapLen  = 4;
constexpr int  kMaxCaps    = 8;

const char* const kCodes[kReactionCount] = {"up", "heart", "lol", "wow", "sad", "fire"};

bool isLower(char c)
{
    return c >= 'a' && c <= 'z';
}

bool isUpper(char c)
{
    return c >= 'A' && c <= 'Z';
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isHexLower(char c)
{
    return isDigit(c) || (c >= 'a' && c <= 'f');
}

bool isValueChar(char c)
{
    return isLower(c) || isUpper(c) || isDigit(c) || c == '.' || c == '_' || c == ',' || c == ':' || c == '-';
}

bool allOf(const QByteArray& text, bool (*pred)(char))
{
    for (const char c : text) {
        if (!pred(c))
            return false;
    }
    return true;
}

bool isType(const QByteArray& type)
{
    return !type.isEmpty() && type.size() <= kMaxTypeLen && allOf(type, isUpper);
}

bool isKey(const QByteArray& key)
{
    return !key.isEmpty() && key.size() <= kMaxKeyLen && allOf(key, isLower);
}

bool isValue(const QByteArray& value)
{
    return value.size() <= kMaxValueChars && allOf(value, isValueChar);
}

// "tsm" followed by digits only: someone's protocol of ours, another version.
bool isOurMagicFamily(const QByteArray& token)
{
    if (token.size() <= 3 || !token.startsWith("tsm"))
        return false;
    for (int i = 3; i < token.size(); ++i) {
        if (!isDigit(token.at(i)))
            return false;
    }
    return true;
}

bool isVersionText(const QByteArray& v)
{
    // 1-3 digits, up to 4 parts: "2.2.0"
    const QList<QByteArray> parts = v.split('.');
    if (parts.isEmpty() || parts.size() > 4)
        return false;
    for (const QByteArray& part : parts) {
        if (part.isEmpty() || part.size() > 3 || !allOf(part, isDigit))
            return false;
    }
    return true;
}

std::optional<Scope> readScope(const Message& m)
{
    const QByteArray s = m.value("s");
    if (s == "c")
        return Scope::Channel;
    if (s == "p")
        return Scope::Private;
    return std::nullopt; // missing, the server chat ("s") or anything else
}

QByteArray scopeText(Scope scope)
{
    return scope == Scope::Private ? QByteArray("p") : QByteArray("c");
}

QByteArray itemText(const ReactItem& item)
{
    return item.key.toLatin1() + ':' + maskToCodes(item.mask);
}

// 2.2 emoji: the v1 reactions as emoji (the order of kCodes).
const char* const kLegacyEmoji[kReactionCount] = {"1f44d", "2764-fe0f", "1f602", "1f62e", "1f622", "1f525"};

bool isEmojiCodeChar(char c)
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || c == '-';
}

} // namespace

// ---- reactions ------------------------------------------------------------------------------------

const char* reactionCode(int index)
{
    return index >= 0 && index < kReactionCount ? kCodes[index] : nullptr;
}

int reactionIndex(const QByteArray& code)
{
    for (int i = 0; i < kReactionCount; ++i) {
        if (code == kCodes[i])
            return i;
    }
    return -1;
}

QByteArray maskToCodes(quint8 mask)
{
    QByteArray out;
    for (int i = 0; i < kReactionCount; ++i) {
        if (!(mask & (1u << i)))
            continue;
        if (!out.isEmpty())
            out += '.';
        out += kCodes[i];
    }
    return out;
}

bool codesToMask(const QByteArray& codes, quint8* mask)
{
    *mask = 0;
    if (codes.isEmpty())
        return true;
    const QList<QByteArray> list = codes.split('.');
    if (list.size() > kMaxCodesPerItem)
        return false;
    quint8 result = 0;
    for (const QByteArray& code : list) {
        if (code.isEmpty() || code.size() > kMaxCodeLen || !allOf(code, isLower))
            return false;
        const int index = reactionIndex(code);
        if (index >= 0)
            result |= static_cast<quint8>(1u << index);
    }
    *mask = result;
    return true;
}

bool isMediaKey(const QByteArray& key)
{
    return key.size() == 20 && allOf(key, isHexLower);
}

// ---- 2.2 emoji ----------------------------------------------------------------------------------------

int legacyReactionEmoji(int index)
{
    if (index < 0 || index >= kReactionCount)
        return -1;
    return emoji::fromWireCode(QByteArray(kLegacyEmoji[index]));
}

int legacyReactionIndex(int emojiId)
{
    if (!emoji::isValid(emojiId))
        return -1;
    for (int i = 0; i < kReactionCount; ++i) {
        if (legacyReactionEmoji(i) == emojiId)
            return i;
    }
    return -1;
}

quint8 legacyMask(const ReactionSet& set)
{
    quint8 mask = 0;
    for (int id : set) {
        const int index = legacyReactionIndex(id);
        if (index >= 0)
            mask |= static_cast<quint8>(1u << index);
    }
    return mask;
}

ReactionSet setFromMask(quint8 mask)
{
    ReactionSet set;
    for (int i = 0; i < kReactionCount; ++i) {
        const int id = legacyReactionEmoji(i);
        if ((mask & (1u << i)) && id >= 0)
            set.append(id);
    }
    return set;
}

ReactionSet cleanSet(const ReactionSet& set)
{
    ReactionSet out;
    for (int id : set) {
        if (emoji::isValid(id) && !out.contains(id) && canAdd(out, id))
            out.append(id);
    }
    return out;
}

bool sameSet(const ReactionSet& a, const ReactionSet& b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a.at(i) != b.at(i))
            return false;
    }
    return true;
}

bool canAdd(const ReactionSet& set, int emojiId)
{
    if (!emoji::isValid(emojiId) || set.contains(emojiId) || set.size() >= kMaxReactionsPerSet)
        return false;
    ReactionSet more = set;
    more.append(emojiId);
    return setToEmojiCodes(more).size() <= kMaxSetCodeChars;
}

QByteArray setToEmojiCodes(const ReactionSet& set)
{
    QByteArray out;
    for (int id : set) {
        const QByteArray code = emoji::wireCode(id);
        if (code.isEmpty())
            continue;
        if (!out.isEmpty())
            out += '.';
        out += code;
    }
    return out;
}

bool emojiCodesToSet(const QByteArray& codes, ReactionSet* set)
{
    set->clear();
    if (codes.isEmpty())
        return true;
    if (codes.size() > kMaxValueChars)
        return false;
    const QList<QByteArray> list = codes.split('.');
    if (list.size() > kMaxCodesPerItem)
        return false;
    for (const QByteArray& code : list) {
        if (!allOf(code, isEmojiCodeChar) || !emoji::isWireCode(code))
            return false;
        const int id = emoji::fromWireCode(code);
        if (id >= 0 && !set->contains(id) && set->size() < kMaxReactionsPerSet)
            set->append(id);
    }
    return true;
}

bool isMediaKey(const QString& key)
{
    if (key.size() != 20)
        return false;
    for (const QChar ch : key) {
        if (ch.unicode() > 0x7f || !isHexLower(static_cast<char>(ch.unicode())))
            return false;
    }
    return true;
}

// ---- messages -------------------------------------------------------------------------------------

bool Message::has(const char* key) const
{
    for (const auto& field : fields) {
        if (field.first == key)
            return true;
    }
    return false;
}

QByteArray Message::value(const char* key) const
{
    for (const auto& field : fields) {
        if (field.first == key)
            return field.second;
    }
    return {};
}

void Message::set(const char* key, const QByteArray& value)
{
    for (auto& field : fields) {
        if (field.first == key) {
            field.second = value;
            return;
        }
    }
    fields.append({QByteArray(key), value});
}

bool looksLikeOurs(const char* raw)
{
    if (!raw)
        return false;
    // strnlen: never reads past kMaxRecvBytes + 1 bytes of an untrusted buffer.
    const size_t length = strnlen(raw, static_cast<size_t>(kMaxRecvBytes) + 1);
    if (length > static_cast<size_t>(kMaxRecvBytes) || length < static_cast<size_t>(kMagicLen))
        return false;
    return std::memcmp(raw, kMagic, kMagicLen) == 0 && (raw[kMagicLen] == ' ' || raw[kMagicLen] == '\0');
}

std::optional<Message> parse(const QByteArray& raw, ParseResult* result)
{
    ParseResult dummy;
    ParseResult& r = result ? *result : dummy;
    if (raw.isEmpty()) {
        r = ParseResult::Empty;
        return std::nullopt;
    }
    if (raw.size() > kMaxRecvBytes) {
        r = ParseResult::TooLong;
        return std::nullopt;
    }
    const int firstSpace = raw.indexOf(' ');
    const QByteArray magic = firstSpace < 0 ? raw : raw.left(firstSpace);
    if (magic != kMagic) {
        r = isOurMagicFamily(magic) ? ParseResult::NewerVersion : ParseResult::NotOurs;
        return std::nullopt;
    }
    // Printable ASCII and single spaces only (no tabs, controls, NUL, non-ASCII).
    for (int i = 0; i < raw.size(); ++i) {
        const char c = raw.at(i);
        if (c < 0x20 || c > 0x7e) {
            r = ParseResult::Malformed;
            return std::nullopt;
        }
    }
    if (raw.endsWith(' ') || raw.contains("  ")) {
        r = ParseResult::Malformed;
        return std::nullopt;
    }
    const QList<QByteArray> tokens = raw.split(' ');
    if (tokens.size() < 2 || tokens.size() > kMaxTokens || !isType(tokens.at(1))) {
        r = ParseResult::Malformed;
        return std::nullopt;
    }
    Message m;
    m.type = tokens.at(1);
    for (int i = 2; i < tokens.size(); ++i) {
        const QByteArray& token = tokens.at(i);
        const int         eq    = token.indexOf('=');
        if (eq <= 0) {
            r = ParseResult::Malformed;
            return std::nullopt;
        }
        const QByteArray key   = token.left(eq);
        const QByteArray value = token.mid(eq + 1);
        if (!isKey(key) || !isValue(value) || m.has(key.constData())) {
            r = ParseResult::Malformed;
            return std::nullopt;
        }
        m.fields.append({key, value});
    }
    r = ParseResult::Ok;
    return m;
}

QByteArray serialize(const Message& message)
{
    if (!isType(message.type))
        return {};
    QByteArray out = QByteArray(kMagic) + ' ' + message.type;
    if (message.fields.size() + 2 > kMaxTokens)
        return {};
    for (int i = 0; i < message.fields.size(); ++i) {
        const auto& field = message.fields.at(i);
        if (!isKey(field.first) || !isValue(field.second))
            return {};
        for (int j = 0; j < i; ++j) {
            if (message.fields.at(j).first == field.first)
                return {};
        }
        out += ' ' + field.first + '=' + field.second;
    }
    return out.size() <= kMaxSendBytes ? out : QByteArray();
}

// ---- typed messages -------------------------------------------------------------------------------

bool Hello::hasCap(const char* cap) const
{
    return caps.contains(QString::fromLatin1(cap));
}

std::optional<Hello> readHello(const Message& message)
{
    Hello hello;
    if (message.type == "HI")
        hello.hi = true;
    else if (message.type != "HELLO")
        return std::nullopt;

    // pv: the protocol majors the sender speaks; it must include ours.
    bool speaksOne = false;
    for (const QByteArray& pv : message.value("pv").split(',')) {
        if (pv.size() != 1 || !isDigit(pv.at(0)))
            return std::nullopt;
        speaksOne = speaksOne || pv == "1";
    }
    if (!speaksOne)
        return std::nullopt;

    const QByteArray v = message.value("v");
    if (isVersionText(v))
        hello.version = QString::fromLatin1(v);
    const QByteArray caps = message.value("caps");
    if (!caps.isEmpty()) {
        for (const QByteArray& cap : caps.split(',')) {
            if (cap.isEmpty() || cap.size() > kMaxCapLen || !allOf(cap, isLower))
                continue; // a malformed capability is just not understood
            const QString name = QString::fromLatin1(cap);
            if (!hello.caps.contains(name) && hello.caps.size() < kMaxCaps)
                hello.caps.append(name);
        }
    }
    hello.replyRequested = !hello.hi && message.value("rr") == "1";
    return hello;
}

namespace {
Message helloMessage(const char* type, const QString& version, const QStringList& caps)
{
    Message m;
    m.type = type;
    m.set("pv", "1");
    const QByteArray v = version.toLatin1();
    if (isVersionText(v))
        m.set("v", v);
    QByteArray list;
    for (const QString& cap : caps) {
        const QByteArray c = cap.toLatin1();
        if (c.isEmpty() || c.size() > kMaxCapLen || !allOf(c, isLower))
            continue;
        if (!list.isEmpty())
            list += ',';
        list += c;
    }
    if (!list.isEmpty())
        m.set("caps", list);
    return m;
}
} // namespace

Message makeHello(const QString& version, const QStringList& caps, bool replyRequested)
{
    Message m = helloMessage("HELLO", version, caps);
    if (replyRequested)
        m.set("rr", "1");
    return m;
}

Message makeHi(const QString& version, const QStringList& caps)
{
    return helloMessage("HI", version, caps);
}

Message makeBye()
{
    Message m;
    m.type = "BYE";
    return m;
}

std::optional<React> readReact(const Message& message)
{
    if (message.type != "R")
        return std::nullopt;
    const std::optional<Scope> scope = readScope(message);
    if (!scope)
        return std::nullopt;
    React react;
    react.scope      = *scope;
    react.syncAnswer = message.value("y") == "1";
    const QByteArray list = message.value("i");
    if (list.isEmpty())
        return std::nullopt;
    const QList<QByteArray> items = list.split(',');
    if (items.size() > kMaxItems)
        return std::nullopt;
    for (const QByteArray& item : items) {
        const int colon = item.indexOf(':');
        if (colon < 0)
            return std::nullopt;
        const QByteArray key = item.left(colon);
        quint8           mask = 0;
        if (!isMediaKey(key) || !codesToMask(item.mid(colon + 1), &mask))
            return std::nullopt;
        const QString keyText = QString::fromLatin1(key);
        for (const ReactItem& seen : qAsConst(react.items)) {
            if (seen.key == keyText)
                return std::nullopt; // contradicting states in one message
        }
        react.items.append({keyText, mask, setFromMask(mask)});
    }

    // 2.2 emoji: whole sets for items of i= (all of e= is valid, or none of it counts).
    const QByteArray extended = message.value("e");
    if (!extended.isEmpty()) {
        QHash<QString, ReactionSet> sets;
        const QList<QByteArray>     parts = extended.split(',');
        bool                        valid = parts.size() <= kMaxItems;
        for (int p = 0; valid && p < parts.size(); ++p) {
            const QByteArray& part    = parts.at(p);
            const int         colon   = part.indexOf(':');
            const QString     keyText = QString::fromLatin1(part.left(qMax(0, colon)));
            ReactionSet       set;
            valid = colon > 0 && isMediaKey(part.left(colon)) && !sets.contains(keyText) && emojiCodesToSet(part.mid(colon + 1), &set);
            if (valid)
                sets.insert(keyText, set);
        }
        if (valid) {
            for (ReactItem& item : react.items) {
                const auto found = sets.constFind(item.key);
                if (found == sets.constEnd())
                    continue;
                item.set  = cleanSet(found.value());
                item.mask = legacyMask(item.set);
            }
        }
    }
    return react;
}

QVector<Message> makeReacts(Scope scope, bool syncAnswer, const QVector<ReactItem>& items)
{
    QVector<Message> out;
    Message          base;
    base.type = "R";
    base.set("s", scopeText(scope));
    if (syncAnswer)
        base.set("y", "1");
    // Room for i= (and e=) in one command: the base message plus " i=" / " e=" and the lists.
    const int baseBytes = serialize(base).size() + 3;

    QByteArray list;
    QByteArray extended; // 2.2 emoji
    int        count = 0;
    auto       flush = [&] {
        if (count == 0)
            return;
        Message m = base;
        m.set("i", list);
        if (!extended.isEmpty())
            m.set("e", extended);
        out.append(m);
        list.clear();
        extended.clear();
        count = 0;
    };
    for (const ReactItem& item : items) {
        if (!isMediaKey(item.key))
            continue;
        // 2.2 emoji: the v1 part of the set in i=, the whole set in e= when it has other emoji.
        const ReactionSet set       = cleanSet(item.reactions());
        const quint8      mask      = legacyMask(set);
        const QByteArray  text      = itemText(ReactItem{item.key, mask, {}});
        const bool        other     = !set.isEmpty() && setFromMask(mask).size() != set.size();
        const QByteArray  more      = other ? item.key.toLatin1() + ':' + setToEmojiCodes(set) : QByteArray();
        const int         added     = text.size() + (count > 0 ? 1 : 0);
        const int         addedMore = more.isEmpty() ? 0 : more.size() + (extended.isEmpty() ? 3 : 1);
        if (count > 0
            && (count >= kMaxItems || list.size() + added > kMaxValueChars || extended.size() + addedMore > kMaxValueChars
                || baseBytes + list.size() + added + extended.size() + addedMore > kMaxSendBytes))
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

std::optional<Sync> readSync(const Message& message)
{
    if (message.type != "SYNC")
        return std::nullopt;
    const std::optional<Scope> scope = readScope(message);
    if (!scope)
        return std::nullopt;
    Sync sync;
    sync.scope = *scope;
    const QByteArray list = message.value("m");
    if (list.isEmpty())
        return std::nullopt;
    const QList<QByteArray> keys = list.split(',');
    if (keys.size() > kMaxItems)
        return std::nullopt;
    for (const QByteArray& key : keys) {
        if (!isMediaKey(key))
            return std::nullopt;
        const QString text = QString::fromLatin1(key);
        if (!sync.keys.contains(text))
            sync.keys.append(text);
    }
    return sync;
}

Message makeSync(Scope scope, const QStringList& keys)
{
    Message m;
    m.type = "SYNC";
    m.set("s", scopeText(scope));
    QByteArray  list;
    QStringList used;
    for (const QString& key : keys) {
        if (used.size() >= kMaxItems)
            break;
        if (!isMediaKey(key) || used.contains(key))
            continue;
        used.append(key);
        if (!list.isEmpty())
            list += ',';
        list += key.toLatin1();
    }
    if (!list.isEmpty())
        m.set("m", list);
    return m;
}

} // namespace proto
