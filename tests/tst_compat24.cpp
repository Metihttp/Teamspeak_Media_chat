// 2.5 link previews: compatibility with TS Media 2.4.0 (and so 2.3.x, which has no link-preview code at all).
// Every plugin command 2.5 can send about link previews and presence goes through a FROZEN copy of 2.4.0's readers
// (compat24.h, generated from refs/tags/v2.4.0): its tsm1 parser, its HELLO reader, its LP / LPNO readers, the
// link-preview hub's type gate and the store's entry reader. 2.4.0 must parse every one of them (no command it would
// count as malformed), drop LPX and LPXNO at the type gate (nothing stored, nothing downloaded), keep reading the
// capability "l" as 2.4.0 meant it, and read a 2.5 LP exactly as 2.5 does.

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "compat24.h"
#include "linkpreview/announcement.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/previewstore.h"
#include "linkpreview/textclean.h"
#include "peerprotocol.h"
#include "testmain.h"
#include "textreact.h"

using namespace linkpreview;

namespace {

QString shaOf(const QByteArray& bytes)
{
    return shaText(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
}

QByteArray ref()
{
    return textreact::wireRef(QStringLiteral("0123456789abcdef"), 3);
}

Announcement legacy(Kind kind, bool media)
{
    Announcement a;
    a.urlKey      = QStringLiteral("00112233445566778899aabbccddeeff");
    a.kind        = kind;
    a.previewPath = QStringLiteral("/tsmedia/previews/3f9a1c2e.jpg");
    a.previewSha  = shaOf("preview");
    a.previewSize = 4321;
    if (media && kind != Kind::YouTube) {
        a.mediaPath = kind == Kind::Video ? QStringLiteral("/tsmedia/link_3f9a1c2e.mp4") : kind == Kind::Gif ? QStringLiteral("/tsmedia/link_3f9a1c2e.gif")
                                                                                                           : QStringLiteral("/tsmedia/link_3f9a1c2e.png");
        a.mediaSha  = shaOf("media");
        a.mediaSize = 123456;
    }
    a.width  = 1280;
    a.height = 720;
    if (kind == Kind::Video)
        a.durationMs = 61000;
    if (kind == Kind::YouTube) {
        a.title  = QStringLiteral("A song \u0645\u06cc\u200c\u062e\u0648\u0627\u0647\u0645");
        a.author = QStringLiteral("A channel");
    }
    a.channelId = 7;
    return a;
}

Announcement extended(Kind kind, bool picture, int textScale = 1)
{
    Announcement a;
    a.extended = true;
    a.urlKey   = QStringLiteral("8eb4311921f7b1964785a11a4cc1f418");
    a.kind     = kind;
    if (picture || kind != Kind::Card) {
        a.channelId   = 7;
        a.previewPath = QStringLiteral("/tsmedia/previews/linkpreview_3f9a1c2e.jpg");
        a.previewSha  = shaOf("preview");
        a.previewSize = 4321;
        a.width       = 1200;
        a.height      = 630;
    }
    if (kind != Kind::Card) {
        a.mediaPath = kind == Kind::Gif ? QStringLiteral("/tsmedia/link_3f9a1c2e.gif") : kind == Kind::Image ? QStringLiteral("/tsmedia/link_3f9a1c2e.png")
                                                                                                             : QStringLiteral("/tsmedia/link_3f9a1c2e.mp4");
        a.mediaSha  = shaOf("media");
        a.mediaSize = 654321;
    }
    if (kind == Kind::Video || kind == Kind::Loop)
        a.durationMs = 4200;
    if (kind == Kind::Card) {
        // Cleaned as the wire carries it (a stored LPX is checked against its cleaned text).
        a.card.site        = textclean::cleanToBytes(QStringLiteral("Example News ").repeated(textScale), kCardSiteBytes);
        a.card.author      = textclean::cleanToBytes(QStringLiteral("Someone ").repeated(textScale), kCardAuthorBytes);
        a.card.title       = textclean::cleanToBytes(QStringLiteral("A title of a page ").repeated(4 * textScale), kCardTitleBytes);
        a.card.description = textclean::cleanToBytes(QStringLiteral("A description of the page, long enough. ").repeated(3 * textScale), kCardDescriptionBytes);
        a.card.layout      = picture ? CardLayout::Large : CardLayout::None;
        a.card.hasColor    = true;
        a.card.color       = qRgb(0x2e, 0x7d, 0x32);
        a.card.play        = picture;
        a.durationMs       = picture ? 95000 : 0;
    }
    a.sensitive = kind == Kind::Gif;
    return a;
}

// Every command 2.5 sends about link previews and presence.
QVector<proto::Message> everyCommand()
{
    QVector<proto::Message> out;
    for (const Kind kind : {Kind::YouTube, Kind::Image, Kind::Gif, Kind::Video}) {
        for (const bool media : {false, true}) {
            for (const proto::Scope scope : {proto::Scope::Channel, proto::Scope::Private})
                out << makeMessage(legacy(kind, media), scope, ref());
        }
    }
    out << makeSkip(proto::Scope::Channel, ref(), QStringLiteral("00112233445566778899aabbccddeeff"));
    for (const Kind kind : {Kind::Image, Kind::Gif, Kind::Video, Kind::Loop, Kind::Card}) {
        for (const int cap : {900, 1400}) {
            out << makeLpx(extended(kind, true), proto::Scope::Channel, ref(), cap);
            out << makeLpx(extended(kind, true, 4), proto::Scope::Private, ref(), cap); // shortened to fit
        }
    }
    out << makeLpx(extended(Kind::Card, false), proto::Scope::Channel, ref(), 900); // a text-only card
    const QString k1 = QStringLiteral("00112233445566778899aabbccddeeff"), k2 = QStringLiteral("8eb4311921f7b1964785a11a4cc1f418"),
                  k3 = QStringLiteral("2019fab9d770c5f007bced59a5abfa1e");
    out << makeLpxSkip(proto::Scope::Channel, ref(), {k1});
    out << makeLpxSkip(proto::Scope::Channel, ref(), {k1}, SkipReason::Nothing);
    out << makeLpxSkip(proto::Scope::Private, ref(), {k2}, SkipReason::CrossSite);
    out << makeLpxSkip(proto::Scope::Channel, ref(), {k1, k2, k3}); // the intake notice
    const QStringList caps{QStringLiteral("p"), QStringLiteral("r"), QStringLiteral("l"), QStringLiteral("lx"), QStringLiteral("lw")};
    out << proto::makeHello(QStringLiteral("2.5.0"), caps, true) << proto::makeHello(QStringLiteral("2.5.0"), caps, false)
        << proto::makeHi(QStringLiteral("2.5.0"), caps);
    return out;
}

} // namespace

class TestCompat24 : public QObject
{
    Q_OBJECT

