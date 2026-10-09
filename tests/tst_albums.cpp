// Unit tests for the 2.2 album grid (src/albums.*): grid geometry and hit testing, grouping chat
// messages into albums, TeamSpeak's message header, the album object names and the sender registry,
// and the way from the composer to one album on the receiving side.

#include <QtTest>

#include "albums.h"
#include "medialink.h"
#include "testmain.h"

namespace {

albums::Link item(const QString& key, quint32 album, int index, int count)
{
    albums::Link link;
    link.key     = key;
    link.albumId = album;
    link.index   = index;
    link.count   = count;
    return link;
}

albums::Link single(const QString& key)
{
    albums::Link link;
    link.key = key;
    return link;
}

albums::Message message(const QString& sender, bool bare, const QVector<albums::Link>& links)
{
    albums::Message m;
    m.sender = sender;
    m.bare   = bare;
    m.links  = links;
    return m;
}

// A key-shaped value (20 lower-case hex digits) for object names.
QString keyOf(int n)
{
    return QStringLiteral("%1").arg(n, 20, 16, QLatin1Char('0'));
}

// QVector comparisons pull in a deprecated MSVC iterator (warning C4996): compare as text.
QString numbers(const QVector<int>& values)
{
    QStringList parts;
    for (const int v : values)
        parts << QString::number(v);
    return parts.join(QLatin1Char(','));
}

QString rectText(const QRect& r)
{
    return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

MediaLink photoLink(const QString& name, int index, int count, quint32 album)
{
    MediaLink link;
    link.host       = QStringLiteral("ts.example.org");
    link.port       = 9987;
    link.serverUid  = QStringLiteral("gR2y8uMbs6HnE5dpkXbM0cMYkO0=");
    link.channelId  = 12;
    link.path       = QStringLiteral("/tsmedia");
    link.fileName   = name;
    link.size       = 845221 + static_cast<quint64>(index);
    link.dateTime   = 1760000000;
    link.protocol   = MediaLink::kProtocol;
    link.width      = 4000;
    link.height     = 3000;
    link.blurHash   = QStringLiteral("LEHV6nWB2yk8pyo0adR*.7kCMdnj");
    link.albumId    = album;
    link.albumIndex = index;
    link.albumCount = count;
    return link;
}

} // namespace

class TestAlbums : public QObject
{
    Q_OBJECT

  private slots:
    // geometry
    void layoutDesignSizes();
    void layoutNarrow();
    void layoutInvariants_data();
    void layoutInvariants();
    void tileAtHits();

    // grouping
    void groupOneMessage();
    void groupSeveralMessages();
    void groupOpenWithPlaceholders();
    void groupClosedShrinks();
    void groupSenderRules();
    void groupDuplicatePosition();
    void groupOutOfOrder();
    void groupBackToBack();
    void groupSingleItemIsNoAlbum();
    void groupLaterRunOnlyItsItems();
    void groupBrokenRuns();
    void groupIgnoresInvalidItems();

    // header
    void headerFromFragments_data();
    void headerFromFragments();
    void headerFromText_data();
    void headerFromText();
    void uidFromHref_data();
    void uidFromHref();

    // object names, registry
    void objectIdRoundTrip();
    void objectIdRejects_data();
    void objectIdRejects();
    void registryOwnerAndPositions();
    void registryForgetsOldest();

