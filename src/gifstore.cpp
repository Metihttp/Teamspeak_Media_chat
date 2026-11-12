#include "gifstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

#include "linkpreview/textclean.h"

namespace gifs {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

QLatin1String k(const char* key)
{
    return QLatin1String(key);
}

// The keys a known item uses; everything else is kept in Item::extra.
const char* const kItemKeys[] = {"id",  "src",  "kind", "how", "url", "ukey", "pid",   "anim",  "as",   "name",
                                 "file", "sha", "sp",   "size", "w",  "h",    "d",     "thumb", "added", "used"};

bool isItemKey(const QString& key)
{
    for (const char* known : kItemKeys) {
        if (key == QLatin1String(known))
            return true;
    }
    return false;
}

std::optional<Source> sourceOf(const QString& text)
{
    if (text == QLatin1String("link"))
        return Source::Link;
    if (text == QLatin1String("file"))
        return Source::File;
    return std::nullopt;
}

std::optional<Kind> kindOf(const QString& text)
{
    if (text == QLatin1String("gif"))
        return Kind::Gif;
    if (text == QLatin1String("webp"))
        return Kind::Webp;
    if (text == QLatin1String("loop"))
        return Kind::Loop;
    return std::nullopt;
}

// An integer JSON value within lo..hi; nullopt for anything else (a string, a fraction, out of range).
std::optional<qint64> integerIn(const QJsonValue& value, qint64 lo, qint64 hi)
{
    if (!value.isDouble())
        return std::nullopt;
    const double d = value.toDouble();
    if (!std::isfinite(d) || d != std::floor(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi))
        return std::nullopt;
    return static_cast<qint64>(d);
}

// An optional integer: absent means fallback.
std::optional<qint64> optionalIn(const QJsonObject& o, const char* key, qint64 lo, qint64 hi, qint64 fallback)
{
    const QJsonValue v = o.value(k(key));
    if (v.isUndefined() || v.isNull())
        return fallback;
    return integerIn(v, lo, hi);
}

} // namespace

bool storableUrl(const QString& url)
{
    if (url.isEmpty() || url.size() > kMaxUrlChars)
        return false;
    if (!url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) && !url.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        return false;
    for (const QChar ch : url) {
        const ushort u = ch.unicode();
        if (u <= 0x20 || u == 0x7f || (u >= 0x80 && u <= 0x9f) || ch == QLatin1Char('[') || ch == QLatin1Char(']') || ch == QLatin1Char('"')
            || ch.isSpace() || ch.category() == QChar::Other_Format || ch.category() == QChar::Other_Control || ch.category() == QChar::Separator_Line
            || ch.category() == QChar::Separator_Paragraph)
            return false;
    }
    return true;
}

bool isItemId(const QString& id)
{
    if (id.size() != 16)
        return false;
    for (const QChar ch : id) {
        const ushort u = ch.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f')))
            return false;
    }
    return true;
}

bool isOwnFileName(const QString& name, const QString& id, std::initializer_list<const char*> exts)
{
    if (!isItemId(id) || name.size() < 18 || !name.startsWith(id) || name.at(16) != QLatin1Char('.'))
        return false;
    const QString ext = name.mid(17);
    for (const char* allowed : exts) {
        if (ext == QLatin1String(allowed))
            return true;
    }
    return false;
}

bool isOurPattern(const QString& fileName)
{
    QString name = fileName;
    if (name.endsWith(QLatin1String(".part")))
        name.chop(5);
    return isOwnFileName(name, name.left(16), {"jpg", "png", "gif", "webp", "mp4"});
}

namespace {

// ".gif" of "party.gif" (at most 8 letters and digits after the last dot, the dot not first); empty: none.
QString extensionOf(const QString& name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || name.size() - dot - 1 < 1 || name.size() - dot - 1 > 8)
        return {};
    for (int i = dot + 1; i < name.size(); ++i) {
        if (!name.at(i).isLetterOrNumber() || name.at(i).unicode() > 0x7f)
            return {};
    }
    return name.mid(dot);
}

} // namespace