  private slots:
    void everyCommandParses();
    void typeGateDropsLpx();
    void helloKeepsL();
    void lpReadIdentically();
    void lpnoReadIdentically();
    void longestLpxStillParses();
    void storeDowngrade();
    void storeUpgradeKeepsLpTitles();
    void shareLinksStayOut();
};

void TestCompat24::shareLinksStayOut()
{
    // design2 2.10 / 4.3: a Nextcloud share link. 2.4.0's classify gives it no slot (only YouTube and media files), and
    // 2.4.0 drops what a 2.5 sender announces about it: the media (LPX k=i), the share card (LPX k=c) and LPXNO r=n.
    const Target share = classify(QStringLiteral("https://cloud.example.org/s/Ab3dE6gH9jK2mN5"));
    QCOMPARE(share.cls, LinkClass::MediaPage);
    QCOMPARE(share.provider, QStringLiteral("nextcloud"));
    QVERIFY(!share.isLegacy());
    Announcement media = extended(Kind::Image, true);
    media.urlKey       = share.key;
    Announcement card  = extended(Kind::Card, true);
    card.urlKey        = share.key;
    card.card.site.clear(); // no site line: the footer shows the host
    card.card.author.clear();
    card.card.title       = QStringLiteral("report.pdf");
    card.card.description = QString::fromUtf8("PDF document \xC2\xB7 2.1 MB");
    card.card.layout      = CardLayout::Thumb;
    card.card.play        = false;
    card.durationMs       = 0;
    for (const proto::Message& m : {makeLpx(media, proto::Scope::Channel, ref(), 900), makeLpx(card, proto::Scope::Channel, ref(), 900),
                                    makeLpxSkip(proto::Scope::Channel, ref(), {share.key}, SkipReason::Nothing)}) {
        QVERIFY(!m.type.isEmpty());
        proto::ParseResult                  result = proto::ParseResult::Empty;
        const std::optional<proto::Message> parsed = compat24::parse(proto::serialize(m), &result);
        QVERIFY(parsed && result == proto::ParseResult::Ok);
        QVERIFY(!compat24::hubTakes(*parsed));
        QVERIFY(!compat24::readMessage(*parsed));
    }
    // The share card on the wire: t and x, the theme colour, the picture; no site name.
    const proto::Message lpx = makeLpx(card, proto::Scope::Channel, ref(), 900);
    QVERIFY(lpx.has("t") && lpx.has("x") && lpx.has("cl") && lpx.has("p"));
    QVERIFY(!lpx.has("n"));
    const std::optional<Incoming> read = readLpx(*proto::parse(proto::serialize(lpx)));
    QVERIFY(read);
    QCOMPARE(read->announcement.card.title, QStringLiteral("report.pdf"));
    QCOMPARE(read->announcement.card.description, QString::fromUtf8("PDF document \xC2\xB7 2.1 MB"));
    // The share's explicit file link is a 2.4 link both ways: LP, read by 2.4.0 exactly as by 2.5.
    const Target file = classify(QStringLiteral("https://cloud.example.org/s/Ab3dE6gH9jK2mN5/download/a.mp4"));
    QCOMPARE(file.cls, LinkClass::Direct);
    Announcement lp = legacy(Kind::Video, true);
    lp.urlKey       = file.key;
    const QByteArray                        wire = proto::serialize(makeMessage(lp, proto::Scope::Channel, ref()));
    const std::optional<compat24::Incoming> then = compat24::readMessage(*compat24::parse(wire));
    QVERIFY(then);
    QCOMPARE(then->announcement.urlKey, file.key);
}

void TestCompat24::storeUpgradeKeepsLpTitles()
{
    // Review fix: a link-previews.json written by 2.4.0 holds YouTube titles cleaned by 2.4.0's rules, which kept tag
    // characters (the England flag), the Hangul filler and private-use characters. 2.5 cleans them on load (as on
    // receipt) and keeps the entry: the sender's shared preview of that message stays.
    PreviewStore store;
    const qint64 now = 1760000100;
    Announcement a   = legacy(Kind::YouTube, false);
    a.messageKey     = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000);
    a.senderUid      = QStringLiteral("AliceUidBBBBBBBBBBBBBBBBBBB=");
    a.serverUid      = QStringLiteral("server");
    a.title          = QStringLiteral("Match day");
    QCOMPARE(store.add(a, now), PreviewStore::Add::Added);
    QJsonObject root    = QJsonDocument::fromJson(store.toJson(now)).object();
    QJsonArray  entries = root.value(QLatin1String("entries")).toArray();
    QCOMPARE(entries.size(), 1);
    static const uint kFlag[] = {0x1F3F4, 0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067, 0xE007F}; // England: a tag sequence
    const QString     title24 = QStringLiteral("Match day ") + QString::fromUcs4(kFlag, 7) + QChar(0x3164) + QChar(0xE000);
    QJsonObject       entry   = entries.at(0).toObject();
    entry.insert(QLatin1String("t"), title24);
    entries.replace(0, entry);
    root.insert(QLatin1String("entries"), entries);
    const QByteArray json24 = QJsonDocument(root).toJson(QJsonDocument::Compact);
    // 2.4.0 itself wrote and reads such an entry.
    compat24::Announcement read24;
    QVERIFY(compat24::storeEntry(entry, &read24));
    // 2.5 keeps it, its title cleaned by 2.5's rules.
    PreviewStore upgraded;
    QVERIFY(upgraded.fromJson(json24, now));
    QCOMPARE(upgraded.count(), 1);
    const Announcement* kept = upgraded.find(a.messageKey, a.urlKey);
    QVERIFY(kept);
    QCOMPARE(kept->title, textclean::cleanToBytes(title24, kMaxTitleBytes));
    QVERIFY(!kept->title.contains(QChar(0x3164)));
}