    // composer -> chat -> album
    void composedAlbumIsOneMessage_data();
    void composedAlbumIsOneMessage();
    void composedAlbumOverSeveralMessages();
};

// ============================================================================================
// Geometry
// ============================================================================================

void TestAlbums::layoutDesignSizes()
{
    // The design's examples at the default limits (400 x 300: height cap 400).
    struct Case {
        int          items;
        QSize        box;
        QList<QRect> tiles;
        int          overflow;
    };
    const QList<Case> cases = {
        {2, QSize(400, 198), {QRect(0, 0, 198, 198), QRect(202, 0, 198, 198)}, 0},
        {3, QSize(400, 264), {QRect(0, 0, 264, 264), QRect(268, 0, 132, 130), QRect(268, 134, 132, 130)}, 0},
        {4, QSize(400, 400), {QRect(0, 0, 198, 198), QRect(202, 0, 198, 198), QRect(0, 202, 198, 198), QRect(202, 202, 198, 198)}, 0},
        {5, QSize(400, 333), {QRect(0, 0, 198, 198), QRect(202, 0, 198, 198), QRect(0, 202, 130, 131), QRect(134, 202, 131, 131), QRect(269, 202, 131, 131)}, 0},
        {6, QSize(400, 266), {QRect(0, 0, 130, 131), QRect(134, 0, 131, 131), QRect(269, 0, 131, 131), QRect(0, 135, 130, 131), QRect(134, 135, 131, 131), QRect(269, 135, 131, 131)}, 0},
    };
    for (const Case& c : cases) {
        const albums::Geometry g = albums::layout(c.items, 400, 300);
        QCOMPARE(g.box, c.box);
        QCOMPARE(g.overflow, c.overflow);
        QCOMPARE(g.tiles.size(), c.tiles.size());
        for (int i = 0; i < c.tiles.size(); ++i)
            QCOMPARE(rectText(g.tiles.at(i)), rectText(c.tiles.at(i)));
    }
    // 7 to 10: the six-tile grid, "+N" on the sixth (it stands for its own item too).
    for (int items = 7; items <= 10; ++items) {
        const albums::Geometry g = albums::layout(items, 400, 300);
        QCOMPARE(g.box, QSize(400, 266));
        QCOMPARE(g.tiles.size(), 6);
        QCOMPARE(g.overflow, items - 5);
    }
    // A lower height limit scales the rows down together (4: 2 x 198 + 4 > 267).
    const albums::Geometry low = albums::layout(4, 400, 200);
    QCOMPARE(low.box, QSize(400, 267));
    QCOMPARE(low.tiles.at(0).height() + low.tiles.at(2).height() + albums::kGap, 267);
    QVERIFY(qAbs(low.tiles.at(0).height() - low.tiles.at(2).height()) <= 1);
}

void TestAlbums::layoutNarrow()
{
    // Below 224 px: at most two columns and four tiles.
    const albums::Geometry three = albums::layout(3, 200, 300);
    QCOMPARE(three.tiles.size(), 3);
    QCOMPARE(rectText(three.tiles.at(0)), rectText(QRect(0, 0, 200, 113))); // 16:9 top row
    QCOMPARE(rectText(three.tiles.at(1)), rectText(QRect(0, 117, 98, 98)));
    QCOMPARE(rectText(three.tiles.at(2)), rectText(QRect(102, 117, 98, 98)));
    QCOMPARE(three.box, QSize(200, 215));

    const albums::Geometry five = albums::layout(5, 200, 300);
    QCOMPARE(five.tiles.size(), 4);
    QCOMPARE(five.overflow, 2); // "+2" on the fourth tile
    QCOMPARE(five.box, QSize(200, 200));

    // The smallest setting (120 px wide) keeps usable tiles.
    const albums::Geometry smallest = albums::layout(10, 120, 80);
    QCOMPARE(smallest.tiles.size(), 4);
    QCOMPARE(smallest.overflow, 7);
    for (const QRect& tile : smallest.tiles)
        QVERIFY(tile.width() >= albums::kMinTile && tile.height() >= albums::kMinTile);

    // 223 is narrow, 224 is not.
    QCOMPARE(albums::layout(6, 223, 300).tiles.size(), 4);
    QCOMPARE(albums::layout(6, 224, 300).tiles.size(), 6);
}

void TestAlbums::layoutInvariants_data()
{
    QTest::addColumn<int>("items");
    QTest::addColumn<int>("maxWidth");
    QTest::addColumn<int>("maxHeight");
    for (int items = 2; items <= 10; ++items) {
        for (const int width : {1200, 400, 300, 224, 223, 160, 120}) {
            for (const int height : {1200, 300, 200, 80})
                QTest::addRow("%d items %dx%d", items, width, height) << items << width << height;
        }
    }
}

void TestAlbums::layoutInvariants()
{
    QFETCH(int, items);
    QFETCH(int, maxWidth);
    QFETCH(int, maxHeight);
    const albums::Geometry g      = albums::layout(items, maxWidth, maxHeight);
    const bool             narrow = maxWidth < albums::kNarrowWidth;
    const int              shown  = qMin(items, narrow ? albums::kMaxTilesNarrow : albums::kMaxTiles);

    QCOMPARE(g.box.width(), maxWidth);
    QVERIFY2(g.box.height() <= qRound(maxHeight * 4.0 / 3.0), qPrintable(QStringLiteral("height %1").arg(g.box.height())));
    QCOMPARE(g.tiles.size(), shown);
    QCOMPARE(g.overflow, items > shown ? items - shown + 1 : 0);

    const QRect box(QPoint(0, 0), g.box);
    for (int i = 0; i < g.tiles.size(); ++i) {
        const QRect& t = g.tiles.at(i);
        QVERIFY2(box.contains(t), qPrintable(rectText(t)));
        QVERIFY2(t.width() >= albums::kMinTile && t.height() >= albums::kMinTile, qPrintable(rectText(t)));
        for (int j = i + 1; j < g.tiles.size(); ++j)
            QVERIFY2(!t.intersects(g.tiles.at(j)), qPrintable(rectText(t) + QStringLiteral(" / ") + rectText(g.tiles.at(j))));
        // Every tile touches the box edge or is exactly one gap away from a neighbour, on each side.
        auto neighbourRight = [&] {
            for (const QRect& o : g.tiles) {
                if (o.x() == t.x() + t.width() + albums::kGap && o.top() <= t.bottom() && o.bottom() >= t.top())
                    return true;
            }
            return false;
        };
        auto neighbourBelow = [&] {
            for (const QRect& o : g.tiles) {
                if (o.y() == t.y() + t.height() + albums::kGap && o.left() <= t.right() && o.right() >= t.left())
                    return true;
            }
            return false;
        };
        QVERIFY2(t.x() + t.width() == g.box.width() || neighbourRight(), qPrintable(rectText(t)));
        QVERIFY2(t.y() + t.height() == g.box.height() || neighbourBelow(), qPrintable(rectText(t)));
    }
    // The tiles and the gaps fill the box: no empty corner.
    qint64 area = 0;
    for (const QRect& t : g.tiles)
        area += static_cast<qint64>(t.width()) * t.height();
    QVERIFY(area <= static_cast<qint64>(g.box.width()) * g.box.height());
    QVERIFY(area >= static_cast<qint64>(g.box.width() - 2 * albums::kGap) * (g.box.height() - albums::kGap) * 9 / 10);

    // Deterministic.
    const albums::Geometry again = albums::layout(items, maxWidth, maxHeight);
    QCOMPARE(again.box, g.box);
    QCOMPARE(again.tiles.size(), g.tiles.size());
    for (int i = 0; i < g.tiles.size(); ++i)
        QCOMPARE(again.tiles.at(i), g.tiles.at(i));
}

void TestAlbums::tileAtHits()
{
    for (const int items : {2, 3, 4, 5, 6, 8}) {
        const albums::Geometry g = albums::layout(items, 400, 300);
        for (int i = 0; i < g.tiles.size(); ++i) {
            const QRectF t(g.tiles.at(i));
            QCOMPARE(albums::tileAt(g, t.center()), i);
            QCOMPARE(albums::tileAt(g, t.topLeft()), i);    // edges belong to the tile
            QCOMPARE(albums::tileAt(g, t.bottomRight()), i);
            // The middle of the gap to the right (when there is a neighbour there) is no tile.
            const QPointF gap(t.right() + albums::kGap / 2.0, t.center().y());
            if (gap.x() < g.box.width())
                QCOMPARE(albums::tileAt(g, gap), -1);
        }
        QCOMPARE(albums::tileAt(g, QPointF(-1, 10)), -1);
        QCOMPARE(albums::tileAt(g, QPointF(10, g.box.height() + 1)), -1);
    }
}

// ============================================================================================
// Grouping
// ============================================================================================

void TestAlbums::groupOneMessage()
{
    const quint32                  a = 0x7c1e09ab;
    const QVector<albums::Message> chat = {
        message(QStringLiteral("u:alice"), false, {}),
        message(QStringLiteral("u:alice"), false, {item(QStringLiteral("k1"), a, 1, 4), item(QStringLiteral("k2"), a, 2, 4), item(QStringLiteral("k3"), a, 3, 4), item(QStringLiteral("k4"), a, 4, 4)}),
    };
    const QVector<albums::Album> result = albums::group(chat);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.at(0).albumId, a);
    QCOMPARE(result.at(0).keys, QStringList({QStringLiteral("k1"), QStringLiteral("k2"), QStringLiteral("k3"), QStringLiteral("k4")}));
    QCOMPARE(result.at(0).anchorMessage, 1);
    QCOMPARE(result.at(0).anchorLink, 3);
    QVERIFY(result.at(0).hiddenMessages.isEmpty());
    QVERIFY(!result.at(0).open);
    QCOMPARE(result.at(0).members.size(), 4);
}

