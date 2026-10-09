#include "albums.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace albums {

namespace {

constexpr int kMaxItems   = 10; // MediaLink::kMaxAlbumItems (this module doesn't depend on medialink)
constexpr int kKeyChars   = 20; // MediaLink::key(): 20 lower-case hex digits
constexpr int kMinHeightCap = 64;

// total pixels in n parts that differ by at most one.
QVector<int> split(int total, int n)
{
    QVector<int> parts;
    int          previous = 0;
    for (int i = 1; i <= n; ++i) {
        const int at = static_cast<int>(static_cast<qint64>(total) * i / n);
        parts.append(at - previous);
        previous = at;
    }
    return parts;
}

// The rows' wanted heights, scaled down together when they don't fit available. The parts add up to
// exactly available (rounded cumulatively, so the error never piles up in one row).
QVector<int> fitHeights(const QVector<int>& wanted, int available)
{
    qint64 sum = 0;
    for (const int h : wanted)
        sum += h;
    if (sum <= available || sum <= 0)
        return wanted;
    QVector<int> heights;
    qint64       cumulative = 0;
    int          previous   = 0;
    for (const int h : wanted) {
        cumulative += h;
        const int at = static_cast<int>((cumulative * available + sum / 2) / sum);
        heights.append(at - previous);
        previous = at;
    }
    return heights;
}

bool isKey(const QString& text)
{
    if (text.size() != kKeyChars)
        return false;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f')))
            return false;
    }
    return true;
}

QString hex8(quint32 value)
{
    return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

} // namespace

// ============================================================================================
// Geometry
// ============================================================================================

Geometry layout(int items, int maxWidth, int maxHeight)
{
    Geometry g;
    if (items <= 0)
        return g;
    const int  width  = qMax(2 * kMinTile + kGap, maxWidth);
    const int  cap    = qMax(kMinHeightCap, qRound(maxHeight * 4.0 / 3.0));
    const bool narrow = width < kNarrowWidth;
    const int  shown  = qMin(items, narrow ? kMaxTilesNarrow : kMaxTiles);
    g.overflow        = items > shown ? items - shown + 1 : 0;

    // Three in a wide grid: one large tile on the left, two stacked on its right.
    if (shown == 3 && !narrow) {
        const int big    = qRound((width - kGap) * 2.0 / 3.0);
        const int right  = width - kGap - big;
        const int height = qMin(big, cap);
        const int top    = (height - kGap) / 2;
        g.tiles          = {QRect(0, 0, big, height), QRect(big + kGap, 0, right, top), QRect(big + kGap, top + kGap, right, height - kGap - top)};
        g.box            = QSize(width, height);
        return g;
    }

    QVector<int> rows; // tiles per row
    switch (shown) {
    case 1:
        rows = {1};
        break;
    case 2:
        rows = {2};
        break;
    case 3:
        rows = {1, 2}; // narrow
        break;
    case 4:
        rows = {2, 2};
        break;
    case 5:
        rows = {2, 3};
        break;
    default:
        rows = {3, 3};
        break;
    }

    // Square tiles, except the 16:9 top row of three in a narrow grid.
    QVector<int> wanted;
    for (int r = 0; r < rows.size(); ++r) {
        const int n = rows.at(r);
        wanted.append(shown == 3 && r == 0 ? qRound(width * 9.0 / 16.0) : qRound(static_cast<double>(width - (n - 1) * kGap) / n));
    }
    const QVector<int> heights = fitHeights(wanted, cap - (rows.size() - 1) * kGap);

    int y = 0;
    for (int r = 0; r < rows.size(); ++r) {
        const int          n      = rows.at(r);
        const QVector<int> widths = split(width - (n - 1) * kGap, n);
        int                x      = 0;
        for (const int w : widths) {
            g.tiles.append(QRect(x, y, w, heights.at(r)));
            x += w + kGap;
        }
        y += heights.at(r) + kGap;
    }
    g.box = QSize(width, y - kGap);
    return g;
}