void TestCompat24::everyCommandParses()
{
    const QVector<proto::Message> commands = everyCommand();
    QVERIFY(commands.size() > 30);
    for (const proto::Message& m : commands) {
        QVERIFY2(!m.type.isEmpty(), "a sample command couldn't be made");
        const QByteArray wire = proto::serialize(m);
        QVERIFY2(!wire.isEmpty(), m.type.constData());
        QVERIFY(wire.size() <= proto::kMaxRecvBytes);
        proto::ParseResult                  result = proto::ParseResult::Empty;
        const std::optional<proto::Message> parsed = compat24::parse(wire, &result);
        QVERIFY2(parsed && result == proto::ParseResult::Ok, wire.left(80).constData());
        QCOMPARE(parsed->type, m.type);
    }
}

void TestCompat24::typeGateDropsLpx()
{
    for (const proto::Message& m : everyCommand()) {
        const std::optional<proto::Message> parsed = compat24::parse(proto::serialize(m));
        QVERIFY(parsed);
        const bool newType = m.type == "LPX" || m.type == "LPXNO";
        QCOMPARE(compat24::hubTakes(*parsed), m.type == "LP" || m.type == "LPNO");
        if (newType) {
            // Nothing of 2.4.0's would read them either.
            QVERIFY(!compat24::readMessage(*parsed));
            QVERIFY(!compat24::readSkip(*parsed));
            QVERIFY(!compat24::readHello(*parsed));
        }
    }
}