void TestAlbums::groupSeveralMessages()
{
    const quint32                  a = 0x11;
    const QVector<albums::Message> chat = {
        message(QStringLiteral("u:alice"), false, {item(QStringLiteral("k1"), a, 1, 5), item(QStringLiteral("k2"), a, 2, 5)}), // with a caption
        message(QStringLiteral("u:alice"), true, {item(QStringLiteral("k3"), a, 3, 5), item(QStringLiteral("k4"), a, 4, 5)}),
        message(QStringLiteral("u:alice"), true, {item(QStringLiteral("k5"), a, 5, 5)}),
        message(QStringLiteral("u:bob"), false, {}),
    };
    const QVector<albums::Album> result = albums::group(chat);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.at(0).keys.size(), 5);
    QCOMPARE(result.at(0).anchorMessage, 0);
    QCOMPARE(result.at(0).anchorLink, 1);
    QCOMPARE(numbers(result.at(0).hiddenMessages), QStringLiteral("1,2"));
    QVERIFY(!result.at(0).open);
}

void TestAlbums::groupOpenWithPlaceholders()
{
    const quint32                  a = 0x22;
    const QVector<albums::Message> chat = {
        message(QStringLiteral("u:x"), false, {}),
        message(QStringLiteral("u:alice"), false, {item(QStringLiteral("k1"), a, 1, 4), item(QStringLiteral("k2"), a, 2, 4)}),
    };
    const QVector<albums::Album> result = albums::group(chat);
    QCOMPARE(result.size(), 1);
    QVERIFY(result.at(0).open);
    QCOMPARE(result.at(0).keys, QStringList({QStringLiteral("k1"), QStringLiteral("k2"), QString(), QString()}));

    // The rest arrives in the next message: the same grid, now complete, and that message is hidden.
    QVector<albums::Message> later = chat;
    later.append(message(QStringLiteral("u:alice"), true, {item(QStringLiteral("k3"), a, 3, 4), item(QStringLiteral("k4"), a, 4, 4)}));
    const QVector<albums::Album> complete = albums::group(later);
    QCOMPARE(complete.size(), 1);
    QVERIFY(!complete.at(0).open);
    QCOMPARE(complete.at(0).keys.size(), 4);
    QVERIFY(!complete.at(0).keys.contains(QString()));
    QCOMPARE(numbers(complete.at(0).hiddenMessages), QStringLiteral("2"));

    // One item of three at the end: still an album (it is waiting for two more).
    const QVector<albums::Album> lone = albums::group({message(QStringLiteral("u:a"), false, {item(QStringLiteral("k1"), a, 1, 3)})});
    QCOMPARE(lone.size(), 1);
    QCOMPARE(lone.at(0).keys, QStringList({QStringLiteral("k1"), QString(), QString()}));
}

