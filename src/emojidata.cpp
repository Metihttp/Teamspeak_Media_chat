#include "emojidata.h"

#include <QHash>
#include <QSet>

#include <algorithm>

#include "i18n.h"

namespace emoji {

namespace {

struct Row {
    const char* text;
    const char* name;
    int         group;
    int         flags;
    const char* words;
};

#include "emojitable.inc"

constexpr int kRowCount    = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
constexpr int kTextDefault = 1;
constexpr int kHasTones    = 2;

struct Index {
    QVector<QString>        texts;
    QVector<QString>        names;  // tone variants: empty (built from the base)
    QVector<QStringList>    words;  // name words and search words, lower case
    QHash<QString, int>     byKey;
    QVector<QVector<int>>   members;
    QSet<uint>              starts;
    int                     maxKeyUnits = 0;
};

QStringList splitWords(const QString& text)
{
    QStringList out;
    QString     word;
    for (const QChar ch : text) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('+') || ch == QLatin1Char('\'') || ch.unicode() == 0x2019) {
            word += ch.toLower();
        } else if (!word.isEmpty()) {
            out.append(word);
            word.clear();
        }
    }
    if (!word.isEmpty())
        out.append(word);
    return out;
}

Index build()
{
    Index index;
    index.texts.reserve(kRowCount);
    index.names.reserve(kRowCount);
    index.words.reserve(kRowCount);
    index.members.resize(kGroupCount);
    for (int i = 0; i < kRowCount; ++i) {
        const Row& row = kRows[i];
        index.texts.append(QString::fromUtf8(row.text));
        index.names.append(QString::fromUtf8(row.name));
        QStringList words = splitWords(index.names.last());
        // Shortcodes are written with '_' ("thumbs_up"): searchable as a whole and by part.
        for (const QString& word : QString::fromUtf8(row.words).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            words.append(word.toLower());
            if (word.contains(QLatin1Char('_')))
                words << word.toLower().split(QLatin1Char('_'), Qt::SkipEmptyParts);
        }
        index.words.append(words);
        const QString key = lookupKey(index.texts.last());
        if (!key.isEmpty() && !index.byKey.contains(key))
            index.byKey.insert(key, i);
        if (!key.isEmpty()) {
            const QChar first = key.at(0);
            index.starts.insert(first.isHighSurrogate() && key.size() > 1 ? QChar::surrogateToUcs4(first, key.at(1)) : first.unicode());
            index.maxKeyUnits = qMax(index.maxKeyUnits, key.size());
        }
        if ((row.flags >> 2) == 0 && row.group >= 0 && row.group < kGroupCount)
            index.members[row.group].append(i);
    }
    return index;
}

const Index& data()
{
    static const Index index = build();
    return index;
}

struct Emoticon {
    const char* file;
    const char* text; // UTF-8
    const char* code;
};

// TeamSpeak's icon packs (gfx/*.zip, emoticons/emoticons.txt): ten emoticons, "<file> = <code>". The
// emoji each one stands for.
const Emoticon kEmoticons[] = {
    {"smile", "\xF0\x9F\x99\x82", ":)"},     // 🙂 slightly smiling face
    {"laugh", "\xF0\x9F\x98\x83", ":D"},     // 😃 grinning face with big eyes
    {"cool", "\xF0\x9F\x98\x8E", "8)"},      // 😎
    {"twinkle", "\xF0\x9F\x98\x89", ";)"},   // 😉
    {"sad", "\xF0\x9F\x99\x81", ":("},       // 🙁 slightly frowning face
    {"angry", "\xF0\x9F\x98\xA0", ":C"},     // 😠
    {"scream", "\xF0\x9F\x98\xB2", ":0"},    // 😲 astonished face
    {"skeptical", "\xF0\x9F\x98\x95", ":/"}, // 😕 confused face
    {"stunned", "\xF0\x9F\xA4\x90", ":x"},   // 🤐 zipper-mouth face (the pack draws an X for a mouth)
    {"tongue", "\xF0\x9F\x98\x9B", ":P"},    // 😛
};