QString itemName(const QString& displayName)
{
    const QString whole = linkpreview::textclean::clean(displayName, 4 * kMaxNameChars);
    QString       out   = whole;
    if (whole.size() > kMaxNameChars) {
        const QString ext  = extensionOf(whole);
        const QString base = whole.left(whole.size() - ext.size());
        out                = linkpreview::textclean::clean(base, kMaxNameChars - ext.size()) + ext;
    }
    if (out.trimmed().isEmpty() || out.size() > kMaxNameChars)
        return QString::fromLatin1("GIF.gif");
    return out;
}

QString remoteNameFor(const QString& name, const QString& file)
{
    const QString ext  = extensionOf(file).toLower(); // ".gif" | ".webp": what the copy is (by its bytes, 4.2)
    const QString own  = extensionOf(name);
    QString       base = own.isEmpty() ? name : name.left(name.size() - own.size());
    if (base.trimmed().isEmpty())
        base = QString::fromLatin1("GIF");
    return base + (ext.isEmpty() ? QString::fromLatin1(".gif") : ext);
}

QString shaText(const QByteArray& sha)
{
    if (sha.size() != 32)
        return {};
    return QString::fromLatin1(sha.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray shaFromText(const QString& text)
{
    if (text.size() != 43)
        return {};
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (!((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_'))
            return {};
    }
    const QByteArray sha = QByteArray::fromBase64(text.toLatin1(), QByteArray::Base64UrlEncoding);
    return sha.size() == 32 ? sha : QByteArray();
}

const char* kindName(Kind kind)
{
    switch (kind) {
    case Kind::Gif:
        return "gif";
    case Kind::Webp:
        return "webp";
    case Kind::Loop:
        return "loop";
    }
    return "gif";
}

// ---- Store ------------------------------------------------------------------------------------------------------

Store::Store(const Limits& limits)
    : m_limits(limits)
{
}

bool Store::valid(const Item& item, const Limits& limits)
{
    if (!isItemId(item.id))
        return false;
    if (!item.thumb.isEmpty() && !isOwnFileName(item.thumb, item.id, {"jpg", "png"}))
        return false;
    if (item.width < 0 || item.width > kMaxDimension || item.height < 0 || item.height > kMaxDimension || item.durationMs < 0
        || item.durationMs > kMaxDurationMs)
        return false;
    if (item.source == Source::Link) {
        if (!storableUrl(item.url) || item.kind == Kind::Webp || item.size < 0 || item.size > kMaxLinkSize)
            return false;
        if (!item.anim.isEmpty() && !isOwnFileName(item.anim, item.id, {"gif", "webp", "mp4"}))
            return false;
        return item.animSize >= 0 && item.animSize <= limits.maxAnimCopyBytes;
    }
    if (item.kind == Kind::Loop || !isOwnFileName(item.file, item.id, {"gif", "webp"}) || item.sha.size() != 32)
        return false;
    if (item.name.isEmpty() || item.name.size() > kMaxNameChars)
        return false;
    return item.size > 0 && item.size <= limits.maxCopyBytes;
}

int Store::indexOf(const QString& id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).id == id)
            return i;
    }
    return -1;
}

const Item* Store::find(const QString& id) const
{
    const int i = indexOf(id);
    return i >= 0 ? &m_items.at(i) : nullptr;
}

const Item* Store::findLink(const QString& urlKey, const QString& pid) const
{
    if (!urlKey.isEmpty()) {
        for (const Item& item : m_items) {
            if (item.source == Source::Link && item.urlKey == urlKey)
                return &item;
        }
    }
    if (!pid.isEmpty()) {
        for (const Item& item : m_items) {
            if (item.source == Source::Link && item.pid == pid)
                return &item;
        }
    }
    return nullptr;
}

const Item* Store::findFile(const QByteArray& sha) const
{
    if (sha.size() != 32)
        return nullptr;
    for (const Item& item : m_items) {
        if (item.source == Source::File && item.sha == sha)
            return &item;
    }
    return nullptr;
}

QVector<Item> Store::items() const
{
    return m_items;
}

QString Store::newId() const
{
    for (;;) {
        const quint64 value = QRandomGenerator::system()->generate64();
        const QString id    = QString::number(value, 16).rightJustified(16, QLatin1Char('0'));
        bool          taken = indexOf(id) >= 0;
        for (const QJsonObject& raw : m_hidden)
            taken = taken || raw.value(k("id")).toString() == id;
        if (!taken)
            return id;
    }
}

void Store::evict(QStringList* evicted)
{
    while (m_items.size() > m_limits.maxItems) {
        // The least recently used: m_items is newest first, ties in array order, so the last one.
        int oldest = m_items.size() - 1;
        for (int i = m_items.size() - 1; i >= 0; --i) {
            if (m_items.at(i).usedSec < m_items.at(oldest).usedSec)
                oldest = i;
        }
        if (evicted)
            evicted->append(m_items.at(oldest).id);
        m_items.removeAt(oldest);
    }
}

QString Store::add(Item item, qint64 nowSec, QStringList* evicted)
{
    if (m_readOnly)
        return {};
    const Item* existing = item.source == Source::Link ? findLink(item.urlKey, item.pid) : findFile(item.sha);
    if (existing) {
        Item updated = *existing;
        if (item.source == Source::Link && storableUrl(item.url))
            updated.url = item.url; // the newest form of the link
        if (item.source == Source::Link && !item.urlKey.isEmpty())
            updated.urlKey = item.urlKey;
        if (item.size > 0)
            updated.size = item.size;
        if (item.width > 0 && item.height > 0) {
            updated.width  = item.width;
            updated.height = item.height;
        }
        if (!item.thumb.isEmpty() && isOwnFileName(item.thumb, updated.id, {"jpg", "png"}))
            updated.thumb = item.thumb;
        updated.usedSec = nowSec;
        const int at    = indexOf(updated.id);
        m_items.removeAt(at);
        m_items.prepend(updated);
        return updated.id;
    }
    if (item.id.isEmpty())
        item.id = newId();
    if (item.addedSec <= 0)
        item.addedSec = nowSec;
    item.usedSec = nowSec;
    if (!valid(item, m_limits) || indexOf(item.id) >= 0)
        return {};
    m_items.prepend(item);
    evict(evicted);
    return item.id;
}

bool Store::touch(const QString& id, qint64 nowSec)
{
    if (m_readOnly)
        return false;
    const int at = indexOf(id);
    if (at < 0)
        return false;
    Item item    = m_items.takeAt(at);
    item.usedSec = nowSec;
    m_items.prepend(item);
    return true;
}

bool Store::update(const Item& item)
{
    if (m_readOnly)
        return false;
    const int at = indexOf(item.id);
    if (at < 0 || !valid(item, m_limits))
        return false;
    m_items[at] = item;
    return true;
}

std::optional<Item> Store::remove(const QString& id)
{
    if (m_readOnly)
        return std::nullopt;
    const int at = indexOf(id);
    if (at < 0)
        return std::nullopt;
    return m_items.takeAt(at);
}

void Store::clear()
{
    m_items.clear();
    m_hidden.clear();
}

qint64 Store::copyBytes() const
{
    qint64 total = 0;
    for (const Item& item : m_items)
        total += item.source == Source::File ? item.size : (item.anim.isEmpty() ? 0 : item.animSize);
    return total;
}

QStringList Store::overBudget() const
{
    QStringList out;
    qint64      total = copyBytes();
    if (total <= m_limits.maxTotalCopyBytes)
        return out;
    // Least recently used first (the list is newest first, ties in array order: walk it backwards, stable by used).
    QVector<int> order;
    for (int i = 0; i < m_items.size(); ++i)
        order.append(i);
    // Among equal times the later entry is the older one.
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        if (m_items.at(a).usedSec != m_items.at(b).usedSec)
            return m_items.at(a).usedSec < m_items.at(b).usedSec;
        return a > b;
    });
    for (int i : qAsConst(order)) {
        if (total <= m_limits.maxTotalCopyBytes)
            break;
        const Item& item  = m_items.at(i);
        const qint64 bytes = item.source == Source::File ? item.size : (item.anim.isEmpty() ? 0 : item.animSize);
        if (bytes <= 0)
            continue;
        out.append(item.id);
        total -= bytes;
    }
    return out;
}