void TestAlbums::groupClosedShrinks()
{
    const quint32                  a = 0x33;
    const QVector<albums::Message> chat = {
        message(QStringLiteral("u:alice"), false, {item(QStringLiteral("k1"), a, 1, 4), item(QStringLiteral("k3"), a, 3, 4)}),
        message(QStringLiteral("u:carol"), false, {}), // "nice"
    };
    const QVector<albums::Album> result = albums::group(chat);
    QCOMPARE(result.size(), 1);
    QVERIFY(!result.at(0).open);
    QCOMPARE(result.at(0).keys, QStringList({QStringLiteral("k1"), QStringLiteral("k3")}));
}

void TestAlbums::groupSenderRules()
{
    const quint32 a = 0x44;
    auto          two = [&](const QString& first, const QString& second) {
        return albums::group({
            message(first, false, {item(QStringLiteral("k1"), a, 1, 4), item(QStringLiteral("k2"), a, 2, 4)}),
            message(second, true, {item(QStringLiteral("k3"), a, 3, 4), item(QStringLiteral("k4"), a, 4, 4)}),
        });
    };
    // Same unique id, or the same nickname (after a plugin reload): one album.
    QCOMPARE(two(QStringLiteral("u:alice"), QStringLiteral("u:alice")).size(), 1);
    QCOMPARE(two(QStringLiteral("n:Alice"), QStringLiteral("n:Alice")).size(), 1);
    // Someone else, or nobody known: the second message forms its own album of its items.
    for (const auto& pair : {qMakePair(QStringLiteral("u:alice"), QStringLiteral("u:mallory")), qMakePair(QString(), QString()),
                             qMakePair(QStringLiteral("u:alice"), QStringLiteral("n:Alice")), qMakePair(QStringLiteral("n:Alice"), QStringLiteral("n:Bob"))}) {
        const QVector<albums::Album> result = two(pair.first, pair.second);
        QCOMPARE(result.size(), 2);
        QCOMPARE(result.at(0).keys, QStringList({QStringLiteral("k1"), QStringLiteral("k2")}));
        QCOMPARE(result.at(1).keys, QStringList({QStringLiteral("k3"), QStringLiteral("k4")}));
        QVERIFY(result.at(1).hiddenMessages.isEmpty());
        QCOMPARE(result.at(1).anchorMessage, 1);
    }
}

void TestAlbums::groupDuplicatePosition()
{
    const quint32                a      = 0x55;
    const QVector<albums::Album> result = albums::group({
        message(QStringLiteral("u:a"), false, {item(QStringLiteral("k1"), a, 1, 3), item(QStringLiteral("k2"), a, 2, 3), item(QStringLiteral("x2"), a, 2, 3), item(QStringLiteral("k3"), a, 3, 3)}),
    });
    // The repeated position ends the first run; the second run lays out only its own items.
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.at(0).keys, QStringList({QStringLiteral("k1"), QStringLiteral("k2")}));
    QCOMPARE(result.at(1).keys, QStringList({QStringLiteral("x2"), QStringLiteral("k3")}));
    QCOMPARE(result.at(0).anchorLink, 1);
    QCOMPARE(result.at(1).anchorLink, 3);
}

void TestAlbums::groupOutOfOrder()
{
    const quint32                a      = 0x66;
    const QVector<albums::Album> result = albums::group({
        message(QStringLiteral("u:a"), false, {item(QStringLiteral("k3"), a, 3, 3), item(QStringLiteral("k1"), a, 1, 3), item(QStringLiteral("k2"), a, 2, 3)}),
        message(QStringLiteral("u:b"), false, {}),
    });
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.at(0).keys, QStringList({QStringLiteral("k1"), QStringLiteral("k2"), QStringLiteral("k3")})); // by position
    QCOMPARE(result.at(0).anchorLink, 2); // after the last of them in the message
}