int tileAt(const Geometry& geometry, const QPointF& pos)
{
    for (int i = 0; i < geometry.tiles.size(); ++i) {
        if (QRectF(geometry.tiles.at(i)).contains(pos))
            return i;
    }
    return -1;
}

// ============================================================================================
// Grouping
// ============================================================================================

namespace {

bool isMember(const Link& link)
{
    return link.albumId != 0 && link.count >= 2 && link.count <= kMaxItems && link.index >= 1 && link.index <= link.count && !link.key.isEmpty();
}

struct Run {
    quint32                  id    = 0;
    int                      count = 0;
    QString                  sender;
    QVector<QPair<int, int>> members;
    QVector<int>             indexes;
    int                      lastMessage = -1;
    int                      lastLink    = -1;
};

// Whether the item at (m, l) carries run on.
bool continues(const Run& run, const QVector<Message>& messages, int m, int l)
{
    const Link& link = messages.at(m).links.at(l);
    if (link.albumId != run.id || link.count != run.count || run.indexes.contains(link.index))
        return false;
    if (m == run.lastMessage)
        return l == run.lastLink + 1;
    // Into the next message: it must directly follow, the run must have ended its message, and the
    // next message must be nothing but more of this album from the same (known) sender.
    if (m != run.lastMessage + 1 || l != 0 || run.lastLink != messages.at(run.lastMessage).links.size() - 1)
        return false;
    const Message& next = messages.at(m);
    if (!next.bare || next.sender.isEmpty() || next.sender != run.sender)
        return false;
    QVector<int> seen = run.indexes;
    for (const Link& other : next.links) {
        if (!isMember(other) || other.albumId != run.id || other.count != run.count || seen.contains(other.index))
            return false;
        seen.append(other.index);
    }
    return true;
}

} // namespace

QVector<Album> group(const QVector<Message>& messages)
{
    QVector<Album> albums;
    QSet<quint32>  started; // ids whose first run is done
    Run            run;

    const auto close = [&] {
        if (run.id == 0)
            return;
        const bool first = !started.contains(run.id);
        started.insert(run.id);
        const int  last  = messages.size() - 1;
        const bool atEnd = run.lastMessage == last && run.lastLink == messages.at(last).links.size() - 1;

        Album album;
        album.albumId = run.id;
        album.members = run.members;
        if (first && atEnd) {
            // Still arriving: every position gets its tile now, so the grid keeps its size.
            for (int i = 0; i < run.count; ++i)
                album.keys.append(QString());
            for (const auto& member : qAsConst(run.members)) {
                const Link& link            = messages.at(member.first).links.at(member.second);
                album.keys[link.index - 1] = link.key;
            }
            album.open = album.keys.contains(QString());
        } else {
            QVector<QPair<int, QString>> present;
            for (const auto& member : qAsConst(run.members)) {
                const Link& link = messages.at(member.first).links.at(member.second);
                present.append({link.index, link.key});
            }
            std::sort(present.begin(), present.end(), [](const QPair<int, QString>& a, const QPair<int, QString>& b) { return a.first < b.first; });
            for (const auto& item : qAsConst(present))
                album.keys.append(item.second);
        }
        if (album.keys.size() >= 2) {
            album.anchorMessage = run.members.first().first;
            for (const auto& member : qAsConst(run.members)) {
                if (member.first == album.anchorMessage)
                    album.anchorLink = member.second;
                else if (!album.hiddenMessages.contains(member.first))
                    album.hiddenMessages.append(member.first);
            }
            albums.append(album);
        }
        run = Run();
    };

    for (int m = 0; m < messages.size(); ++m) {
        const Message& message = messages.at(m);
        for (int l = 0; l < message.links.size(); ++l) {
            const Link& link   = message.links.at(l);
            const bool  member = isMember(link);
            if (run.id != 0 && member && continues(run, messages, m, l)) {
                run.members.append({m, l});
                run.indexes.append(link.index);
                run.lastMessage = m;
                run.lastLink    = l;
                continue;
            }
            close();
            if (!member)
                continue;
            run.id          = link.albumId;
            run.count       = link.count;
            run.sender      = message.sender;
            run.members     = {{m, l}};
            run.indexes     = {link.index};
            run.lastMessage = m;
            run.lastLink    = l;
        }
    }
    close();
    return albums;
}