void TestCompat24::helloKeepsL()
{
    const QStringList caps{QStringLiteral("p"), QStringLiteral("r"), QStringLiteral("l"), QStringLiteral("lx"), QStringLiteral("lw")};
    for (const proto::Message& m : {proto::makeHello(QStringLiteral("2.5.0"), caps, true), proto::makeHi(QStringLiteral("2.5.0"), caps)}) {
        const std::optional<proto::Message>   parsed = compat24::parse(proto::serialize(m));
        QVERIFY(parsed);
        const std::optional<compat24::Hello> hello = compat24::readHello(*parsed);
        QVERIFY(hello);
        QCOMPARE(hello->version, QStringLiteral("2.5.0"));
        QVERIFY(hello->hasCap("l"));
        QVERIFY(hello->hasCap("p") && hello->hasCap("r"));
        QCOMPARE(hello->caps.size(), 5); // within 2.4.0's eight
        QCOMPARE(hello->hi, m.type == "HI");
    }
    // Without "make previews": no "l" (2.4.0 then never waits for this sender).
    const std::optional<compat24::Hello> off =
        compat24::readHello(*compat24::parse(proto::serialize(proto::makeHello(QStringLiteral("2.5.0"), {QStringLiteral("p"), QStringLiteral("r")}, true))));
    QVERIFY(off && !off->hasCap("l"));
}