void TestAlbums::groupBackToBack()
{
    const QVector<albums::Album> result = albums::group({
        message(QStringLiteral("u:a"), false, {item(QStringLiteral("a1"), 1, 1, 2), item(QStringLiteral("a2"), 1, 2, 2)}),
        message(QStringLiteral("u:a"), false, {item(QStringLiteral("b1"), 2, 1, 2), item(QStringLiteral("b2"), 2, 2, 2)}),
    });
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.at(0).albumId, 1u);
    QCOMPARE(result.at(1).albumId, 2u);
    QCOMPARE(result.at(1).anchorMessage, 1);
}

void TestAlbums::groupSingleItemIsNoAlbum()
{
    const QVector<albums::Album> result = albums::group({
        message(QStringLiteral("u:a"), false, {item(QStringLiteral("k1"), 7, 1, 2)}),
        message(QStringLiteral("u:b"), false, {}),
    });
    QVERIFY(result.isEmpty());
}

void TestAlbums::groupLaterRunOnlyItsItems()
{
    const quint32                a      = 0x77;
    const QVector<albums::Album> result = albums::group({
        message(QStringLiteral("u:a"), false, {item(QStringLiteral("k1"), a, 1, 4), item(QStringLiteral("k2"), a, 2, 4)}),
        message(QStringLiteral("u:c"), false, {}),
        message(QStringLiteral("u:a"), true, {item(QStringLiteral("k3"), a, 3, 4), item(QStringLiteral("k4"), a, 4, 4)}),
    });
    // The later run ends the chat but is not the album's first: no placeholders.
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.at(0).keys, QStringList({QStringLiteral("k1"), QStringLiteral("k2")}));
    QCOMPARE(result.at(1).keys, QStringList({QStringLiteral("k3"), QStringLiteral("k4")}));
    QVERIFY(!result.at(1).open);
}

void TestAlbums::groupBrokenRuns()
{
    const quint32 a = 0x88;
    // A file that is not part of the album between its items.
    QVERIFY(albums::group({message(QStringLiteral("u:a"), false, {item(QStringLiteral("k1"), a, 1, 2), single(QStringLiteral("x")), item(QStringLiteral("k2"), a, 2, 2)})}).isEmpty());
    // The next message is not bare (text in front of its link) or carries another file: not continued.
    for (const bool foreign : {false, true}) {
        QVector<albums::Link> second = {item(QStringLiteral("k3"), a, 3, 4), item(QStringLiteral("k4"), a, 4, 4)};
        if (foreign)
            second.append(single(QStringLiteral("x")));
        const QVector<albums::Album> result = albums::group({
            message(QStringLiteral("u:a"), false, {item(QStringLiteral("k1"), a, 1, 4), item(QStringLiteral("k2"), a, 2, 4)}),
            message(QStringLiteral("u:a"), foreign, second),
        });
        QCOMPARE(result.size(), 2);
        QVERIFY(result.at(0).hiddenMessages.isEmpty());
    }
    // A different size for the same id ends the run.
    QVERIFY(albums::group({message(QStringLiteral("u:a"), false, {item(QStringLiteral("k1"), a, 1, 2), item(QStringLiteral("k2"), a, 2, 3)}),
                           message(QStringLiteral("u:b"), false, {})})
                .isEmpty());
}

void TestAlbums::groupIgnoresInvalidItems()
{
    // Out of range positions and sizes are no album items (the link parser drops them already).
    const QVector<albums::Album> result = albums::group({
        message(QStringLiteral("u:a"), false,
                {item(QStringLiteral("k1"), 9, 0, 2), item(QStringLiteral("k2"), 9, 3, 2), item(QStringLiteral("k3"), 9, 1, 11), item(QString(), 9, 1, 2), item(QStringLiteral("k5"), 9, 1, 1)}),
    });
    QVERIFY(result.isEmpty());
}

// ============================================================================================
// Header
// ============================================================================================

