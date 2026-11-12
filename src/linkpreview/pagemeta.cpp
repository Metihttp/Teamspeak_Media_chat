#include "pagemeta.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTextCodec>

#include <cmath>
#include <cstring>

#include "linkurl.h"
#include "netpolicy.h"
#include "providers.h"
#include "textclean.h"

namespace linkpreview::pagemeta {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

constexpr int kMaxTags          = 4096;
constexpr int kMaxNameChars     = 16;  // HTML tag names
constexpr int kMaxXmlNameChars  = 64;
constexpr int kMaxAttrs         = 64;
constexpr int kMaxAttrNameChars = 64;
constexpr int kMaxValueChars    = 4096;
constexpr int kMaxValueBytes    = 4 * kMaxValueChars;
constexpr int kMaxTitleBytes    = 4 * kMaxValueChars;
constexpr int kMaxXmlTextBytes  = 64 * 1024;
constexpr int kMaxWalkValueBytes = 128 * 1024; // walkTags: a share page's base64 states (shares.h: 64 KiB of JSON each)
constexpr int kPrescanBytes     = 8 * 1024;
constexpr int kUtf8CheckBytes   = 64 * 1024;

bool isWs(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool isLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

QByteArray lowered(const char* d, int n)
{
    QByteArray out(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i)
        out[i] = lower(d[i]);
    return out;
}

// ---- the tokenizer -------------------------------------------------------------------------------------------

struct Token {
    bool                                   end = false;
    bool                                   selfClosing = false;
    QByteArray                             name; // lower case
    QVector<QPair<QByteArray, QByteArray>> attrs; // lower-case names, raw values (first of each name)
    bool                                   hasText = false;
    QByteArray                             text; // <title> (HTML) / a captured element's text (xml-lite)

    const QByteArray* attr(const char* attrName) const
    {
        for (const auto& a : attrs) {
            if (a.first == attrName)
                return &a.second;
        }
        return nullptr;
    }
};

class Tokenizer
{
  public:
    Tokenizer(const QByteArray& data, bool xml, const QSet<QByteArray>& capture = {}, int maxValueBytes = kMaxValueBytes)
        : m_d(data.constData())
        , m_n(data.size())
        , m_xml(xml)
        , m_capture(capture)
        , m_maxValueBytes(maxValueBytes)
    {
    }

    bool next(Token* out);
    int  position() const { return m_i; }
    int  tags() const { return qMin(m_tags, kMaxTags); }
    bool tagLimit() const { return m_tagLimit; }

  private:
    bool startsWithAt(int at, const char* text) const
    {
        const int size = static_cast<int>(strlen(text));
        return at + size <= m_n && memcmp(m_d + at, text, static_cast<size_t>(size)) == 0;
    }
    int find(const char* text, int from) const
    {
        const int size = static_cast<int>(strlen(text));
        for (int at = from; at + size <= m_n; ++at) {
            const void* hit = memchr(m_d + at, text[0], static_cast<size_t>(m_n - at));
            if (!hit)
                return -1;
            at = static_cast<int>(static_cast<const char*>(hit) - m_d);
            if (at + size <= m_n && memcmp(m_d + at, text, static_cast<size_t>(size)) == 0)
                return at;
        }
        return -1;
    }
    // "</name" (case-insensitive) from from on.
    int findEndTag(const QByteArray& name, int from) const
    {
        for (int at = from; at + 2 + name.size() <= m_n; ++at) {
            const void* hit = memchr(m_d + at, '<', static_cast<size_t>(m_n - at));
            if (!hit)
                return -1;
            at = static_cast<int>(static_cast<const char*>(hit) - m_d);
            if (at + 2 + name.size() > m_n)
                return -1;
            if (m_d[at + 1] != '/')
                continue;
            bool same = true;
            for (int k = 0; k < name.size() && same; ++k)
                same = lower(m_d[at + 2 + k]) == name.at(k);
            if (same)
                return at;
        }
        return -1;
    }
    int skipTo(char c, int from) const
    {
        if (from >= m_n)
            return -1;
        const void* hit = memchr(m_d + from, c, static_cast<size_t>(m_n - from));
        return hit ? static_cast<int>(static_cast<const char*>(hit) - m_d) : -1;
    }
    bool isNameChar(char c) const
    {
        if (isLetter(c) || isDigit(c))
            return true;
        return m_xml && (c == ':' || c == '-' || c == '_' || c == '.');
    }

    const char*             m_d;
    int                     m_n;
    bool                    m_xml;
    QSet<QByteArray>        m_capture;
    int                     m_maxValueBytes = kMaxValueBytes;
    int                     m_i        = 0;
    int                     m_tags     = 0;
    bool                    m_tagLimit = false;
    bool                    m_titleSeen = false;
};

bool Tokenizer::next(Token* out)
{
    while (m_i < m_n) {
        const int lt = skipTo('<', m_i);
        if (lt < 0) {
            m_i = m_n;
            return false;
        }
        m_i = lt;
        if (startsWithAt(m_i, "<!--")) {
            const int e = find("-->", m_i + 4);
            m_i         = e < 0 ? m_n : e + 3;
            continue;
        }
        if (startsWithAt(m_i, "<![CDATA[")) {
            const int e = find("]]>", m_i + 9);
            m_i         = e < 0 ? m_n : e + 3;
            continue;
        }
        if (m_i + 1 < m_n && (m_d[m_i + 1] == '!' || m_d[m_i + 1] == '?')) {
            const int e = skipTo('>', m_i + 2);
            m_i         = e < 0 ? m_n : e + 1;
            continue;
        }
        if (m_i + 1 < m_n && m_d[m_i + 1] == '/') {
            int j = m_i + 2;
            while (j < m_n && isNameChar(m_d[j]) && j - (m_i + 2) <= kMaxXmlNameChars)
                ++j;
            const QByteArray name = lowered(m_d + m_i + 2, j - (m_i + 2));
            const int        e    = skipTo('>', j);
            m_i                   = e < 0 ? m_n : e + 1;
            if (name.isEmpty())
                continue;
            *out      = Token();
            out->end  = true;
            out->name = name;
            return true;
        }
        if (m_i + 1 < m_n && isLetter(m_d[m_i + 1])) {
            if (++m_tags > kMaxTags) {
                m_tagLimit = true;
                return false;
            }
            int j = m_i + 1;
            while (j < m_n && isNameChar(m_d[j]))
                ++j;
            const int nameChars = j - (m_i + 1);
            if (nameChars > (m_xml ? kMaxXmlNameChars : kMaxNameChars)) {
                const int e = skipTo('>', j);
                m_i         = e < 0 ? m_n : e + 1; // an unknown tag
                continue;
            }
            Token tok;
            tok.name        = lowered(m_d + m_i + 1, nameChars);
            bool terminated = false;
            while (j < m_n) {
                while (j < m_n && isWs(m_d[j]))
                    ++j;
                if (j >= m_n)
                    break;
                const char c = m_d[j];
                if (c == '>') {
                    ++j;
                    terminated = true;
                    break;
                }
                if (c == '/') {
                    if (j + 1 < m_n && m_d[j + 1] == '>') {
                        tok.selfClosing = true;
                        j += 2;
                        terminated = true;
                        break;
                    }
                    ++j;
                    continue;
                }
                int k = j;
                while (k < m_n && !isWs(m_d[k]) && m_d[k] != '/' && m_d[k] != '>' && (m_d[k] != '=' || k == j))
                    ++k;
                const int nameStart = j, nameEnd = k;
                j                   = k;
                while (j < m_n && isWs(m_d[j]))
                    ++j;
                int  valueStart = -1, valueEnd = -1;
                if (j < m_n && m_d[j] == '=') {
                    ++j;
                    while (j < m_n && isWs(m_d[j]))
                        ++j;
                    if (j < m_n && (m_d[j] == '"' || m_d[j] == '\'')) {
                        const int e = skipTo(m_d[j], j + 1);
                        valueStart  = j + 1;
                        valueEnd    = e < 0 ? m_n : e;
                        j           = e < 0 ? m_n : e + 1;
                    } else {
                        k = j;
                        while (k < m_n && !isWs(m_d[k]) && m_d[k] != '>')
                            ++k;
                        valueStart = j;
                        valueEnd   = k;
                        j          = k;
                    }
                }
                const int nameSize = nameEnd - nameStart;
                if (tok.attrs.size() < kMaxAttrs && nameSize > 0 && nameSize <= kMaxAttrNameChars
                    && (valueStart < 0 || valueEnd - valueStart <= m_maxValueBytes)) {
                    const QByteArray attrName = lowered(m_d + nameStart, nameSize);
                    if (!tok.attr(attrName.constData())) {
                        const QByteArray value = valueStart < 0 ? QByteArray() : QByteArray(m_d + valueStart, valueEnd - valueStart);
                        tok.attrs.append(qMakePair(attrName, value));
                    }
                }
            }
            if (!terminated) {
                m_i = m_n; // an unterminated tag: the scan ends with what it found before
                return false;
            }
            m_i = j;
            if (!tok.selfClosing) {
                static const QSet<QByteArray> kRaw     = {"script", "style", "textarea", "noscript", "template", "iframe", "xmp", "noembed", "noframes"};
                static const QSet<QByteArray> kXmlRaw  = {"script", "style"};
                const bool                    raw      = m_xml ? kXmlRaw.contains(tok.name) : kRaw.contains(tok.name);
                const bool                    title    = !m_xml && tok.name == "title" && !m_titleSeen;
                const bool                    captured = m_xml && m_capture.contains(tok.name);
                if (raw) {
                    const int e = findEndTag(tok.name, m_i);
                    m_i         = e < 0 ? m_n : e;
                } else if (title || captured) {
                    m_titleSeen     = m_titleSeen || title;
                    const int e     = findEndTag(tok.name, m_i);
                    const int stop  = e < 0 ? m_n : e;
                    const int limit = title ? kMaxTitleBytes : kMaxXmlTextBytes;
                    tok.hasText     = true;
                    tok.text        = QByteArray(m_d + m_i, qMin(stop - m_i, limit));
                    m_i             = stop;
                }
            }
            *out = tok;
            return true;
        }
        ++m_i; // a '<' that starts nothing
    }
    return false;
}

// ---- decoding -------------------------------------------------------------------------------------------------

QTextCodec* utf8()
{
    static QTextCodec* codec = QTextCodec::codecForName("UTF-8");
    return codec;
}

// A value as text: decoded with the page's codec, dropped when more than 10% of it is U+FFFD, entities decoded
// once, at most kMaxValueChars characters.
QString decodeValue(const QByteArray& raw, const QTextCodec* codec, int maxChars = kMaxValueChars)
{
    if (raw.isEmpty())
        return {};
    const QTextCodec*         use = codec ? codec : utf8();
    QTextCodec::ConverterState state;
    const QString             text = use->toUnicode(raw.constData(), raw.size(), &state);
    int                       bad  = 0;
    for (const QChar ch : text)
        bad += ch.unicode() == 0xFFFD ? 1 : 0;
    if (text.isEmpty() || bad * 10 > text.size())
        return {};
    QString out = decodeEntities(text);
    if (out.size() > maxChars)
        out.truncate(maxChars);
    return out;
}

bool validUtf8(const QByteArray& data)
{
    const int n = qMin(data.size(), kUtf8CheckBytes);
    const auto* d = reinterpret_cast<const unsigned char*>(data.constData());
    for (int i = 0; i < n;) {
        const unsigned char c = d[i];
        int                 extra;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= n)
            return true; // a sequence cut by the end of the check window (or of the body) is not an invalid one
        for (int k = 1; k <= extra; ++k) {
            if ((d[i + k] & 0xC0) != 0x80)
                return false;
        }
        if (c == 0xE0 && d[i + 1] < 0xA0)
            return false; // overlong
        if (c == 0xED && d[i + 1] >= 0xA0)
            return false; // surrogates
        if (c == 0xF0 && d[i + 1] < 0x90)
            return false;
        if (c == 0xF4 && d[i + 1] >= 0x90)
            return false;
        i += extra + 1;
    }
    return true;
}

QByteArray charsetOfContentType(const QByteArray& contentType)
{
    const QByteArray lowerType = contentType.toLower();
    const int        at        = lowerType.indexOf("charset");
    if (at < 0)
        return {};
    int j = at + 7;
    while (j < lowerType.size() && isWs(lowerType.at(j)))
        ++j;
    if (j >= lowerType.size() || lowerType.at(j) != '=')
        return {};
    ++j;
    while (j < lowerType.size() && isWs(lowerType.at(j)))
        ++j;
    int e = j;
    while (e < lowerType.size() && lowerType.at(e) != ';' && !isWs(lowerType.at(e)))
        ++e;
    QByteArray label = lowerType.mid(j, e - j);
    label.replace('"', QByteArray());
    label.replace('\'', QByteArray());
    return label.trimmed();
}

// The charset a <meta> of the first kPrescanBytes names.
QByteArray prescanCharset(const QByteArray& body)
{
    Tokenizer t(body.left(kPrescanBytes), false);
    Token     tok;
    while (t.next(&tok)) {
        if (tok.end || tok.name != "meta")
            continue;
        if (const QByteArray* charset = tok.attr("charset"))
            return charset->trimmed().toLower();
        const QByteArray* equiv   = tok.attr("http-equiv");
        const QByteArray* content = tok.attr("content");
        if (equiv && content && equiv->trimmed().toLower() == "content-type") {
            const QByteArray label = charsetOfContentType(*content);
            if (!label.isEmpty())
                return label;
        }
    }
    return {};
}

// ---- entities ---------------------------------------------------------------------------------------------------

const QHash<QString, uint>& namedEntities()
{
    static const QHash<QString, uint>* table = [] {
        struct E {
            const char* name;
            uint        code;
        };
        static const E kList[] = {
            {"amp", 38},      {"lt", 60},       {"gt", 62},       {"quot", 34},     {"apos", 39},     {"nbsp", 160},    {"ensp", 8194},
            {"emsp", 8195},   {"thinsp", 8201}, {"hellip", 8230}, {"mdash", 8212},  {"ndash", 8211},  {"lsquo", 8216},  {"rsquo", 8217},
            {"sbquo", 8218},  {"ldquo", 8220},  {"rdquo", 8221},  {"bdquo", 8222},  {"laquo", 171},   {"raquo", 187},   {"lsaquo", 8249},
            {"rsaquo", 8250}, {"bull", 8226},   {"middot", 183},  {"copy", 169},    {"reg", 174},     {"trade", 8482},  {"euro", 8364},
            {"pound", 163},   {"yen", 165},     {"cent", 162},    {"sect", 167},    {"para", 182},    {"deg", 176},     {"plusmn", 177},
            {"times", 215},   {"divide", 247},  {"frac12", 189},  {"frac14", 188},  {"frac34", 190},  {"sup1", 185},    {"sup2", 178},
            {"sup3", 179},    {"iexcl", 161},   {"iquest", 191},  {"shy", 173},     {"acute", 180},   {"uml", 168},     {"cedil", 184},
            {"ordf", 170},    {"ordm", 186},    {"not", 172},     {"macr", 175},    {"micro", 181},   {"Agrave", 192},  {"Aacute", 193},
            {"Acirc", 194},   {"Atilde", 195},  {"Auml", 196},    {"Aring", 197},   {"AElig", 198},   {"Ccedil", 199},  {"Egrave", 200},
            {"Eacute", 201},  {"Ecirc", 202},   {"Euml", 203},    {"Igrave", 204},  {"Iacute", 205},  {"Icirc", 206},   {"Iuml", 207},
            {"ETH", 208},     {"Ntilde", 209},  {"Ograve", 210},  {"Oacute", 211},  {"Ocirc", 212},   {"Otilde", 213},  {"Ouml", 214},
            {"Oslash", 216},  {"Ugrave", 217},  {"Uacute", 218},  {"Ucirc", 219},   {"Uuml", 220},    {"Yacute", 221},  {"THORN", 222},
            {"szlig", 223},   {"agrave", 224},  {"aacute", 225},  {"acirc", 226},   {"atilde", 227},  {"auml", 228},   {"aring", 229},
            {"aelig", 230},   {"ccedil", 231},  {"egrave", 232},  {"eacute", 233},  {"ecirc", 234},   {"euml", 235},    {"igrave", 236},
            {"iacute", 237},  {"icirc", 238},   {"iuml", 239},    {"eth", 240},     {"ntilde", 241},  {"ograve", 242},  {"oacute", 243},
            {"ocirc", 244},   {"otilde", 245},  {"ouml", 246},    {"oslash", 248},  {"ugrave", 249},  {"uacute", 250},  {"ucirc", 251},
            {"uuml", 252},    {"yacute", 253},  {"thorn", 254},   {"yuml", 255},
        };
        auto* out = new QHash<QString, uint>;
        for (const E& e : kList)
            out->insert(QString::fromLatin1(e.name), e.code);
        return out;
    }();
    return *table;
}

uint windows1252(uint code)
{
    static const uint kHigh[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                   0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                   0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    return code >= 0x80 && code <= 0x9F ? kHigh[code - 0x80] : code;
}

void appendCode(QString* out, uint code)
{
    if (code == 0 || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
        code = 0xFFFD;
    code = windows1252(code);
    *out += QString::fromUcs4(&code, 1);
}

// ---- walls ------------------------------------------------------------------------------------------------------

QString squeezed(const QString& text)
{
    QString out;
    for (const QChar ch : text) {
        if (!ch.isSpace())
            out += ch.toLower();
    }
    return out;
}

QString alnumOnly(const QString& text)
{
    QString out;
    for (const QChar ch : text) {
        if (ch.isLetterOrNumber())
            out += ch.toLower();
    }
    return out;
}

QString collapsed(const QString& text)
{
    return text.simplified().toLower();
}

bool pathHasSegment(const QUrl& url, const QStringList& names)
{
    const QStringList segments = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& segment : segments) {
        for (const QString& name : names) {
            if (segment.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        }
    }
    return false;
}

// The site part of "<word> <sep> <site>": og:site_name, the registrable domain or its first label (loosely).
bool isSitePart(const QString& part, const Meta& m, const QString& host)
{
    const QString p = alnumOnly(part);
    if (p.isEmpty())
        return false;
    const QString domain = net::registrableDomain(host);
    return (!m.ogSiteName.isEmpty() && p == alnumOnly(m.ogSiteName)) || p == alnumOnly(domain) || p == alnumOnly(domain.section(QLatin1Char('.'), 0, 0))
           || providers::siteNameMatches(part, host);
}

bool isWallWordTitle(const QString& cleanedTitle, const Meta& m, const QString& host)
{
    static const char* const kWords[] = {"access denied", "before you continue", "security check", "403 forbidden", "404 not found",
                                         "page not found", "error", "log in", "login", "sign in", "sign up"};
    const QString title = collapsed(cleanedTitle);
    for (const char* word : kWords) {
        const QString w = QString::fromLatin1(word);
        if (title == w)
            return true;
        // "<word> <sep> <site>" or "<site> <sep> <word>"
        for (const QString& sep : {latin("-"), QString(QChar(0x2013)), QString(QChar(0x2014)), latin("|"), QString(QChar(0x00B7)), latin(":")}) {
            const QString head = w + QLatin1Char(' ') + sep + QLatin1Char(' ');
            const QString tail = QLatin1Char(' ') + sep + QLatin1Char(' ') + w;
            if (title.startsWith(head) && isSitePart(title.mid(head.size()), m, host))
                return true;
            if (title.endsWith(tail) && isSitePart(title.left(title.size() - tail.size()), m, host))
                return true;
            // "<word>: <site>" without the space before the separator
            const QString tight = w + sep + QLatin1Char(' ');
            if (title.startsWith(tight) && isSitePart(title.mid(tight.size()), m, host))
                return true;
        }
    }
    return false;
}

// ---- colours ----------------------------------------------------------------------------------------------------

double linear(double channel)
{
    const double c = channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(QRgb c)
{
    return 0.2126 * linear(qRed(c)) + 0.7152 * linear(qGreen(c)) + 0.0722 * linear(qBlue(c));
}

void lab(QRgb c, double* L, double* A, double* B)
{
    const double r = linear(qRed(c)), g = linear(qGreen(c)), b = linear(qBlue(c));
    const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    const double y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b) / 1.0;
    const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
    const auto   f = [](double t) {
        const double d = 6.0 / 29.0;
        return t > d * d * d ? std::cbrt(t) : t / (3 * d * d) + 4.0 / 29.0;
    };
    *L = 116 * f(y) - 16;
    *A = 500 * (f(x) - f(y));
    *B = 200 * (f(y) - f(z));
}

QString xmlText(const QByteArray& raw)
{
    QByteArray bytes = raw.trimmed();
    if (bytes.startsWith("<![CDATA[") && bytes.endsWith("]]>"))
        return QString::fromUtf8(bytes.mid(9, bytes.size() - 12)); // no entities inside CDATA
    return decodeEntities(QString::fromUtf8(bytes));
}

} // namespace

// ---- charset ------------------------------------------------------------------------------------------------

QByteArray codecNameFor(const QByteArray& rawLabel)
{
    QByteArray label = rawLabel.trimmed().toLower();
    label.replace('"', QByteArray());
    label.replace('\'', QByteArray());
    label = label.trimmed();
    if (label == "utf-8" || label == "utf8" || label == "unicode-1-1-utf-8")
        return "UTF-8";
    if (label == "windows-1252" || label == "cp1252" || label == "iso-8859-1" || label == "iso8859-1" || label == "latin1" || label == "l1"
        || label == "us-ascii" || label == "ascii")
        return "windows-1252";
    for (int n = 1250; n <= 1258; ++n) {
        if (label == "windows-" + QByteArray::number(n) || label == "cp" + QByteArray::number(n))
            return "windows-" + QByteArray::number(n);
    }
    for (int n = 2; n <= 16; ++n) {
        if (n == 12)
            continue;
        if (label == "iso-8859-" + QByteArray::number(n) || label == "iso8859-" + QByteArray::number(n))
            return "ISO-8859-" + QByteArray::number(n);
    }
    if (label == "koi8-r")
        return "KOI8-R";
    if (label == "koi8-u")
        return "KOI8-U";
    if (label == "shift_jis" || label == "shift-jis" || label == "sjis" || label == "x-sjis" || label == "ms_kanji" || label == "windows-31j")
        return "Shift_JIS";
    if (label == "euc-jp")
        return "EUC-JP";
    if (label == "euc-kr" || label == "ks_c_5601-1987" || label == "windows-949" || label == "cp949")
        return "EUC-KR";
    if (label == "gbk" || label == "gb2312" || label == "x-gbk" || label == "gb18030")
        return "GB18030";
    if (label == "big5" || label == "big5-hkscs")
        return "Big5";
    return {};
}

Decoder decoderFor(const QByteArray& body, const QByteArray& contentType)
{
    Decoder    d;
    const auto use = [&d](const QByteArray& name) {
        QTextCodec* codec = name.isEmpty() ? nullptr : QTextCodec::codecForName(name);
        d.codec           = codec ? codec : utf8();
        d.name            = codec ? name : QByteArray("UTF-8");
        return d;
    };
    if (body.startsWith("\xEF\xBB\xBF"))
        return use("UTF-8");
    if (body.startsWith("\xFF\xFE") || body.startsWith("\xFE\xFF")) {
        d.utf16 = true;
        return use("UTF-8"); // prepared() converts the body to UTF-8 first
    }
    const QByteArray header = codecNameFor(charsetOfContentType(contentType));
    if (!header.isEmpty())
        return use(header);
    const QByteArray meta = codecNameFor(prescanCharset(body));
    if (!meta.isEmpty())
        return use(meta);
    return use(validUtf8(body) ? QByteArray("UTF-8") : QByteArray("windows-1252"));
}

QByteArray prepared(const QByteArray& body, const Decoder& decoder)
{
    if (!decoder.utf16)
        return body;
    const bool  little = body.startsWith("\xFF\xFE");
    QTextCodec* codec  = QTextCodec::codecForName(little ? "UTF-16LE" : "UTF-16BE");
    if (!codec)
        return QByteArray();
    QTextCodec::ConverterState state;
    return codec->toUnicode(body.constData() + 2, body.size() - 2, &state).toUtf8();
}

QString decodeEntities(const QString& text)
{
    if (!text.contains(QLatin1Char('&')))
        return text;
    QString out;
    out.reserve(text.size());
    const int n = text.size();
    for (int i = 0; i < n; ++i) {
        const QChar ch = text.at(i);
        if (ch != QLatin1Char('&')) {
            out += ch;
            continue;
        }
        // Numeric: &#NNN; / &#xHHH;
        if (i + 2 < n && text.at(i + 1) == QLatin1Char('#')) {
            const bool hex = text.at(i + 2) == QLatin1Char('x') || text.at(i + 2) == QLatin1Char('X');
            int        j   = i + (hex ? 3 : 2);
            int        start = j;
            uint       value = 0;
            while (j < n && j - start < 8) {
                const ushort u = text.at(j).unicode();
                int          digit;
                if (u >= '0' && u <= '9')
                    digit = u - '0';
                else if (hex && u >= 'a' && u <= 'f')
                    digit = u - 'a' + 10;
                else if (hex && u >= 'A' && u <= 'F')
                    digit = u - 'A' + 10;
                else
                    break;
                value = value * (hex ? 16 : 10) + static_cast<uint>(digit);
                ++j;
            }
            if (j > start && j < n && text.at(j) == QLatin1Char(';')) {
                appendCode(&out, value);
                i = j;
                continue;
            }
            out += ch;
            continue;
        }
        // Named: &name;
        int j = i + 1;
        while (j < n && j - (i + 1) <= 32 && text.at(j).unicode() < 0x80 && (text.at(j).isLetterOrNumber()))
            ++j;
        const QString name = text.mid(i + 1, j - (i + 1));
        if (!name.isEmpty() && j < n && text.at(j) == QLatin1Char(';')) {
            const auto it = namedEntities().constFind(name);
            if (it != namedEntities().constEnd()) {
                appendCode(&out, it.value());
                i = j;
                continue;
            }
        } else if (!name.isEmpty()) {
            // Legacy forms without ';'
            static const char* const kLegacy[] = {"amp", "lt", "gt", "quot", "nbsp"};
            bool                     done      = false;
            for (const char* legacy : kLegacy) {
                const int size = static_cast<int>(strlen(legacy));
                if (text.midRef(i + 1, size) == QLatin1String(legacy)) {
                    appendCode(&out, namedEntities().value(QString::fromLatin1(legacy)));
                    i += size;
                    done = true;
                    break;
                }
            }
            if (done)
                continue;
        }
        out += ch;
    }
    return out;
}

// ---- meta -------------------------------------------------------------------------------------------------------

Meta scan(const QByteArray& html, const QTextCodec* codec, bool bodyMode)
{
    Meta      m;
    Tokenizer t(html, false);
    Token     tok;
    struct Theme {
        QString value, media;
        bool    hasMedia = false;
    };
    QVector<Theme> themes;
    int            lastVideo = -1;
    bool           titleDone = false;
    while (t.next(&tok)) {
        if (tok.end) {
            if (tok.name == "head" && !bodyMode) {
                m.stop = Meta::Stop::HeadEnd;
                break;
            }
            continue;
        }
        if (tok.name == "body") {
            if (!bodyMode) {
                m.stop = Meta::Stop::BodyStart;
                break;
            }
            continue;
        }
        if (tok.name == "title" && tok.hasText && !titleDone) {
            titleDone = true;
            m.title   = decodeValue(tok.text, codec);
            continue;
        }
        if (tok.name != "meta")
            continue;
        if (const QByteArray* charset = tok.attr("charset")) {
            if (m.charset.isEmpty())
                m.charset = QString::fromLatin1(charset->trimmed().left(40));
        }
        const QByteArray* property = tok.attr("property");
        const QByteArray* nameAttr = tok.attr("name");
        const QByteArray* content  = tok.attr("content");
        QByteArray        key      = property && !property->trimmed().isEmpty() ? *property : (nameAttr ? *nameAttr : QByteArray());
        key                        = key.trimmed().toLower();
        if (key.isEmpty() || !content)
            continue;
        const QString value = decodeValue(*content, codec);
        if (value.isEmpty())
            continue;
        const auto first = [&value](QString* field) {
            if (field->isEmpty())
                *field = value;
        };
        if (key == "og:title")
            first(&m.ogTitle);
        else if (key == "og:description")
            first(&m.ogDescription);
        else if (key == "og:site_name")
            first(&m.ogSiteName);
        else if (key == "og:type")
            first(&m.ogType);
        else if (key == "twitter:title")
            first(&m.twitterTitle);
        else if (key == "twitter:description")
            first(&m.twitterDescription);
        else if (key == "twitter:card")
            first(&m.twitterCard);
        else if (key == "description")
            first(&m.description);
        else if (key == "og:image" || key == "og:image:url" || key == "og:image:secure_url") {
            const QString url = value.trimmed();
            if (!url.isEmpty() && !m.ogImages.contains(url) && m.ogImages.size() < 4)
                m.ogImages << url;
        } else if (key == "twitter:image" || key == "twitter:image:src") {
            const QString url = value.trimmed();
            if (!url.isEmpty() && !m.twitterImages.contains(url) && m.twitterImages.size() < 2)
                m.twitterImages << url;
        } else if (key == "og:video" || key == "og:video:url" || key == "og:video:secure_url") {
            const QString url = value.trimmed();
            int           at  = -1;
            for (int i = 0; i < m.ogVideos.size(); ++i) {
                if (m.ogVideos.at(i).url == url)
                    at = i;
            }
            if (at < 0 && !url.isEmpty() && m.ogVideos.size() < 4) {
                m.ogVideos.append({url, QString()});
                at = m.ogVideos.size() - 1;
            }
            if (at >= 0)
                lastVideo = at;
        } else if (key == "og:video:type") {
            if (lastVideo >= 0 && m.ogVideos[lastVideo].type.isEmpty())
                m.ogVideos[lastVideo].type = value.trimmed().toLower();
        } else if (key == "twitter:player:stream") {
            first(&m.twitterPlayerStream);
        } else if (key == "og:image:width") {
            if (m.ogImageWidth == 0)
                m.ogImageWidth = qBound(0, value.trimmed().toInt(), 100000);
        } else if (key == "og:image:height") {
            if (m.ogImageHeight == 0)
                m.ogImageHeight = qBound(0, value.trimmed().toInt(), 100000);
        } else if (key == "og:video:duration" || key == "video:duration") {
            if (m.videoDurationSec == 0) {
                bool      ok = false;
                const int s  = value.trimmed().toInt(&ok);
                if (ok && s >= 1 && s <= 86400)
                    m.videoDurationSec = s;
            }
        } else if (key == "theme-color") {
            const QByteArray* media = tok.attr("media");
            themes.append({value.trimmed(), media ? QString::fromLatin1(media->toLower()) : QString(), media != nullptr});
        } else if (key == "rating") {
            first(&m.rating);
        } else if (key == "og:restrictions:age") {
            first(&m.ageRestriction);
        } else if (key == "robots") {
            if (value.contains(QLatin1String("noindex"), Qt::CaseInsensitive))
                m.robotsNoindex = true;
        }
    }
    if (t.tagLimit())
        m.stop = Meta::Stop::Tags;
    m.bytesScanned = t.position();
    m.tags         = t.tags();
    // theme-color: the first without media, else the first whose media says light, else the first.
    for (const Theme& theme : qAsConst(themes)) {
        if (!theme.hasMedia && !theme.value.isEmpty()) {
            m.themeColor = theme.value;
            break;
        }
    }
    if (m.themeColor.isEmpty()) {
        for (const Theme& theme : qAsConst(themes)) {
            if (theme.media.contains(QLatin1String("light"))) {
                m.themeColor = theme.value;
                break;
            }
        }
    }
    if (m.themeColor.isEmpty() && !themes.isEmpty())
        m.themeColor = themes.first().value;
    return m;
}

bool headDone(const Meta& m, bool bodyMode)
{
    return !bodyMode && (m.stop == Meta::Stop::HeadEnd || m.stop == Meta::Stop::BodyStart);
}

Meta scanPage(const QByteArray& html, const QTextCodec* codec, bool bodyMode, bool truncated)
{
    Meta m = scan(html, codec, bodyMode);
    if (truncated && m.stop == Meta::Stop::End)
        m.stop = Meta::Stop::Cap;
    return m;
}

// ---- tags ---------------------------------------------------------------------------------------------------------

const QByteArray* Tag::attr(const char* attrName) const
{
    for (const auto& a : attrs) {
        if (a.first == attrName)
            return &a.second;
    }
    return nullptr;
}

Walk walkTags(const QByteArray& html, int maxValueBytes, const std::function<bool(const Tag&)>& visit)
{
    Tokenizer t(html, false, {}, qBound(0, maxValueBytes, kMaxWalkValueBytes));
    Token     tok;
    Walk      walk;
    while (t.next(&tok)) {
        Tag tag;
        tag.end   = tok.end;
        tag.name  = std::move(tok.name);
        tag.attrs = std::move(tok.attrs);
        if (!visit(tag))
            break;
    }
    walk.tagLimit     = t.tagLimit();
    walk.bytesScanned = t.position();
    walk.tags         = t.tags();
    return walk;
}

QString decodeAttribute(const QByteArray& raw, const QTextCodec* codec, int maxChars)
{
    return decodeValue(raw, codec, qBound(0, maxChars, kMaxValueChars));
}

// ---- card fields ------------------------------------------------------------------------------------------------

QString titleOf(const Meta& m)
{
    for (const QString& candidate : {m.ogTitle, m.twitterTitle, m.title}) {
        const QString clean = textclean::clean(candidate, kCardTitleChars);
        if (!clean.isEmpty())
            return clean;
    }
    return {};
}

QString descriptionOf(const Meta& m)
{
    for (const QString& candidate : {m.ogDescription, m.twitterDescription, m.description}) {
        const QString clean = textclean::clean(candidate, kCardDescriptionChars);
        if (!clean.isEmpty())
            return clean;
    }
    return {};
}

QString siteNameOf(const Meta& m)
{
    return textclean::clean(m.ogSiteName, kCardSiteChars);
}

QStringList pictureCandidates(const Meta& m)
{
    QStringList out;
    for (const QString& url : m.ogImages + m.twitterImages) {
        if (out.size() >= 4)
            break;
        if (!out.contains(url))
            out << url;
    }
    return out;
}

CardLayout layoutFor(const QString& twitterCard, bool hasPicture, int pictureWidth, int pictureHeight)
{
    if (!hasPicture)
        return CardLayout::None;
    const QString card = twitterCard.trimmed().toLower();
    if (card == QLatin1String("summary_large_image") || card == QLatin1String("player"))
        return CardLayout::Large;
    if (card == QLatin1String("summary"))
        return CardLayout::Thumb;
    if (pictureWidth >= 400 && pictureHeight > 0 && static_cast<double>(pictureWidth) / pictureHeight >= 1.3)
        return CardLayout::Large;
    return CardLayout::Thumb;
}

qint64 durationMsOf(const Meta& m)
{
    return m.videoDurationSec >= 1 && m.videoDurationSec <= 86400 ? qint64(m.videoDurationSec) * 1000 : 0;
}

// ---- walls ------------------------------------------------------------------------------------------------------

bool isChallengeTitle(const QString& cleanedTitle)
{
    static const char* const kPhrases[] = {"just a moment", "attention required!", "client challenge", "robot check", "are you a robot",
                                           "verify you are human", "establishing a secure connection", "one more step", "ddos-guard",
                                           "please wait... | cloudflare"};
    const QString title = collapsed(cleanedTitle);
    for (const char* phrase : kPhrases) {
        if (title.startsWith(QLatin1String(phrase)))
            return true;
    }
    return false;
}

Verdict wallOf(const Meta& m, const QUrl& linkUrl, const QUrl& finalUrl, bool challenge)
{
    if (challenge)
        return Verdict::Walled;
    const QString finalHost = publicHostName(finalUrl);
    if (finalHost.startsWith(QLatin1String("consent.")) || finalHost.startsWith(QLatin1String("guce.")) || finalHost.startsWith(QLatin1String("accounts.")))
        return Verdict::Walled;
    static const QStringList kWallSegments = {latin("consent"), latin("zustimmung"), latin("login"), latin("signin"), latin("agecheck"), latin("age_check"),
                                              latin("age-verification"), latin("challenge"), latin("checkpoint")};
    if (pathHasSegment(finalUrl, kWallSegments) && !pathHasSegment(linkUrl, kWallSegments))
        return Verdict::Walled;
    const QString title   = textclean::clean(m.title, kCardTitleChars);
    const QString ogTitle = textclean::clean(m.ogTitle, kCardTitleChars);
    if (isChallengeTitle(title))
        return Verdict::Walled;
    const QString host = finalHost.isEmpty() ? publicHostName(linkUrl) : finalHost;
    if ((ogTitle.isEmpty() || ogTitle == title) && !title.isEmpty() && isWallWordTitle(title, m, host))
        return Verdict::Walled;
    if (m.robotsNoindex && ogTitle.isEmpty() && pathHasSegment(finalUrl, {latin("consent"), latin("zustimmung"), latin("auswahl")}))
        return Verdict::Walled;

    // Boilerplate: a deep link whose title is just the site's name.
    const QStringList segments = linkUrl.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const bool        deep     = segments.size() >= 2 || (segments.size() == 1 && segments.first().size() > 5) || linkUrl.hasQuery();
    const QString     shown    = squeezed(titleOf(m));
    if (deep && !shown.isEmpty()) {
        const QString domain = net::registrableDomain(publicHostName(linkUrl));
        if ((!m.ogSiteName.isEmpty() && shown == squeezed(m.ogSiteName)) || shown == squeezed(domain) || shown == squeezed(domain.section(QLatin1Char('.'), 0, 0)))
            return Verdict::Nothing;
    }
    return Verdict::Card;
}

// ---- colours, sensitive -------------------------------------------------------------------------------------------

bool parseHexColour(const QString& text, QRgb* out)
{
    const QString t = text.trimmed();
    static const QRegularExpression re(QStringLiteral("^#([0-9A-Fa-f]{3}|[0-9A-Fa-f]{6})$"));
    const QRegularExpressionMatch   m = re.match(t);
    if (!m.hasMatch())
        return false;
    QString hex = m.captured(1);
    if (hex.size() == 3)
        hex = QString(hex.at(0)) + hex.at(0) + hex.at(1) + hex.at(1) + hex.at(2) + hex.at(2);
    bool       ok    = false;
    const uint value = hex.toUInt(&ok, 16);
    if (!ok)
        return false;
    *out = 0xFF000000u | value;
    return true;
}

double deltaE(QRgb a, QRgb b)
{
    double l1, a1, b1, l2, a2, b2;
    lab(a, &l1, &a1, &b1);
    lab(b, &l2, &a2, &b2);
    return std::sqrt((l1 - l2) * (l1 - l2) + (a1 - a2) * (a1 - a2) + (b1 - b2) * (b1 - b2));
}

bool isAcceptableThemeColour(QRgb colour)
{
    const double l = luminance(colour);
    if (l > 0.85 || l < 0.02)
        return false;
    for (const QRgb brand : providers::brandColours()) {
        if (deltaE(colour, brand) < 6.0)
            return false;
    }
    return true;
}

bool themeColourOf(const Meta& m, QRgb* out)
{
    QRgb colour = 0;
    if (m.themeColor.isEmpty() || !parseHexColour(m.themeColor, &colour) || !isAcceptableThemeColour(colour))
        return false;
    *out = colour;
    return true;
}

bool isSensitive(const Meta& m)
{
    const QString rating = m.rating.toLower();
    if (rating.contains(QLatin1String("adult")) || rating.contains(QLatin1String("rta-5042-1996-1400-1577-rta")))
        return true;
    const QString age = m.ageRestriction.trimmed();
    int           i   = 0;
    while (i < age.size() && age.at(i).isDigit() && i < 3)
        ++i;
    return i > 0 && age.leftRef(i).toInt() >= 18;
}

// ---- xml-lite ---------------------------------------------------------------------------------------------------

FeedEntry readFeed(const QByteArray& xml)
{
    FeedEntry entry;
    Tokenizer t(xml, true, {"title", "name", "content"});
    Token     tok;
    bool      inEntry = false;
    while (t.next(&tok)) {
        if (tok.end) {
            if (tok.name == "entry" && inEntry)
                break;
            continue;
        }
        if (tok.name == "entry") {
            inEntry     = true;
            entry.found = true;
            continue;
        }
        if (!inEntry)
            continue;
        if (tok.name == "title" && tok.hasText && entry.title.isEmpty())
            entry.title = xmlText(tok.text);
        else if (tok.name == "category" && entry.category.isEmpty()) {
            if (const QByteArray* label = tok.attr("label"))
                entry.category = decodeEntities(QString::fromUtf8(*label));
        } else if (tok.name == "name" && tok.hasText && entry.author.isEmpty()) {
            entry.author = xmlText(tok.text);
        } else if (tok.name == "media:thumbnail" && entry.thumbnail.isEmpty()) {
            if (const QByteArray* url = tok.attr("url"))
                entry.thumbnail = decodeEntities(QString::fromUtf8(*url));
        } else if (tok.name == "content" && tok.hasText && entry.content.isEmpty()) {
            entry.content = xmlText(tok.text);
        }
    }
    return entry;
}

QString linkOfContent(const QString& content)
{
    static const QRegularExpression re(QStringLiteral("<a\\s+href\\s*=\\s*\"([^\"]{1,2048})\"[^>]{0,256}>\\s*\\[link\\]\\s*</a>"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(content);
    return m.hasMatch() ? decodeEntities(m.captured(1)).trimmed() : QString();
}

Manifest readManifest(const QByteArray& xml)
{
    Manifest  manifest;
    Tokenizer t(xml, true, {"baseurl"});
    Token     tok;
    bool      setAudio = false, inRep = false, repAudio = false;
    int       repHeight = 0;
    const auto isAudio = [](const Token& token) {
        const QByteArray* type = token.attr("contenttype");
        const QByteArray* mime = token.attr("mimetype");
        return (type && type->trimmed().toLower() == "audio") || (mime && mime->trimmed().toLower().startsWith("audio/"));
    };
    while (t.next(&tok)) {
        if (tok.end) {
            if (tok.name == "adaptationset")
                setAudio = false;
            else if (tok.name == "representation")
                inRep = false;
            continue;
        }
        if (tok.name == "adaptationset") {
            setAudio = isAudio(tok);
            if (setAudio)
                manifest.hasAudio = true;
        } else if (tok.name == "representation") {
            inRep     = !tok.selfClosing;
            repAudio  = setAudio || isAudio(tok);
            repHeight = 0;
            if (repAudio)
                manifest.hasAudio = true;
            if (const QByteArray* height = tok.attr("height"))
                repHeight = qBound(0, height->trimmed().toInt(), 100000);
        } else if (tok.name == "baseurl" && tok.hasText && inRep && !repAudio) {
            manifest.videos.append({repHeight, xmlText(tok.text).trimmed()});
        }
    }
    return manifest;
}

QString pickManifestVideo(const Manifest& manifest, int maxHeight)
{
    static const QRegularExpression kName(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    const Manifest::Video* best     = nullptr;
    const Manifest::Video* smallest = nullptr;
    for (const Manifest::Video& v : manifest.videos) {
        if (!kName.match(v.baseUrl).hasMatch() || v.baseUrl.startsWith(QLatin1Char('.')))
            continue;
        if (v.height <= maxHeight && (!best || v.height > best->height))
            best = &v;
        if (!smallest || v.height < smallest->height)
            smallest = &v;
    }
    const Manifest::Video* pick = best ? best : smallest;
    return pick ? pick->baseUrl : QString();
}

} // namespace linkpreview::pagemeta