void TestCompat24::lpReadIdentically()
{
    for (const Kind kind : {Kind::YouTube, Kind::Image, Kind::Gif, Kind::Video}) {
        for (const bool media : {false, true}) {
            const proto::Message m = makeMessage(legacy(kind, media), proto::Scope::Channel, ref());
            QVERIFY(!m.type.isEmpty());
            const QByteArray                      wire = proto::serialize(m);
            const std::optional<Incoming>          now  = readMessage(*proto::parse(wire));
            const std::optional<compat24::Incoming> then = compat24::readMessage(*compat24::parse(wire));
            QVERIFY(now && then);
            const Announcement&           a = now->announcement;
            const compat24::Announcement& b = then->announcement;
            QCOMPARE(now->hash, then->hash);
            QCOMPARE(now->ageSec, then->ageSec);
            QCOMPARE(a.urlKey, b.urlKey);
            QCOMPARE(a.kind, b.kind);
            QCOMPARE(a.channelId, b.channelId);
            QCOMPARE(a.previewPath, b.previewPath);
            QCOMPARE(a.previewSha, b.previewSha);
            QCOMPARE(a.previewSize, b.previewSize);
            QCOMPARE(a.mediaPath, b.mediaPath);
            QCOMPARE(a.mediaSha, b.mediaSha);
            QCOMPARE(a.mediaSize, b.mediaSize);
            QCOMPARE(a.width, b.width);
            QCOMPARE(a.height, b.height);
            QCOMPARE(a.durationMs, b.durationMs);
            QVERIFY(!a.extended);
            // The title: 2.4.0 strips the Persian joiner again (harmless); the rest is the same.
            QString title = a.title;
            title.remove(QChar(0x200c));
            QCOMPARE(title, b.title);
            QCOMPARE(a.author, b.author);
        }
    }
}

void TestCompat24::lpnoReadIdentically()
{
    const QByteArray                     wire = proto::serialize(makeSkip(proto::Scope::Private, ref(), QStringLiteral("00112233445566778899aabbccddeeff")));
    const std::optional<Skip>             now  = readSkip(*proto::parse(wire));
    const std::optional<compat24::Skip>  then = compat24::readSkip(*compat24::parse(wire));
    QVERIFY(now && then);
    QCOMPARE(now->urlKey, then->urlKey);
    QCOMPARE(now->hash, then->hash);
    QCOMPARE(now->ageSec, then->ageSec);
    QCOMPARE(now->scope, then->scope);
}

void TestCompat24::longestLpxStillParses()
{
    // The longest an LPX can be (1400 bytes once live check L1 passes): 2.4.0's parser takes 2048.
    Announcement a        = extended(Kind::Card, true, 8);
    a.previewPath         = QStringLiteral("/") + QStringLiteral("a").repeated(150) + QStringLiteral("/linkpreview_3f9a1c2e.jpg");
    const proto::Message m = makeLpx(a, proto::Scope::Channel, ref(), proto::kMaxLpxSendBytes);
    QVERIFY(!m.type.isEmpty());
    const QByteArray wire = proto::serialize(m);
    QVERIFY(wire.size() > 1200 && wire.size() <= proto::kMaxLpxSendBytes);
    QVERIFY(compat24::parse(wire));
    QVERIFY(!compat24::hubTakes(*compat24::parse(wire)));
}

void TestCompat24::storeDowngrade()
{
    // A 2.5 link-previews.json with LP and LPX entries, read by 2.4.0: the LPX entries are skipped one by one.
    PreviewStore store;
    const qint64 now = 1760000100;
    int          added = 0;
    for (const Kind kind : {Kind::Image, Kind::Card, Kind::Loop}) {
        Announcement a = kind == Kind::Image ? legacy(Kind::Image, true) : extended(kind, true);
        a.messageKey   = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000 + added);
        a.senderUid    = QStringLiteral("AliceUidBBBBBBBBBBBBBBBBBBB=");
        a.serverUid    = QStringLiteral("server");
        QCOMPARE(store.add(a, now), PreviewStore::Add::Added);
        ++added;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(store.toJson(now));
    QVERIFY(doc.isObject());
    const QJsonArray entries = doc.object().value(QLatin1String("entries")).toArray();
    QCOMPARE(entries.size(), 3);
    int kept = 0;
    for (const QJsonValue& v : entries) {
        compat24::Announcement a;
        const bool             read = compat24::storeEntry(v.toObject(), &a);
        const QString          k    = v.toObject().value(QLatin1String("k")).toString();
        QCOMPARE(read, k.size() == 1);
        kept += read ? 1 : 0;
    }
    QCOMPARE(kept, 1);
    // And 2.5 reads all three back.
    PreviewStore again;
    QVERIFY(again.fromJson(store.toJson(now), now));
    QCOMPARE(again.count(), 3);
}

TSMEDIA_REGISTER_TEST(TestCompat24)
#include "tst_compat24.moc"