void TestAlbums::headerFromFragments_data()
{
    QTest::addColumn<QString>("before");
    QTest::addColumn<QString>("nickText");
    QTest::addColumn<QString>("href");
    QTest::addColumn<QString>("after");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QString>("nick");
    QTest::addColumn<QString>("uid");
    QTest::addColumn<int>("length");

    const QString icon = QString(QChar::ObjectReplacementCharacter);
    const QString href = QStringLiteral("client://17/KpNqZMq7js/JajRo+3zOFPViX1E=~TesterB");
    const QString uid  = QStringLiteral("KpNqZMq7js/JajRo+3zOFPViX1E=");
    // S0: icon, "<20:02:13>", " ", "\"TesterB\"" (the link), ": ", the message.
    QTest::newRow("S0 header") << icon + QStringLiteral("<20:02:13> ") << QStringLiteral("\"TesterB\"") << href << QStringLiteral(": hello") << true << "TesterB" << uid << 23;
    QTest::newRow("no time") << icon << QStringLiteral("\"TesterB\"") << href << QStringLiteral(": x") << true << "TesterB" << uid << 12;
    QTest::newRow("12 h time") << icon + QStringLiteral("<9:05:01 PM> ") << QStringLiteral("\"Ali: \"x\"\"") << QStringLiteral("client://3/abc=~Ali") << QStringLiteral(":")
                               << true << "Ali: \"x\"" << "abc=" << 25;
    QTest::newRow("RTL nick") << icon + QStringLiteral("<21:14:05> ") << QStringLiteral("\"مهدی\"") << QStringLiteral("client://5/xyz=~x") << QStringLiteral(": سلام") << true
                              << "مهدی" << "xyz=" << 20;
    QTest::newRow("text in front") << QStringLiteral("hi ") << QStringLiteral("\"TesterB\"") << href << QStringLiteral(": x") << false << "" << "" << 0;
    QTest::newRow("no colon") << icon << QStringLiteral("\"TesterB\"") << href << QStringLiteral(" says") << false << "" << "" << 0;
    QTest::newRow("not a client link") << icon << QStringLiteral("\"TesterB\"") << QStringLiteral("https://x/~a") << QStringLiteral(": x") << false << "" << "" << 0;
    QTest::newRow("nick too long") << icon << QStringLiteral("\"%1\"").arg(QString(65, QLatin1Char('a'))) << href << QStringLiteral(": x") << false << "" << "" << 0;
    QTest::newRow("empty nick") << icon << QStringLiteral("\"\"") << href << QStringLiteral(": x") << false << "" << "" << 0;
    QTest::newRow("line break in front") << icon + QChar(QChar::LineSeparator) << QStringLiteral("\"TesterB\"") << href << QStringLiteral(": x") << false << "" << "" << 0;
}

void TestAlbums::headerFromFragments()
{
    QFETCH(QString, before);
    QFETCH(QString, nickText);
    QFETCH(QString, href);
    QFETCH(QString, after);
    QFETCH(bool, valid);
    QFETCH(QString, nick);
    QFETCH(QString, uid);
    QFETCH(int, length);
    const albums::Header h = albums::parseHeader(before, nickText, href, after);
    QCOMPARE(h.valid, valid);
    QCOMPARE(h.nick, nick);
    QCOMPARE(h.uid, uid);
    QCOMPARE(h.length, length);
}

void TestAlbums::headerFromText_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QString>("nick");
    QTest::addColumn<QString>("rest"); // the message text after the header

    const QString icon = QString(QChar::ObjectReplacementCharacter);
    QTest::newRow("time and nick") << QStringLiteral("<21:14:05> \"Alice\": ") << true << "Alice" << "";
    QTest::newRow("icon") << icon + QStringLiteral("<21:14:05> \"Alice\": x") << true << "Alice" << "x";
    QTest::newRow("no time") << QStringLiteral("\"Alice\": ") << true << "Alice" << "";
    QTest::newRow("quotes in nick") << QStringLiteral("<9:05:01 PM> \"Ali: \"x\"\": ") << true << "Ali: \"x\"" << "";
    QTest::newRow("caption") << QStringLiteral("<21:14:05> \"Alice\": Road trip! ") << true << "Alice" << "Road trip! ";
    QTest::newRow("fake header in message") << QStringLiteral("\"Bob\": hi \"x\": ") << true << "Bob" << "hi \"x\": ";
    QTest::newRow("nick too long") << QStringLiteral("\"%1\": ").arg(QString(65, QLatin1Char('a'))) << false << "" << "";
    QTest::newRow("empty") << QString() << false << "" << "";
    QTest::newRow("plain text") << QStringLiteral("hello there") << false << "" << "";
}

void TestAlbums::headerFromText()
{
    QFETCH(QString, text);
    QFETCH(bool, valid);
    QFETCH(QString, nick);
    QFETCH(QString, rest);
    const albums::Header h = albums::parseHeaderText(text);
    QCOMPARE(h.valid, valid);
    QCOMPARE(h.nick, nick);
    if (valid)
        QCOMPARE(text.mid(h.length), rest);
}

void TestAlbums::uidFromHref_data()
{
    QTest::addColumn<QString>("href");
    QTest::addColumn<QString>("uid");
    QTest::newRow("S0") << QStringLiteral("client://17/KpNqZMq7js/JajRo+3zOFPViX1E=~TesterB") << QStringLiteral("KpNqZMq7js/JajRo+3zOFPViX1E=");
    QTest::newRow("percent-encoded") << QStringLiteral("client://17/KpNq%2BZ%3D~x") << QStringLiteral("KpNq+Z=");
    QTest::newRow("upper-case scheme") << QStringLiteral("CLIENT://2/abc=~n") << QStringLiteral("abc=");
    QTest::newRow("no tilde") << QStringLiteral("client://17/abc=") << QString();
    QTest::newRow("no client id") << QStringLiteral("client:///abc=~n") << QString();
    QTest::newRow("other scheme") << QStringLiteral("ts3file://17/abc=~n") << QString();
    QTest::newRow("space in id") << QStringLiteral("client://17/ab c~n") << QString();
    QTest::newRow("too long") << QStringLiteral("client://17/%1~n").arg(QString(65, QLatin1Char('a'))) << QString();
}