QSet<QString> Store::namedFiles() const
{
    QSet<QString> out;
    for (const Item& item : m_items) {
        for (const QString& name : {item.file, item.thumb, item.anim}) {
            if (!name.isEmpty())
                out.insert(name);
        }
    }
    for (const QJsonObject& raw : m_hidden) {
        for (const char* key : {"file", "thumb", "anim"}) {
            const QJsonValue v = raw.value(k(key));
            if (v.isString() && !v.toString().isEmpty())
                out.insert(v.toString());
        }
    }
    return out;
}

QJsonObject Store::itemToJson(const Item& item)
{
    QJsonObject o = item.extra;
    o.insert(k("id"), item.id);
    o.insert(k("src"), item.source == Source::Link ? latin("link") : latin("file"));
    o.insert(k("kind"), latin(kindName(item.kind)));
    o.insert(k("how"), item.how == How::Sent ? latin("sent") : latin("saved"));
    if (item.source == Source::Link) {
        o.insert(k("url"), item.url);
        o.insert(k("ukey"), item.urlKey);
        if (!item.pid.isEmpty())
            o.insert(k("pid"), item.pid);
        if (!item.anim.isEmpty()) {
            o.insert(k("anim"), item.anim);
            o.insert(k("as"), static_cast<double>(item.animSize));
        }
    } else {
        o.insert(k("name"), item.name);
        o.insert(k("file"), item.file);
        o.insert(k("sha"), shaText(item.sha));
    }
    if (item.spoiler)
        o.insert(k("sp"), true);
    o.insert(k("size"), static_cast<double>(item.size));
    o.insert(k("w"), item.width);
    o.insert(k("h"), item.height);
    o.insert(k("d"), static_cast<double>(item.durationMs));
    o.insert(k("thumb"), item.thumb);
    o.insert(k("added"), static_cast<double>(item.addedSec));
    o.insert(k("used"), static_cast<double>(item.usedSec));
    return o;
}