QHash<int, QSet<QString>> memberKeys(const QVector<Album>& albums, const QVector<Message>& messages)
{
    QHash<int, QSet<QString>> keys;
    for (const Album& album : albums) {
        for (const auto& member : album.members) {
            if (member.first < 0 || member.first >= messages.size())
                continue;
            const QVector<Link>& links = messages.at(member.first).links;
            if (member.second >= 0 && member.second < links.size() && !links.at(member.second).key.isEmpty())
                keys[member.first].insert(links.at(member.second).key);
        }
    }
    return keys;
}

GridEdits gridEdits(const QVector<GridPlace>& existing, const QVector<GridPlace>& wanted)
{
    // Counted per (message, id): the same grid wanted twice in a message is there twice.
    QHash<QPair<int, QString>, int> missing;
    for (const GridPlace& place : wanted)
        ++missing[qMakePair(place.block, place.id)];
    GridEdits edits;
    for (int i = 0; i < existing.size(); ++i) {
        const auto it = missing.find(qMakePair(existing.at(i).block, existing.at(i).id));
        if (it != missing.end() && it.value() > 0) {
            --it.value(); // kept where it is
            continue;
        }
        edits.remove.append(i);
    }
    for (int i = 0; i < wanted.size(); ++i) {
        const auto it = missing.find(qMakePair(wanted.at(i).block, wanted.at(i).id));
        if (it != missing.end() && it.value() > 0) {
            --it.value();
            edits.insert.append(i);
        }
    }
    return edits;
}

// ============================================================================================
// Header
// ============================================================================================

namespace {

QString nickFrom(const QString& quoted)
{
    QString nick = quoted;
    if (nick.size() >= 2 && nick.startsWith(QLatin1Char('"')) && nick.endsWith(QLatin1Char('"')))
        nick = nick.mid(1, nick.size() - 2);
    if (nick.isEmpty() || nick.size() > 64)
        return {};
    for (const QChar c : nick) {
        if (c == QLatin1Char('\n') || c == QChar::LineSeparator || c == QChar::ParagraphSeparator || c == QChar::ObjectReplacementCharacter)
            return {};
    }
    return nick;
}

} // namespace

Header parseHeader(const QString& before, const QString& nickText, const QString& nickHref, const QString& after)
{
    // The icon (at most two), the optional time and spaces; never text of the message.
    static const QRegularExpression lead(QStringLiteral("^\\x{FFFC}{0,2}[ \\t]*(?:<[^<>\\n\\x{2028}\\x{2029}]{1,24}>)?[ \\t]*$"));
    static const QRegularExpression colon(QStringLiteral("^:[ \\t]?"));
    Header                          header;
    const QString                   nick = nickFrom(nickText);
    if (nick.isEmpty() || !lead.match(before).hasMatch() || !nickHref.startsWith(QLatin1String("client://"), Qt::CaseInsensitive))
        return header;
    const QRegularExpressionMatch match = colon.match(after);
    if (!match.hasMatch())
        return header;
    header.valid  = true;
    header.nick   = nick;
    header.uid    = uidFromClientHref(nickHref);
    header.length = before.size() + nickText.size() + match.capturedLength();
    return header;
}

Header parseHeaderText(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^\\x{FFFC}{0,2}[ \\t]*(?:<[^<>\\n\\x{2028}\\x{2029}]{1,24}>)?[ \\t]*\"((?:(?!\"[ \\t]*:)[^\\n\\x{2028}\\x{2029}]){1,64})\"[ \\t]*:[ \\t]?"));
    Header                          header;
    const QRegularExpressionMatch   match = re.match(text);
    if (!match.hasMatch())
        return header;
    header.nick = nickFrom(match.captured(1));
    if (header.nick.isEmpty())
        return header;
    header.valid  = true;
    header.length = match.capturedLength();
    return header;
}