void TestAlbums::uidFromHref()
{
    QFETCH(QString, href);
    QFETCH(QString, uid);
    QCOMPARE(albums::uidFromClientHref(href), uid);
}

// ============================================================================================
// Object names, registry
// ============================================================================================

void TestAlbums::objectIdRoundTrip()
{
    const QStringList keys = {keyOf(1), QString(), keyOf(3)};
    const QString     id    = albums::objectId(0x0000beef, keys);
    QCOMPARE(id, QStringLiteral("album.0000beef.%1.-.%2").arg(keyOf(1), keyOf(3)));
    QVERIFY(albums::isObjectId(id));
    quint32     album = 0;
    QStringList parsed;
    QVERIFY(albums::parseObjectId(id, &album, &parsed));
    QCOMPARE(album, 0x0000beefu);
    QCOMPARE(parsed, keys);
    // Never mistaken for a single preview's key (20 hex digits).
    QVERIFY(!albums::isObjectId(keyOf(5)));
}

void TestAlbums::objectIdRejects_data()
{
    QTest::addColumn<QString>("id");
    QTest::newRow("no keys") << QStringLiteral("album.0000beef");
    QTest::newRow("zero id") << QStringLiteral("album.00000000.%1").arg(keyOf(1));
    QTest::newRow("short id") << QStringLiteral("album.beef.%1").arg(keyOf(1));
    QTest::newRow("upper-case id") << QStringLiteral("album.0000BEEF.%1").arg(keyOf(1));
    QTest::newRow("bad key") << QStringLiteral("album.0000beef.xyz");
    QTest::newRow("upper-case key") << QStringLiteral("album.0000beef.%1").arg(keyOf(0xabc).toUpper());
    QTest::newRow("eleven keys") << QStringLiteral("album.0000beef") + QStringLiteral(".-").repeated(11);
    QTest::newRow("other prefix") << QStringLiteral("albums.0000beef.%1").arg(keyOf(1));
}

void TestAlbums::objectIdRejects()
{
    QFETCH(QString, id);
    QVERIFY(!albums::parseObjectId(id, nullptr, nullptr));
}

void TestAlbums::registryOwnerAndPositions()
{
    albums::Registry r;
    const QString    server = QStringLiteral("srv=");
    r.note(server, 0xa1, 1, 3, QStringLiteral("k1"), QStringLiteral("alice"));
    r.note(server, 0xa1, 2, 3, QStringLiteral("k2"), QStringLiteral("alice"));
    // Someone else using the same album id is ignored; so is a second key for a taken position.
    r.note(server, 0xa1, 3, 3, QStringLiteral("evil"), QStringLiteral("mallory"));
    r.note(server, 0xa1, 2, 3, QStringLiteral("other"), QStringLiteral("alice"));
    r.note(server, 0xa1, 3, 4, QStringLiteral("k3x"), QStringLiteral("alice")); // another size: not this album
    QCOMPARE(r.senderOf(server, 0xa1, 1, QStringLiteral("k1")), QStringLiteral("alice"));
    QCOMPARE(r.senderOf(server, 0xa1, 2, QStringLiteral("k2")), QStringLiteral("alice"));
    QCOMPARE(r.senderOf(server, 0xa1, 2, QStringLiteral("other")), QString());
    QCOMPARE(r.senderOf(server, 0xa1, 3, QStringLiteral("evil")), QString());
    QCOMPARE(r.senderOf(server, 0xa1, 3, QStringLiteral("k3x")), QString());
    r.note(server, 0xa1, 3, 3, QStringLiteral("k3"), QStringLiteral("alice"));
    QCOMPARE(r.senderOf(server, 0xa1, 3, QStringLiteral("k3")), QStringLiteral("alice"));
    // Albums are per server.
    QCOMPARE(r.senderOf(QStringLiteral("other="), 0xa1, 1, QStringLiteral("k1")), QString());
    // Incomplete notes are ignored.
    r.note(server, 0, 1, 2, QStringLiteral("k"), QStringLiteral("a"));
    r.note(server, 0xb1, 0, 2, QStringLiteral("k"), QStringLiteral("a"));
    r.note(server, 0xb1, 1, 11, QStringLiteral("k"), QStringLiteral("a"));
    r.note(server, 0xb1, 1, 2, QString(), QStringLiteral("a"));
    r.note(server, 0xb1, 1, 2, QStringLiteral("k"), QString());
    QCOMPARE(r.size(), 1);
    QVERIFY(r.find(server, 0xa1));
    QCOMPARE(r.find(server, 0xa1)->keys, QStringList({QStringLiteral("k1"), QStringLiteral("k2"), QStringLiteral("k3")}));
}