QByteArray Store::toJson() const
{
    QJsonObject top = m_top;
    top.insert(k("format"), 1);
    QJsonArray list;
    for (const Item& item : m_items)
        list.append(itemToJson(item));
    for (const QJsonObject& raw : m_hidden)
        list.append(raw);
    top.insert(k("items"), list);
    return QJsonDocument(top).toJson(QJsonDocument::Compact);
}

Store::Load Store::fromJson(const QByteArray& input, qint64 nowSec, const LinkFactsOf& factsOf, LoadStats* stats)
{
    m_items.clear();
    m_hidden.clear();
    m_top      = QJsonObject();
    m_readOnly = false;
    LoadStats counts;
    const auto done = [&](Load load) {
        if (stats)
            *stats = counts;
        return load;
    };
    if (input.size() > m_limits.maxFileBytes)
        return done(Load::TooLarge);
    QByteArray bytes = input;
    if (bytes.startsWith("\xEF\xBB\xBF"))
        bytes.remove(0, 3); // one leading UTF-8 BOM (a hand edit)
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return done(Load::Corrupt);
    const QJsonObject top    = doc.object();
    const auto        format = integerIn(top.value(k("format")), LLONG_MIN / 2, LLONG_MAX / 2);
    if (!format || *format < 1)
        return done(Load::Corrupt);
    if (*format > 1) {
        m_readOnly = true; // a newer TS Media's list: nothing shown, recorded, saved or deleted
        return done(Load::Newer);
    }
    const QJsonValue listValue = top.value(k("items"));
    if (!listValue.isUndefined() && !listValue.isArray())
        return done(Load::Corrupt);
    for (auto it = top.constBegin(); it != top.constEnd(); ++it) {
        if (it.key() != QLatin1String("format") && it.key() != QLatin1String("items"))
            m_top.insert(it.key(), it.value());
    }

    QSet<QString> ids;
    for (const QJsonValue& value : listValue.toArray()) {
        if (!value.isObject()) {
            ++counts.dropped;
            continue;
        }
        const QJsonObject o  = value.toObject();
        const QString     id = o.value(k("id")).toString();
        if (!isItemId(id) || ids.contains(id)) {
            ++counts.dropped;
            continue;
        }
        const auto source = sourceOf(o.value(k("src")).toString());
        const auto kind   = kindOf(o.value(k("kind")).toString());
        if (!source || !kind) {
            // A newer version's entry: verbatim, never parsed further.
            ids.insert(id);
            m_hidden.append(o);
            ++counts.hidden;
            continue;
        }
        Item item;
        item.id      = id;
        item.source  = *source;
        item.kind    = *kind;
        item.how     = o.value(k("how")).toString() == QLatin1String("sent") ? How::Sent : How::Saved;
        item.spoiler = o.value(k("sp")).toBool(false);
        item.thumb   = o.value(k("thumb")).toString();
        const auto size  = optionalIn(o, "size", 0, item.source == Source::File ? m_limits.maxCopyBytes : kMaxLinkSize, -1);
        const auto w     = optionalIn(o, "w", 0, kMaxDimension, 0);
        const auto h     = optionalIn(o, "h", 0, kMaxDimension, 0);
        const auto d     = optionalIn(o, "d", 0, kMaxDurationMs, 0);
        const auto added = optionalIn(o, "added", LLONG_MIN / 2, LLONG_MAX / 2, 0);
        const auto used  = optionalIn(o, "used", LLONG_MIN / 2, LLONG_MAX / 2, 0);
        if (!size || !w || !h || !d || !added || !used) {
            ++counts.dropped;
            continue;
        }
        item.size       = *size < 0 ? 0 : *size;
        item.width      = static_cast<int>(*w);
        item.height     = static_cast<int>(*h);
        item.durationMs = *d;
        item.addedSec   = qBound<qint64>(0, *added, nowSec);
        item.usedSec    = qBound<qint64>(0, *used, nowSec);
        if (item.source == Source::Link) {
            item.url    = o.value(k("url")).toString();
            item.urlKey = o.value(k("ukey")).toString().left(64);
            item.pid    = o.value(k("pid")).toString().left(200);
            item.anim   = o.value(k("anim")).toString();
            const auto as = optionalIn(o, "as", 0, m_limits.maxAnimCopyBytes, 0);
            if (!as) {
                ++counts.dropped;
                continue;
            }
            item.animSize = item.anim.isEmpty() ? 0 : *as;
        } else {
            item.name = o.value(k("name")).toString();
            item.file = o.value(k("file")).toString();
            item.sha  = shaFromText(o.value(k("sha")).toString());
        }
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            if (!isItemKey(it.key()))
                item.extra.insert(it.key(), it.value());
        }
        if (!valid(item, m_limits)) {
            ++counts.dropped;
            continue;
        }
        if (item.source == Source::Link && factsOf) {
            const LinkFacts facts = factsOf(item.url);
            if (facts.key != item.urlKey && !facts.key.isEmpty()) {
                item.urlKey = facts.key;
                ++counts.rekeyed;
            }
            if (!facts.media) {
                item.plain = true;
                ++counts.plain;
            } else if (!facts.pid.isEmpty()) {
                item.pid = facts.pid;
            }
        }
        ids.insert(id);
        m_items.append(item);
    }

    // Re-keyed items may now collide (same key, or same provider id): the more recently used one stays; the same for two
    // copies of one file.
    for (int i = 0; i < m_items.size(); ++i) {
        for (int j = i + 1; j < m_items.size();) {
            Item&       a    = m_items[i];
            const Item& b    = m_items.at(j);
            const bool  same = a.source == b.source
                              && (a.source == Source::Link ? ((!a.urlKey.isEmpty() && a.urlKey == b.urlKey) || (!a.pid.isEmpty() && a.pid == b.pid))
                                                            : a.sha == b.sha);
            if (!same) {
                ++j;
                continue;
            }
            // A thumbnail is named after its own item, so the other's can't be taken over: it becomes an orphan for the
            // sweep, and the kept item gets a new one the next time its link is loaded.
            if (b.usedSec > a.usedSec)
                a = b;
            m_items.removeAt(j);
            ++counts.dropped;
        }
    }
    std::stable_sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) { return a.usedSec > b.usedSec; });
    counts.items = m_items.size();
    return done(Load::Ok);
}

} // namespace gifs