QString uidFromClientHref(const QString& href)
{
    static const QRegularExpression link(QStringLiteral("^client://\\d{1,5}/([^~]{1,128})~"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression uid(QStringLiteral("^[A-Za-z0-9+/=_.\\-]{1,64}$"));
    const QRegularExpressionMatch   match = link.match(href);
    if (!match.hasMatch())
        return {};
    const QString value = QUrl::fromPercentEncoding(match.captured(1).toUtf8());
    return uid.match(value).hasMatch() ? value : QString();
}

// ============================================================================================
// Object names
// ============================================================================================

QString objectId(quint32 albumId, const QStringList& keys)
{
    QString id = QStringLiteral("album.") + hex8(albumId);
    for (const QString& key : keys) {
        id += QLatin1Char('.');
        id += key.isEmpty() ? QStringLiteral("-") : key;
    }
    return id;
}

bool isObjectId(const QString& id)
{
    return id.startsWith(QLatin1String("album."));
}

bool parseObjectId(const QString& id, quint32* albumId, QStringList* keys)
{
    if (!isObjectId(id))
        return false;
    const QStringList parts = id.split(QLatin1Char('.'));
    if (parts.size() < 3 || parts.size() > 2 + kMaxItems || parts.at(1).size() != 8)
        return false;
    bool          ok = false;
    const quint32 value = parts.at(1).toUInt(&ok, 16);
    if (!ok || value == 0 || hex8(value) != parts.at(1))
        return false;
    QStringList found;
    for (int i = 2; i < parts.size(); ++i) {
        const QString& part = parts.at(i);
        if (part == QLatin1String("-"))
            found.append(QString());
        else if (isKey(part))
            found.append(part);
        else
            return false;
    }
    if (albumId)
        *albumId = value;
    if (keys)
        *keys = found;
    return true;
}

// ============================================================================================
// Registry
// ============================================================================================

QString Registry::idOf(const QString& serverUid, quint32 albumId)
{
    return serverUid + QLatin1Char('\n') + hex8(albumId);
}

void Registry::note(const QString& serverUid, quint32 albumId, int index, int count, const QString& key, const QString& sender)
{
    if (albumId == 0 || count < 2 || count > kMaxItems || index < 1 || index > count || key.isEmpty() || sender.isEmpty())
        return;
    const QString id = idOf(serverUid, albumId);
    auto          it = m_albums.find(id);
    if (it == m_albums.end()) {
        while (m_albums.size() >= kMaxAlbums && !m_order.isEmpty())
            m_albums.remove(m_order.takeFirst());
        Entry entry;
        entry.sender = sender;
        entry.count  = count;
        for (int i = 0; i < count; ++i)
            entry.keys.append(QString());
        it = m_albums.insert(id, entry);
    } else {
        if (it->sender != sender || it->count != count)
            return; // someone else's album, or not the same album
        m_order.removeOne(id);
    }
    m_order.append(id);
    if (it->keys.at(index - 1).isEmpty())
        it->keys[index - 1] = key;
}

QString Registry::senderOf(const QString& serverUid, quint32 albumId, int index, const QString& key) const
{
    const Entry* entry = find(serverUid, albumId);
    if (!entry || index < 1 || index > entry->keys.size() || key.isEmpty() || entry->keys.at(index - 1) != key)
        return {};
    return entry->sender;
}

const Registry::Entry* Registry::find(const QString& serverUid, quint32 albumId) const
{
    auto it = m_albums.constFind(idOf(serverUid, albumId));
    return it == m_albums.constEnd() ? nullptr : &it.value();
}

void Registry::clear()
{
    m_albums.clear();
    m_order.clear();
}

} // namespace albums
