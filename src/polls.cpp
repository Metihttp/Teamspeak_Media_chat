#include "polls.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

#include "i18n.h"
#include "medialink.h" // captionToBBCode

namespace polls {

namespace {

constexpr int    kIdBytes       = 16;
constexpr int    kEntryChars    = 22; // base64url of 16 bytes, no padding
constexpr qint64 kMaxFileBytes  = 8 * 1024 * 1024;
const char       kDefaultPage[] = "https://github.com/Metihttp/Teamspeak_Media_chat";

QByteArray::Base64Options b64()
{
    return QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
}

bool isB64Char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// 16 bytes from 22 base64url characters, canonical only (one text per value).
bool decode16(const QByteArray& text, QByteArray* out)
{
    if (text.size() != kEntryChars || !std::all_of(text.begin(), text.end(), isB64Char))
        return false;
    const QByteArray bytes = QByteArray::fromBase64(text, b64());
    if (bytes.size() != kIdBytes || bytes.toBase64(b64()) != text)
        return false;
    *out = bytes;
    return true;
}

bool isHexLower(const QByteArray& text, int maxChars)
{
    if (text.isEmpty() || text.size() > maxChars)
        return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool isDecimal(const QByteArray& text, int maxChars)
{
    if (text.isEmpty() || text.size() > maxChars || (text.size() > 1 && text.at(0) == '0'))
        return false;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

bool invisible(uint c)
{
    return c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0x061c || c == 0x200b || c == 0x200c || c == 0x200d || c == 0x200e || c == 0x200f
           || (c >= 0x202a && c <= 0x202e) || c == 0x2060 || (c >= 0x2066 && c <= 0x2069) || c == 0xfeff;
}

int popcount(quint16 v)
{
    int n = 0;
    for (; v; v &= static_cast<quint16>(v - 1))
        ++n;
    return n;
}

QString pollEmoji()
{
    return QString::fromUtf8("\xF0\x9F\x93\x8A"); // U+1F4CA BAR CHART
}

} // namespace

QString markerPrefix()
{
    return QString::fromLatin1("tsmpoll1-");
}

bool Poll::isValid() const
{
    if (!isKey(key) || question.isEmpty() || options.size() < kMinOptions || options.size() > kMaxOptions || channelId == 0)
        return false;
    for (const QString& option : options) {
        if (option.isEmpty())
            return false;
    }
    return true;
}

QString newKey()
{
    quint64 value = 0;
    while (value == 0)
        value = QRandomGenerator::system()->generate64();
    return QString::fromLatin1("%1").arg(value, kKeyChars, 16, QLatin1Char('0'));
}

bool isKey(const QByteArray& key)
{
    return key.size() == kKeyChars && isHexLower(key, kKeyChars) && key != QByteArray(kKeyChars, '0');
}

bool isKey(const QString& key)
{
    return key.size() == kKeyChars && isKey(key.toLatin1());
}

QString cleanLine(const QString& typed, int max)
{
    QString out;
    out.reserve(qMin(typed.size(), max + 8));
    bool space = false;
    for (int i = 0; i < typed.size(); ++i) {
        const QChar c = typed.at(i);
        uint        code = c.unicode();
        if (c.isHighSurrogate() && i + 1 < typed.size() && typed.at(i + 1).isLowSurrogate())
            code = QChar::surrogateToUcs4(c, typed.at(i + 1));
        else if (c.isSurrogate())
            continue; // a lone half
        const bool ws = code == '\n' || code == '\r' || code == '\t' || code == 0x2028 || code == 0x2029 || (code < 0x10000 && QChar(code).isSpace());
        if (ws) {
            space = !out.isEmpty();
            continue;
        }
        if (invisible(code)) {
            if (code > 0xffff)
                ++i;
            continue;
        }
        const int width = code > 0xffff ? 2 : 1;
        if (out.size() + (space ? 1 : 0) + width > max)
            break;
        if (space)
            out += QLatin1Char(' ');
        space = false;
        if (code > 0xffff) {
            out += c;
            out += typed.at(++i);
        } else {
            out += c;
        }
    }
    return out;
}

// ---- the message ------------------------------------------------------------------------------------------

QString markerFragment(const Poll& poll)
{
    return markerPrefix() + poll.key + QLatin1Char('-') + QString::number(poll.options.size()) + QLatin1Char(poll.multiple ? 'm' : 's')
           + QLatin1Char('-') + QString::number(poll.channelId);
}

QString composeMessage(const Poll& poll, const QString& pageUrl)
{
    QString page = pageUrl.section(QLatin1Char('#'), 0, 0).trimmed();
    const bool web = page.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) || page.startsWith(QLatin1String("http://"), Qt::CaseInsensitive);
    static const QRegularExpression bad(QStringLiteral("[\\[\\]\\s\"]"));
    if (!web || page.size() > 400 || page.contains(bad))
        page = QString::fromLatin1(kDefaultPage);
    QString text = pollEmoji() + QLatin1Char(' ') + i18n::t("Poll:") + QLatin1Char(' ') + captionToBBCode(cleanLine(poll.question, kMaxQuestionChars));
    for (int i = 0; i < poll.options.size(); ++i)
        text += QLatin1Char('\n') + QString::number(i + 1) + QLatin1String(". ") + captionToBBCode(cleanLine(poll.options.at(i), kMaxOptionChars));
    text += QLatin1Char('\n') + QLatin1String("[URL=") + page + QLatin1Char('#') + markerFragment(poll) + QLatin1Char(']') + i18n::t("Vote with TS Media chat")
            + QLatin1String("[/URL]");
    return text;
}

Marker parseMarker(const QString& href)
{
    Marker m;
    if (href.size() > 1024)
        return m;
    if (!href.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) && !href.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        return m;
    const int hash = href.indexOf(QLatin1Char('#'));
    if (hash < 0)
        return m;
    static const QRegularExpression re(QStringLiteral("^tsmpoll1-([0-9a-f]{16})-(10|[2-9])([sm])-([1-9][0-9]{0,14})$"));
    const QRegularExpressionMatch   match = re.match(href.mid(hash + 1));
    if (!match.hasMatch() || !isKey(match.captured(1)))
        return m;
    m.key       = match.captured(1);
    m.count     = match.captured(2).toInt();
    m.multiple  = match.captured(3) == QLatin1String("m");
    m.channelId = match.captured(4).toULongLong();
    return m;
}

std::optional<Poll> parseText(const QString& text, const Marker& marker)
{
    if (!marker.valid() || text.size() > 8192)
        return std::nullopt;
    QString all = text;
    all.replace(QChar::ParagraphSeparator, QChar::LineSeparator);
    all.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QStringList lines = all.split(QChar::LineSeparator);
    while (!lines.isEmpty() && cleanLine(lines.last(), 8).isEmpty())
        lines.removeLast();
    if (lines.size() != marker.count + 1)
        return std::nullopt;
    Poll poll;
    poll.key       = marker.key;
    poll.multiple  = marker.multiple;
    poll.channelId = marker.channelId;
    QString first  = cleanLine(lines.first(), 4 * kMaxQuestionChars);
    if (first.startsWith(pollEmoji()))
        first = first.mid(pollEmoji().size()).trimmed();
    const QString label = i18n::t("Poll:");
    if (!first.startsWith(label, Qt::CaseInsensitive))
        return std::nullopt;
    poll.question = cleanLine(first.mid(label.size()), 2 * kMaxQuestionChars);
    static const QRegularExpression option(QStringLiteral("^([0-9]{1,2})[.)]\\s*(.*)$"));
    for (int i = 1; i < lines.size(); ++i) {
        const QString                 line  = cleanLine(lines.at(i), 4 * kMaxOptionChars);
        const QRegularExpressionMatch match = option.match(line);
        if (!match.hasMatch() || match.captured(1).toInt() != i)
            return std::nullopt;
        poll.options.append(cleanLine(match.captured(2), 2 * kMaxOptionChars));
    }
    if (!poll.isValid())
        return std::nullopt;
    return poll;
}

// ---- ballots -----------------------------------------------------------------------------------------------

QByteArray chainNext(const QString& key, const QByteArray& value)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData("tsmpoll1|", 9);
    h.addData(key.toLatin1());
    h.addData("|", 1);
    h.addData(value);
    return h.result().left(kIdBytes);
}

QByteArray chainAt(const QString& key, const QByteArray& secret, int index)
{
    QByteArray value = secret;
    for (int i = 0; i < index; ++i)
        value = chainNext(key, value);
    return value;
}

QByteArray newSecret()
{
    QByteArray secret(kIdBytes, Qt::Uninitialized);
    for (int i = 0; i < kIdBytes; i += 4) {
        const quint32 v = QRandomGenerator::system()->generate();
        for (int k = 0; k < 4; ++k)
            secret[i + k] = static_cast<char>((v >> (8 * k)) & 0xff);
    }
    return secret;
}

Ballot makeBallot(const QString& key, const QByteArray& secret, int step, quint16 mask)
{
    Ballot b;
    b.step  = qBound(1, step, kChainLength);
    b.proof = chainAt(key, secret, kChainLength - b.step);
    b.id    = chainAt(key, b.proof, b.step);
    b.mask  = mask;
    return b;
}

bool follows(const QString& key, const Ballot& known, const Ballot& update)
{
    if (update.id.size() != kIdBytes || update.proof.size() != kIdBytes || update.step < 1 || update.step > kChainLength)
        return false;
    if (known.step >= update.step)
        return false;
    const QByteArray target = known.step > 0 ? known.proof : known.id;
    if (target.size() != kIdBytes)
        return false;
    return chainAt(key, update.proof, update.step - known.step) == target;
}

QByteArray entryText(const Ballot& ballot)
{
    return ballot.id.toBase64(b64()) + '.' + QByteArray::number(ballot.step) + '.' + ballot.proof.toBase64(b64()) + '.' + QByteArray::number(ballot.mask, 16);
}

std::optional<Ballot> parseEntry(const QByteArray& text)
{
    if (text.size() > 2 * kEntryChars + 12)
        return std::nullopt;
    const QList<QByteArray> parts = text.split('.');
    if (parts.size() != 4)
        return std::nullopt;
    Ballot b;
    if (!decode16(parts.at(0), &b.id) || !decode16(parts.at(2), &b.proof) || !isDecimal(parts.at(1), 3) || !isHexLower(parts.at(3), 3))
        return std::nullopt;
    b.step = parts.at(1).toInt();
    if (b.step < 1 || b.step > kChainLength)
        return std::nullopt;
    const uint mask = parts.at(3).toUInt(nullptr, 16);
    if (mask >= (1u << kMaxOptions) || QByteArray::number(mask, 16) != parts.at(3))
        return std::nullopt;
    b.mask = static_cast<quint16>(mask);
    return b;
}

bool validMask(quint16 mask, int count, bool multiple)
{
    if (mask == 0 || count < 1 || count > kMaxOptions || (mask >> count) != 0)
        return false;
    return multiple || popcount(mask) == 1;
}

int Tally::percent(int option) const
{
    if (voters <= 0 || option < 0 || option >= counts.size())
        return 0;
    return qRound(100.0 * counts.at(option) / voters);
}

Tally tally(const QVector<Ballot>& ballots, int count, bool multiple)
{
    Tally t;
    t.counts.fill(0, qBound(0, count, kMaxOptions));
    for (const Ballot& b : ballots) {
        if (!validMask(b.mask, count, multiple))
            continue;
        ++t.voters;
        for (int i = 0; i < count; ++i) {
            if (b.mask & (1u << i))
                ++t.counts[i];
        }
    }
    return t;
}

QVector<int> percentages(const Tally& tally, bool multiple)
{
    QVector<int> out(tally.counts.size(), 0);
    if (tally.voters <= 0)
        return out;
    if (multiple) {
        for (int i = 0; i < out.size(); ++i)
            out[i] = tally.percent(i);
        return out;
    }
    // Largest remainder: whole percents that add up to 100.
    int                   sum = 0;
    QVector<QPair<double, int>> rest;
    for (int i = 0; i < out.size(); ++i) {
        const double exact = 100.0 * tally.counts.at(i) / tally.voters;
        out[i]             = static_cast<int>(exact);
        sum += out[i];
        rest.append({exact - out[i], i});
    }
    std::stable_sort(rest.begin(), rest.end(), [](const QPair<double, int>& a, const QPair<double, int>& b) { return a.first > b.first; });
    for (int k = 0; k < rest.size() && sum < 100; ++k) {
        if (rest.at(k).first <= 0.0)
            break;
        ++out[rest.at(k).second];
        ++sum;
    }
    return out;
}

// ---- messages ------------------------------------------------------------------------------------------------

QVector<proto::Message> makeVotes(const QString& key, const QVector<Ballot>& ballots, bool answer)
{
    QVector<proto::Message> out;
    if (!isKey(key))
        return out;
    for (int from = 0; from < ballots.size(); from += kMaxEntries) {
        QByteArray list;
        for (int i = from; i < qMin(ballots.size(), from + kMaxEntries); ++i) {
            if (!list.isEmpty())
                list += ',';
            list += entryText(ballots.at(i));
        }
        proto::Message m;
        m.type = "PV";
        m.set("q", key.toLatin1());
        m.set("v", list);
        if (answer)
            m.set("y", "1");
        out.append(m);
    }
    return out;
}

std::optional<Votes> readVotes(const proto::Message& message)
{
    if (message.type != "PV" || !isKey(message.value("q")))
        return std::nullopt;
    Votes v;
    v.key    = QString::fromLatin1(message.value("q"));
    v.answer = message.value("y") == "1";
    const QList<QByteArray> entries = message.value("v").split(',');
    if (entries.size() > kMaxEntries)
        return std::nullopt;
    QSet<QByteArray> seen;
    for (const QByteArray& e : entries) {
        const std::optional<Ballot> b = parseEntry(e);
        if (!b || seen.contains(b->id))
            continue;
        seen.insert(b->id);
        v.ballots.append(*b);
    }
    if (v.ballots.isEmpty())
        return std::nullopt;
    return v;
}

proto::Message makeQuery(const QVector<Digest>& digests)
{
    proto::Message m;
    m.type = "PQ";
    QByteArray list;
    int        used = 0;
    for (const Digest& d : digests) {
        if (used >= kMaxQueried || !isKey(d.key))
            continue;
        if (!list.isEmpty())
            list += ',';
        list += d.key.toLatin1() + ':' + QByteArray::number(qBound(0, d.count, kMaxBallots)) + ':' + d.hash;
        ++used;
    }
    m.set("q", list);
    return m;
}

std::optional<QVector<Digest>> readQuery(const proto::Message& message)
{
    if (message.type != "PQ")
        return std::nullopt;
    const QList<QByteArray> items = message.value("q").split(',');
    if (items.size() > kMaxQueried)
        return std::nullopt;
    QVector<Digest> out;
    for (const QByteArray& item : items) {
        const QList<QByteArray> parts = item.split(':');
        if (parts.size() != 3 || !isKey(parts.at(0)) || !isDecimal(parts.at(1), 4) || parts.at(2).size() != 8
            || !std::all_of(parts.at(2).begin(), parts.at(2).end(), isB64Char))
            continue;
        Digest d;
        d.key   = QString::fromLatin1(parts.at(0));
        d.count = parts.at(1).toInt();
        d.hash  = parts.at(2);
        bool dup = false;
        for (const Digest& o : qAsConst(out))
            dup = dup || o.key == d.key;
        if (!dup && d.count <= kMaxBallots)
            out.append(d);
    }
    if (out.isEmpty())
        return std::nullopt;
    return out;
}

// ---- the store ------------------------------------------------------------------------------------------------

Store::Entry& Store::entry(const QString& key, qint64 now)
{
    auto it = m_polls.find(key);
    if (it == m_polls.end()) {
        Entry fresh;
        fresh.touched = now;
        m_polls.insert(key, fresh);
        trim(key);
        it = m_polls.find(key);
    }
    it->touched = qMax(it->touched, now);
    return it.value();
}

void Store::trim(const QString& keep)
{
    // Polls without your vote go first: your secret is what lets you change your vote (a lost one would
    // make a second ballot of yours on the next vote).
    while (m_polls.size() > kMaxPolls || m_ballotCount > kMaxStoredBallots) {
        auto oldest = m_polls.end();
        for (auto it = m_polls.begin(); it != m_polls.end(); ++it) {
            if (it.key() == keep)
                continue;
            if (oldest == m_polls.end() || (!it->own && oldest->own) || (it->own.has_value() == oldest->own.has_value() && it->touched < oldest->touched))
                oldest = it;
        }
        if (oldest == m_polls.end())
            return;
        m_ballotCount -= oldest->ballots.size();
        m_polls.erase(oldest);
    }
}

void Store::recount()
{
    m_ballotCount = 0;
    for (const Entry& e : qAsConst(m_polls))
        m_ballotCount += e.ballots.size();
}

Store::Merge Store::merge(const QString& key, const Ballot& ballot, qint64 now)
{
    if (!isKey(key))
        return Merge::Ignored;
    auto           it    = m_polls.constFind(key);
    const bool     known = it != m_polls.constEnd() && it->ballots.contains(ballot.id);
    Ballot         base;
    if (known) {
        base = it->ballots.value(ballot.id);
    } else {
        if (it != m_polls.constEnd() && it->ballots.size() >= kMaxBallots)
            return Merge::Ignored;
        base.id = ballot.id;
    }
    if (!follows(key, base, ballot))
        return Merge::Ignored;
    Entry& e = entry(key, now);
    if (!e.ballots.contains(ballot.id))
        ++m_ballotCount;
    e.ballots.insert(ballot.id, ballot);
    if (e.own && e.own->id == ballot.id && ballot.step > e.own->step) {
        // Ours, newer than we remember: what the others have is our vote.
        e.own->step          = ballot.step;
        e.own->mask          = ballot.mask;
        e.own->confirmed     = ballot.mask;
        e.own->confirmedStep = ballot.step;
    }
    if (!known)
        trim(key);
    return known ? Merge::Updated : Merge::Added;
}

std::optional<Ballot> Store::castOwn(const QString& key, quint16 mask, qint64 now)
{
    if (!isKey(key))
        return std::nullopt;
    Entry& e = entry(key, now);
    if (!e.own) {
        Own own;
        own.secret = newSecret();
        own.id     = chainAt(key, own.secret, kChainLength);
        e.own      = own;
    }
    if (e.own->step >= kChainLength)
        return std::nullopt;
    const int    step = e.own->step + 1;
    const Ballot b    = makeBallot(key, e.own->secret, step, mask);
    e.own->step       = step;
    e.own->mask       = mask;
    if (!e.ballots.contains(b.id))
        ++m_ballotCount;
    e.ballots.insert(b.id, b);
    trim(key);
    return b;
}

void Store::confirmOwn(const QString& key, int step, quint16 mask)
{
    auto it = m_polls.find(key);
    if (it == m_polls.end() || !it->own || step < it->own->confirmedStep)
        return;
    it->own->confirmed     = mask;
    it->own->confirmedStep = step;
}

QVector<Ballot> Store::ballots(const QString& key) const
{
    QVector<Ballot> out;
    const auto      it = m_polls.constFind(key);
    if (it == m_polls.constEnd())
        return out;
    out.reserve(it->ballots.size());
    for (const Ballot& b : it->ballots)
        out.append(b);
    std::sort(out.begin(), out.end(), [](const Ballot& a, const Ballot& b) { return a.id < b.id; });
    return out;
}

Ballot Store::ballot(const QString& key, const QByteArray& id) const
{
    const auto it = m_polls.constFind(key);
    return it == m_polls.constEnd() ? Ballot() : it->ballots.value(id);
}

Digest Store::digest(const QString& key) const
{
    Digest d;
    d.key                        = key;
    const QVector<Ballot> sorted = ballots(key);
    d.count                      = sorted.size();
    QCryptographicHash h(QCryptographicHash::Sha256);
    for (const Ballot& b : sorted) {
        h.addData(b.id);
        const char step[2] = {static_cast<char>(b.step >> 8), static_cast<char>(b.step & 0xff)};
        h.addData(step, 2);
    }
    d.hash = h.result().left(6).toBase64(b64());
    return d;
}

Tally Store::tally(const QString& key, int count, bool multiple) const
{
    return polls::tally(ballots(key), count, multiple);
}

std::optional<Store::Own> Store::own(const QString& key) const
{
    const auto it = m_polls.constFind(key);
    if (it == m_polls.constEnd())
        return std::nullopt;
    return it->own;
}

QJsonObject Store::toJson() const
{
    QJsonArray list;
    for (auto it = m_polls.constBegin(); it != m_polls.constEnd(); ++it) {
        QJsonObject poll;
        poll.insert(QStringLiteral("k"), it.key());
        poll.insert(QStringLiteral("t"), static_cast<double>(it->touched));
        QJsonArray ballots;
        for (const Ballot& b : this->ballots(it.key()))
            ballots.append(QString::fromLatin1(entryText(b)));
        poll.insert(QStringLiteral("b"), ballots);
        if (it->own) {
            QJsonObject own;
            own.insert(QStringLiteral("s"), QString::fromLatin1(it->own->secret.toBase64(b64())));
            own.insert(QStringLiteral("j"), it->own->step);
            own.insert(QStringLiteral("m"), it->own->mask);
            own.insert(QStringLiteral("c"), it->own->confirmed);
            own.insert(QStringLiteral("cj"), it->own->confirmedStep);
            poll.insert(QStringLiteral("own"), own);
        }
        list.append(poll);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("tsmedia-polls"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("polls"), list);
    return root;
}

bool Store::fromJson(const QJsonObject& json)
{
    if (json.value(QStringLiteral("format")).toString() != QLatin1String("tsmedia-polls") || json.value(QStringLiteral("version")).toInt() != 1)
        return false;
    QHash<QString, Entry> polls;
    const QJsonArray      list = json.value(QStringLiteral("polls")).toArray();
    for (int i = 0; i < list.size() && polls.size() < kMaxPolls; ++i) {
        const QJsonObject poll = list.at(i).toObject();
        const QString     key  = poll.value(QStringLiteral("k")).toString();
        if (!isKey(key) || polls.contains(key))
            continue;
        Entry            e;
        const double     touched = poll.value(QStringLiteral("t")).toDouble();
        e.touched                = touched > 0 && touched < 1e15 ? static_cast<qint64>(touched) : 0;
        const QJsonArray ballots = poll.value(QStringLiteral("b")).toArray();
        for (int k = 0; k < ballots.size() && e.ballots.size() < kMaxBallots; ++k) {
            const std::optional<Ballot> b = parseEntry(ballots.at(k).toString().toLatin1());
            if (b)
                e.ballots.insert(b->id, *b);
        }
        if (poll.contains(QStringLiteral("own"))) {
            const QJsonObject o = poll.value(QStringLiteral("own")).toObject();
            Own               own;
            QByteArray        secret;
            if (decode16(o.value(QStringLiteral("s")).toString().toLatin1(), &secret)) {
                own.secret        = secret;
                own.id            = chainAt(key, secret, kChainLength);
                own.step          = qBound(0, o.value(QStringLiteral("j")).toInt(), kChainLength);
                own.mask          = static_cast<quint16>(qBound(0, o.value(QStringLiteral("m")).toInt(), (1 << kMaxOptions) - 1));
                own.confirmed     = static_cast<quint16>(qBound(0, o.value(QStringLiteral("c")).toInt(), (1 << kMaxOptions) - 1));
                own.confirmedStep = qBound(0, o.value(QStringLiteral("cj")).toInt(), own.step);
                // The ballot kept is newer than the vote written down: the others have that one.
                const Ballot kept = e.ballots.value(own.id);
                if (kept.step > own.step) {
                    own.step          = kept.step;
                    own.mask          = kept.mask;
                    own.confirmed     = kept.mask;
                    own.confirmedStep = kept.step;
                }
                e.own = own;
            }
        }
        polls.insert(key, e);
    }
    m_polls = polls;
    recount();
    trim(QString()); // a file from before the total limit: within it again
    return true;
}

bool Store::load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFileBytes)
        return false;
    QJsonParseError     error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    return fromJson(doc.object());
}

bool Store::save(const QString& path) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
    return file.commit();
}

} // namespace polls