void TestAlbums::registryForgetsOldest()
{
    albums::Registry r;
    for (quint32 id = 1; id <= albums::Registry::kMaxAlbums; ++id)
        r.note(QStringLiteral("s"), id, 1, 2, QStringLiteral("k"), QStringLiteral("a"));
    QCOMPARE(r.size(), albums::Registry::kMaxAlbums);
    r.note(QStringLiteral("s"), 1, 2, 2, QStringLiteral("k2"), QStringLiteral("a")); // album 1 is used again
    r.note(QStringLiteral("s"), 100000, 1, 2, QStringLiteral("k"), QStringLiteral("a"));
    QCOMPARE(r.size(), albums::Registry::kMaxAlbums);
    QVERIFY(r.find(QStringLiteral("s"), 1));  // recently used: kept
    QVERIFY(!r.find(QStringLiteral("s"), 2)); // the oldest: forgotten
    QVERIFY(r.find(QStringLiteral("s"), 100000));
}

// ============================================================================================
// Composer -> chat -> album
// ============================================================================================

void TestAlbums::composedAlbumIsOneMessage_data()
{
    QTest::addColumn<int>("items");
    QTest::addColumn<QString>("base");
    QTest::addColumn<QString>("caption");
    QTest::newRow("2 photos") << 2 << QStringLiteral("IMG_2041") << QString();
    QTest::newRow("5 photos, caption") << 5 << QStringLiteral("IMG_2041") << QStringLiteral("Road trip! day 2");
    QTest::newRow("10 photos, caption") << 10 << QStringLiteral("IMG_2041") << QString(300, QLatin1Char('x'));
    // The worst case S0 measured: 48-character Persian names.
    QTest::newRow("10 long Persian names") << 10 << QString::fromUtf8("عکس_سفر_شمال_").repeated(4).left(48) << QStringLiteral("سفر");
}

void TestAlbums::composedAlbumIsOneMessage()
{
    QFETCH(int, items);
    QFETCH(QString, base);
    QFETCH(QString, caption);
    // S0: TeamSpeak takes 8192 bytes per message; the composer keeps 24 bytes of room.
    constexpr int kMeasuredLimit = 8192 - 24;

    const quint32    album = 0x7c1e09ab;
    QList<MediaLink> links;
    QStringList      keys;
    for (int i = 1; i <= items; ++i) {
        const MediaLink link = photoLink(base + QStringLiteral("_%1_3f9a1c%2.jpg").arg(i).arg(i, 2, 10, QLatin1Char('0')), i, items, album);
        links.append(link);
        keys.append(link.key());
    }
    ComposeOptions options;
    options.caption  = caption;
    options.maxBytes = kMeasuredLimit;
    const QVector<ComposedMessage> composed = composeChatMessagesDetailed(links, options);
    QCOMPARE(composed.size(), 1);
    QVERIFY(!composed.at(0).tooLong);
    QVERIFY(composed.at(0).dropped.isEmpty()); // nothing left out to fit
    QVERIFY(composed.at(0).text.toUtf8().size() < kMeasuredLimit);

    // The receiver: the links of that one message are one album, in order.
    const QList<MediaLink> found = MediaLink::findInMessage(composed.at(0).text);
    QCOMPARE(found.size(), items);
    albums::Message received;
    received.sender = QStringLiteral("u:alice");
    for (const MediaLink& link : found) {
        QVERIFY(link.hasAlbum());
        received.links.append(item(link.key(), link.albumId, link.albumIndex, link.albumCount));
    }
    const QVector<albums::Album> result = albums::group({received});
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.at(0).albumId, album);
    QCOMPARE(result.at(0).keys, keys);
    QCOMPARE(result.at(0).anchorLink, items - 1);
    QVERIFY(result.at(0).hiddenMessages.isEmpty());
}

void TestAlbums::composedAlbumOverSeveralMessages()
{
    // 2.1's 1000-byte limit packs two links per message: the album still comes out as one grid, and
    // the messages after the first (nothing but more of its items) are hidden.
    const quint32    album = 0x0badcafe;
    QList<MediaLink> links;
    QStringList      keys;
    for (int i = 1; i <= 7; ++i) {
        links.append(photoLink(QStringLiteral("holiday_%1_3f9a1c2e.jpg").arg(i), i, 7, album));
        keys.append(links.last().key());
    }
    ComposeOptions options;
    options.caption  = QStringLiteral("Trip photos");
    options.maxBytes = 1000;
    const QStringList composed = composeChatMessages(links, options);
    QVERIFY(composed.size() >= 3);

    QVector<albums::Message> chat;
    for (int m = 0; m < composed.size(); ++m) {
        albums::Message received;
        received.sender = QStringLiteral("u:alice");
        received.bare   = m > 0; // the first carries the caption
        for (const MediaLink& link : MediaLink::findInMessage(composed.at(m)))
            received.links.append(item(link.key(), link.albumId, link.albumIndex, link.albumCount));
        chat.append(received);
    }
    const QVector<albums::Album> result = albums::group(chat);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.at(0).keys, keys);
    QCOMPARE(result.at(0).anchorMessage, 0);
    QCOMPARE(result.at(0).hiddenMessages.size(), composed.size() - 1);
}

TSMEDIA_REGISTER_TEST(TestAlbums)
#include "tst_albums.moc"