const Emoticon* emoticonFor(const QString& imageName)
{
    if (!imageName.startsWith(QLatin1String("emoticons:"), Qt::CaseInsensitive) || imageName.size() > 200)
        return nullptr;
    QString file = imageName.mid(10);
    const int slash = qMax(file.lastIndexOf(QLatin1Char('/')), file.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        file = file.mid(slash + 1);
    const int dot = file.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        file = file.left(dot);
    for (const Emoticon& e : kEmoticons) {
        if (file.compare(QLatin1String(e.file), Qt::CaseInsensitive) == 0)
            return &e;
    }
    return nullptr;
}

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

} // namespace

int count()
{
    return kRowCount;
}

bool isValid(int id)
{
    return id >= 0 && id < kRowCount;
}

QString text(int id)
{
    return isValid(id) ? data().texts.at(id) : QString();
}

QString name(int id)
{
    if (!isValid(id))
        return {};
    const int t = tone(id);
    if (t > 0)
        return i18n::t("%1: %2").arg(data().names.at(baseOf(id)), toneName(t));
    return data().names.at(id);
}

QString keywords(int id)
{
    return isValid(id) ? QString::fromUtf8(kRows[baseOf(id)].words) : QString();
}

Group group(int id)
{
    return isValid(id) ? static_cast<Group>(qBound(0, kRows[id].group, kGroupCount - 1)) : Group::Symbols;
}

bool isTextDefault(int id)
{
    return isValid(id) && (kRows[id].flags & kTextDefault);
}

bool hasTones(int id)
{
    return isValid(id) && (kRows[id].flags & kHasTones) && id + kToneCount < kRowCount;
}

int tone(int id)
{
    return isValid(id) ? qBound(0, kRows[id].flags >> 2, kToneCount) : 0;
}

int baseOf(int id)
{
    const int t = tone(id);
    return t > 0 ? id - t : id;
}

int withTone(int id, int wanted)
{
    if (!isValid(id))
        return -1;
    const int base = baseOf(id);
    if (!hasTones(base))
        return id;
    if (wanted <= 0 || wanted > kToneCount)
        return base;
    return base + wanted;
}

QString toneName(int t)
{
    switch (t) {
    case 1:
        return i18n::t("light skin tone");
    case 2:
        return i18n::t("medium-light skin tone");
    case 3:
        return i18n::t("medium skin tone");
    case 4:
        return i18n::t("medium-dark skin tone");
    case 5:
        return i18n::t("dark skin tone");
    default:
        return {};
    }
}

QVector<int> members(Group g)
{
    const int index = static_cast<int>(g);
    return index >= 0 && index < kGroupCount ? data().members.at(index) : QVector<int>();
}

QString groupName(Group g)
{
    switch (g) {
    case Group::SmileysPeople:
        return i18n::t("Smileys & people");
    case Group::AnimalsNature:
        return i18n::t("Animals & nature");
    case Group::FoodDrink:
        return i18n::t("Food & drink");
    case Group::Activities:
        return i18n::t("Activities");
    case Group::TravelPlaces:
        return i18n::t("Travel & places");
    case Group::Objects:
        return i18n::t("Objects");
    case Group::Symbols:
        return i18n::t("Symbols");
    case Group::Flags:
        return i18n::t("Flags");
    }
    return {};
}

QString lookupKey(const QString& text)
{
    QString key;
    key.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.unicode() != 0xFE0F && ch.unicode() != 0xFE0E)
            key += ch;
    }
    return key;
}

int findKey(const QString& key)
{
    return data().byKey.value(key, -1);
}

int find(const QString& t)
{
    if (t.isEmpty() || t.size() > 64)
        return -1;
    return findKey(lookupKey(t));
}

bool mayStart(uint codePoint)
{
    return data().starts.contains(codePoint);
}

int maxKeyUnits()
{
    return data().maxKeyUnits;
}

QVector<int> search(const QString& query, int limit)
{
    QVector<int> out;
    QString      cleaned = query.trimmed().toLower();
    // ":thumbs_up:" as in chat apps.
    while (cleaned.startsWith(QLatin1Char(':')))
        cleaned.remove(0, 1);
    while (cleaned.endsWith(QLatin1Char(':')))
        cleaned.chop(1);
    cleaned.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (cleaned.isEmpty() || limit <= 0)
        return out;
    // An emoji typed or pasted into the search finds itself.
    const int direct = find(query.trimmed());
    QStringList wanted = splitWords(cleaned);
    if (wanted.isEmpty() && direct < 0) {
        // "+1", "-1", "100": shortcodes that aren't words
        wanted << cleaned;
    }
    const Index& index = data();
    struct Hit {
        int score;
        int id;
    };
    QVector<Hit> hits;
    if (direct >= 0)
        hits.append({-1, baseOf(direct)});
    const QString whole = wanted.join(QLatin1Char(' '));
    for (int id = 0; id < kRowCount && !wanted.isEmpty(); ++id) {
        if (tone(id) > 0 || id == (direct >= 0 ? baseOf(direct) : -2))
            continue;
        const QStringList& words = index.words.at(id);
        bool               all   = true;
        bool               exact = false;
        for (const QString& w : qAsConst(wanted)) {
            bool found = false;
            for (const QString& word : words) {
                if (word.startsWith(w)) {
                    found = true;
                    exact = exact || word == w;
                    break;
                }
            }
            if (!found) {
                all = false;
                break;
            }
        }
        if (!all)
            continue;
        const QString& nm    = index.names.at(id);
        int            score = 3;
        if (nm == whole)
            score = 0;
        else if (exact && wanted.size() == 1 && QString::fromUtf8(kRows[id].words).split(QLatin1Char(' ')).contains(wanted.first()))
            score = 1; // a shortcode typed in full ("joy", "lol")
        else if (nm.startsWith(whole))
            score = 2;
        hits.append({score, id});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score < b.score; });
    for (const Hit& hit : qAsConst(hits)) {
        if (out.size() >= limit)
            break;
        out.append(hit.id);
    }
    return out;
}

// ---- wire form ----------------------------------------------------------------------------------------

QByteArray wireCode(int id)
{
    if (!isValid(id))
        return {};
    const QString t = text(id);
    QByteArray    out;
    for (int i = 0; i < t.size(); ++i) {
        uint cp = t.at(i).unicode();
        if (t.at(i).isHighSurrogate() && i + 1 < t.size()) {
            cp = QChar::surrogateToUcs4(t.at(i), t.at(i + 1));
            ++i;
        }
        if (!out.isEmpty())
            out += '-';
        out += QByteArray::number(cp, 16);
    }
    return out;
}

bool isWireCode(const QByteArray& code)
{
    if (code.isEmpty() || code.size() > kMaxWireCodePoints * 7)
        return false;
    const QList<QByteArray> parts = code.split('-');
    if (parts.size() > kMaxWireCodePoints)
        return false;
    for (const QByteArray& part : parts) {
        if (part.isEmpty() || part.size() > 6 || part.startsWith('0'))
            return false;
        for (const char c : part) {
            if (!isHexDigit(c))
                return false;
        }
        const uint cp = part.toUInt(nullptr, 16);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
    }
    return true;
}

int fromWireCode(const QByteArray& code)
{
    if (!isWireCode(code))
        return -1;
    QString t;
    for (const QByteArray& part : code.split('-')) {
        const uint cp = part.toUInt(nullptr, 16);
        if (QChar::requiresSurrogates(cp)) {
            t += QChar(QChar::highSurrogate(cp));
            t += QChar(QChar::lowSurrogate(cp));
        } else {
            t += QChar(static_cast<ushort>(cp));
        }
    }
    return find(t);
}

// ---- TeamSpeak's emoticons --------------------------------------------------------------------------

int forTeamSpeakEmoticon(const QString& imageName)
{
    const Emoticon* e = emoticonFor(imageName);
    return e ? find(QString::fromUtf8(e->text)) : -1;
}

QString teamSpeakEmoticonCode(const QString& imageName)
{
    const Emoticon* e = emoticonFor(imageName);
    return e ? QString::fromLatin1(e->code) : QString();
}

QStringList teamSpeakEmoticonFiles()
{
    QStringList files;
    for (const Emoticon& e : kEmoticons)
        files.append(QString::fromLatin1(e.file));
    return files;
}

} // namespace emoji
