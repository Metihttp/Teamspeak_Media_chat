// 2.4 link previews, the chat side (chat-ui): which links of a TeamSpeak-like chat get a preview (tests/tschat.h,
// made-up people and addresses only), the slots under them (cards and a link's media) given back exactly, also
// through the chat layout, text reaction rows and the next line TeamSpeak appends; the message's reaction key never
// changes with a slot; the cards' geometry and hit tests; what a slot shows (linkstate::decide); the made-up links
// Core gets (a sender's files on the TeamSpeak server, a website's file here) and Core never fetching a website's
// file from a TeamSpeak server; the two settings. Nothing here touches the network.

#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QtTest>

#include <algorithm>
#include <memory>

#include "core.h"
#include "fakets3.h"
#include "layoutart.h"
#include "layoutbubbles.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "linkpreview/autoload.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkdoc.h"
#include "linkpreview/linkstate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/lpmessage.h"
#include "linkpreview/previewstore.h"
#include "linkpreview/senderpipeline.h"
#include "privacysection.h"
#include "serversettings.h"
#include "settings.h"
#include "testmain.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "ts3api.h"
#include "tschat.h"
#include "uiutil.h"

namespace {

using layoutart::Mode;
using linkpreview::Kind;

const QString kAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");
const QString kMe    = QStringLiteral("q0Xn6Mine000000000000000000=");

QFont chatFont()
{
    QFont f(QStringLiteral("Segoe UI"));
    f.setPointSizeF(9.0);
    return f;
}

void setup(QTextDocument& doc, int width = 520)
{
    doc.setDefaultFont(chatFont());
    doc.setTextWidth(width);
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
}

layoutdoc::Env envFor(Mode mode, bool bubbles, bool ownRight = false, int width = 512)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false, bubbles && mode == Mode::Cozy);
    env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
    env.dpr          = 1.0;
    env.contentWidth = width;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("l");
    env.ownNick      = QStringLiteral("Sam");
    env.ownUid       = kMe;
    env.ownRight     = ownRight && bubbles && mode == Mode::Cozy;
    return env;
}

// ---- TeamSpeak's document, exactly (as tst_chatdoc23 compares it) ----------------------------------------

struct Snapshot {
    QString              text;
    QVector<QVariantMap> chars;
    QVector<QVariantMap> blocks;
    QVector<bool>        visible;
};

QVariantMap propsOf(const QTextFormat& f, QTextDocument* doc = nullptr)
{
    QVariantMap               out;
    const QMap<int, QVariant> props = f.properties();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (it.key() == QTextFormat::ObjectIndex && doc) {
            QTextObject* o = doc->object(it.value().toInt());
            out.insert(QStringLiteral("object"), o ? QVariant(propsOf(o->format())) : QVariant(QStringLiteral("none")));
            continue;
        }
        out.insert(QString::number(it.key()), it.value());
    }
    return out;
}

Snapshot snapshot(QTextDocument& doc)
{
    Snapshot s;
    s.text = doc.toPlainText();
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        s.blocks.append(propsOf(b.blockFormat(), &doc));
        s.visible.append(b.isVisible());
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            for (int i = 0; i < f.length(); ++i)
                s.chars.append(propsOf(f.charFormat(), &doc));
        }
        s.chars.append(propsOf(b.charFormat(), &doc));
    }
    return s;
}

QString compare(const Snapshot& a, const Snapshot& b)
{
    if (a.text != b.text)
        return QStringLiteral("text differs:\n%1\n--- vs ---\n%2").arg(a.text, b.text);
    if (a.blocks.size() != b.blocks.size())
        return QStringLiteral("block count %1 vs %2").arg(a.blocks.size()).arg(b.blocks.size());
    for (int i = 0; i < a.blocks.size(); ++i) {
        if (a.blocks.at(i) != b.blocks.at(i))
            return QStringLiteral("block %1 format differs").arg(i);
        if (a.visible.at(i) != b.visible.at(i))
            return QStringLiteral("block %1 visibility differs").arg(i);
    }
    if (a.chars.size() != b.chars.size())
        return QStringLiteral("char count %1 vs %2").arg(a.chars.size()).arg(b.chars.size());
    for (int i = 0; i < a.chars.size(); ++i) {
        if (a.chars.at(i) == b.chars.at(i))
            continue;
        QStringList keys = a.chars.at(i).keys() + b.chars.at(i).keys();
        keys.removeDuplicates();
        QStringList diff;
        for (const QString& k : keys) {
            if (a.chars.at(i).value(k) != b.chars.at(i).value(k))
                diff << k;
        }
        return QStringLiteral("char %1 format differs in %2").arg(i).arg(diff.join(QLatin1Char(',')));
    }
    return {};
}

bool hasSlotFormat(QTextDocument& doc)
{
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (linkdoc::carriesSlot(b.charFormat()))
            return true;
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            if (linkdoc::carriesSlot(it.fragment().charFormat()))
                return true;
        }
    }
    return false;
}

// ---- the chat ------------------------------------------------------------------------------------------------

const QString kYouTube = QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42");
const QString kShort   = QStringLiteral("https://youtu.be/aBcDeFgHiJk");
const QString kPng     = QStringLiteral("https://cdn.example.org/pics/lake.png?w=800");
const QString kGif     = QStringLiteral("https://media.example.net/cat.gif");
const QString kMp4     = QStringLiteral("https://files.example.com/clip.mp4");

QString link(const tschat::Skin& s, const QString& url, const QString& label = QString())
{
    return tschat::linkHtml(s, url, label);
}

// Messages with web links of every kind, links that never get a preview, and lines that aren't messages.
void fillLinks(QTextDocument& doc)
{
    setup(doc);
    const tschat::Skin s   = tschat::darkSkin();
    const auto         msg = [&](const char* time, int clid, const QString& uid, const QString& nick, const QString& html, bool out = false) {
        tschat::append(&doc, tschat::messageHtml(s, QString::fromLatin1(time), clid, uid, nick, html, out));
    };
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("20:00:01"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"))));
    msg("20:01:00", 17, kAlice, QStringLiteral("Alice"), QStringLiteral("watch this ") + link(s, kYouTube));                        // 1: YouTube at the end
    msg("20:01:30", 18, kBob, QStringLiteral("Bob"), link(s, kPng) + QStringLiteral(" nice lake"));                                 // 2: text after the link
    msg("20:02:00", 3, kMe, QStringLiteral("Sam"), link(s, kGif), true);                                                             // 3: own
    msg("20:02:30", 18, kBob, QStringLiteral("Bob"), QStringLiteral("http only: ") + link(s, QStringLiteral("http://example.com/a.png"))); // 4: none
    msg("20:03:00", 18, kBob, QStringLiteral("Bob"), QStringLiteral("a page ") + link(s, QStringLiteral("https://example.com/article"))); // 5: none
    msg("20:03:30", 17, kAlice, QStringLiteral("Alice"),
        link(s, kMp4) + QStringLiteral(" and ") + link(s, kShort) + QStringLiteral(" and again ") + link(s, kMp4) + QStringLiteral(" ") + link(s, kGif)
            + QStringLiteral(" ") + link(s, kPng)); // 6: three at most, one per address
    // 7: a TS Media file link, its note and the plugin's own link after it
    msg("20:04:00", 18, kBob, QStringLiteral("Bob"),
        link(s, QStringLiteral("ts3file://127.0.0.1?port=9987&serverUID=abc&channel=5&path=%2Ftsmedia&filename=x.png&size=10&tsm=2"), QStringLiteral("x.png"))
            + QStringLiteral(" — ") + link(s, QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat/raw/main/shot.png"), QStringLiteral("TS Media chat"))
            + QStringLiteral(" plugin required"));
    tschat::append(&doc, tschat::printLinkHtml(s, QStringLiteral("Someone has a newer version."), QStringLiteral("https://example.com/new.png"), QStringLiteral("Check"))); // 8: own line
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("20:05:00"), QStringLiteral("Welcome ") + link(s, QStringLiteral("https://example.com/banner.jpg")))); // 9: system
    msg("20:06:00", 17, kAlice, QStringLiteral("Alice"), link(s, QStringLiteral("https://cdn.example.org/shot.webp"), QStringLiteral("my screenshot"))); // 10: [URL] label
}

QStringList describe(const QVector<linkdoc::Link>& links)
{
    QStringList out;
    for (const linkdoc::Link& l : links)
        out << QStringLiteral("%1:%2:%3").arg(l.block).arg(static_cast<int>(l.target.kind)).arg(l.target.host);
    return out;
}

// Slots as ChatLinkPreviews puts them: a card under every link (media for the picture at block 2).
int insertAll(QTextDocument& doc, bool mediaForPictures = false)
{
    int n = 0;
    // Back to front: earlier positions stay valid.
    QVector<linkdoc::Link> links = linkdoc::findLinks(&doc);
    for (int i = links.size() - 1; i >= 0; --i) {
        const linkdoc::Link& l     = links.at(i);
        const bool           media = mediaForPictures && l.target.kind == Kind::Image;
        const QString        name  = media ? linkdoc::mediaPrefix() + QStringLiteral("k%1").arg(i) : linkdoc::cardPrefix() + QStringLiteral("t%1").arg(i);
        n += linkdoc::insertSlot(&doc, l, name, media ? QSizeF(260, 188) : QSizeF(340, 64)) ? 1 : 0;
    }
    return n;
}

QMap<int, QString> keysOf(QTextDocument& doc)
{
    QMap<int, QString> keys;
    const QDate        today = QDate(2026, 10, 10);
    const QVector<QDate> dates = layoutdoc::datesOf(&doc, today);
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const textreactdoc::Identity id = textreactdoc::identify(b);
        if (!id.eligible())
            continue;
        keys.insert(b.blockNumber(), textreactdoc::localKeyOf(id, textreactdoc::dayOf(dates.value(b.blockNumber()), id.seconds, today, 23 * 3600), 0));
    }
    return keys;
}

void layoutNewLine(QTextDocument& doc, const layoutdoc::Env& env, int n)
{
    QTextBlock b = doc.findBlockByNumber(n);
    if (layoutdoc::hasOurs(b) || b.blockFormat().hasProperty(layoutformat::kBlockTag))
        layoutdoc::sanitize(&doc, n);
    b                        = doc.findBlockByNumber(n);
    const layoutdoc::Row row = layoutdoc::read(b);
    if (row.type == layoutdoc::RowType::Unknown || row.styled)
        return;
    const QVector<QDate> dates = layoutdoc::datesOf(&doc, env.today);
    layoutdoc::apply(&doc, n, layoutdoc::plan(env, b, row, layoutdoc::previousOf(b), dates.value(n), 0, 900 + n), env);
}

PreviewStyle styleFor(bool dark, int maxWidth = 400, bool bubble = false)
{
    PreviewStyle s;
    s.dark     = dark;
    s.font     = chatFont();
    s.dpr      = 1.0;
    s.maxWidth = maxWidth;
    if (bubble)
        s.bubbles = {dark ? QColor(0x38, 0x3a, 0x40) : QColor(0xee, 0xf0, 0xf2)};
    return s;
}

QString sha43(char fill)
{
    return QString::fromLatin1(QByteArray(32, fill).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace

class TestLinkUi : public QObject
{
    Q_OBJECT

  private slots:
    // ---- which links -------------------------------------------------------------------------------------
    void classifyAsTheChatUsesIt()
    {
        // net-core's classify() (tst_linkpreview_net has its whole table): what the chat relies on.
        QCOMPARE(linkpreview::classify(kYouTube).kind, Kind::YouTube);
        QCOMPARE(linkpreview::classify(kYouTube).videoId, QStringLiteral("dQw4w9WgXcQ"));
        QCOMPARE(linkpreview::classify(kShort).kind, Kind::YouTube);
        QCOMPARE(linkpreview::classify(QStringLiteral("https://m.youtube.com/shorts/aBcDeFgHiJk")).kind, Kind::YouTube);
        QCOMPARE(linkpreview::classify(kShort).url, QUrl(QStringLiteral("https://www.youtube.com/watch?v=aBcDeFgHiJk")));
        QCOMPARE(linkpreview::classify(kPng).kind, Kind::Image);
        QCOMPARE(linkpreview::classify(kGif).kind, Kind::Gif);
        QCOMPARE(linkpreview::classify(kMp4).kind, Kind::Video);
        // http: YouTube only (TeamSpeak links "www.youtube.com/..." typed without a scheme as http://); its card
        // opens and fetches the canonical https address.
        const linkpreview::Target plain = linkpreview::classify(QStringLiteral("http://www.youtube.com/watch?v=aBcDeFgHiJk"));
        QCOMPARE(plain.kind, Kind::YouTube);
        QCOMPARE(plain.url.scheme(), QStringLiteral("https"));
        for (const char* no : {"http://example.com/a.png", "https://example.com/article", "https://user@example.com/a.png", "https://example.com:8443/a.png",
                               "https://www.youtube.com/watch?v=short", "https://youtube.com.evil.example/watch?v=dQw4w9WgXcQ", "https://127.0.0.1/a.png",
                               "ts3file://127.0.0.1?filename=a.png", "https://www.youtube.com/channel/abc", "https://cdn.example/a.png"})
            QVERIFY2(linkpreview::classify(QString::fromLatin1(no)).kind == Kind::None, no);
        // The key: stable and the same for the same video however it is written (http or https).
        QCOMPARE(linkpreview::classify(kShort).key, linkpreview::classify(QStringLiteral("https://www.youtube.com/watch?v=aBcDeFgHiJk")).key);
        QCOMPARE(plain.key, linkpreview::classify(kShort).key);
        QVERIFY(linkpreview::isKey(linkpreview::classify(kPng).key));
    }

    void httpYouTubeInTheChat()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:01:00"), 17, kAlice, QStringLiteral("Alice"),
                                                 QStringLiteral("typed ") + link(s, QStringLiteral("http://www.youtube.com/watch?v=aBcDeFgHiJk"), QStringLiteral("www.youtube.com/watch?v=aBcDeFgHiJk"))));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:01:30"), 17, kAlice, QStringLiteral("Alice"),
                                                 QStringLiteral("plain picture ") + link(s, QStringLiteral("http://cdn.example.org/a.png"))));
        const QVector<linkdoc::Link> links = linkdoc::findLinks(&doc);
        QCOMPARE(links.size(), 1);
        QCOMPARE(links.first().block, 0);
        QCOMPARE(links.first().target.kind, Kind::YouTube);
        QCOMPARE(links.first().target.key, linkpreview::classify(kShort).key);
    }

    void linksInTeamSpeakChat()
    {
        QTextDocument doc;
        fillLinks(doc);
        const QVector<linkdoc::Link> links = linkdoc::findLinks(&doc);
        // 1 YouTube, 2 picture, 3 own GIF, 6: video, YouTube, GIF (the repeated video once, the fourth address left
        // out), 10 the labelled picture. Never: http, a page, after a TS Media file link, TS Media's own line, a
        // system row.
        // The host shown is the link's own (net-core's Target::host).
        QCOMPARE(describe(links), QStringList({QStringLiteral("1:1:www.youtube.com"), QStringLiteral("2:2:cdn.example.org"), QStringLiteral("3:3:media.example.net"),
                                               QStringLiteral("6:4:files.example.com"), QStringLiteral("6:1:youtu.be"), QStringLiteral("6:3:media.example.net"),
                                               QStringLiteral("10:2:cdn.example.org")}));
        QVERIFY(links.at(2).outgoing);
        QVERIFY(!links.at(0).outgoing);
        // The anchor is exactly the link's text.
        for (const linkdoc::Link& l : links) {
            QTextCursor c(&doc);
            c.setPosition(l.start);
            c.setPosition(l.end, QTextCursor::KeepAnchor);
            QVERIFY(c.charFormat().isAnchor());
            QCOMPARE(c.charFormat().anchorHref().replace(QStringLiteral("&amp;"), QStringLiteral("&")), l.href);
        }
        // The plugin's own addresses are passed in and never previewed (block 6 had left the picture out already).
        const QVector<linkdoc::Link> without = linkdoc::findLinks(&doc, {kPng});
        QCOMPARE(without.size(), links.size() - 1);
        QCOMPARE(without.at(1).block, 3);
    }

    void cardsAndKeys()
    {
        QTextDocument doc;
        fillLinks(doc);
        const QMap<int, QString> keys = keysOf(doc);
        QVERIFY(keys.contains(1) && keys.contains(2) && keys.contains(6));
        QCOMPARE(insertAll(doc, true), 7);
        // The message keys and the "text message" judgement don't change with a slot (card or media).
        QCOMPARE(keysOf(doc), keys);
        // Slots are found again by property, each right after its link; nothing is found twice.
        const QVector<linkdoc::Link> links = linkdoc::findLinks(&doc);
        QCOMPARE(links.size(), 7);
        for (const linkdoc::Link& l : links) {
            const linkdoc::Slot s = linkdoc::slotAfter(doc.findBlockByNumber(l.block), l);
            QVERIFY2(s.exists(), qPrintable(l.href));
            QCOMPARE(s.urlKey, l.target.key);
            QCOMPARE(s.href, l.href);
            QVERIFY(!linkdoc::insertSlot(&doc, l, QStringLiteral("tsmedia-link:again"), QSizeF(10, 10)));
        }
        QCOMPARE(linkdoc::allSlots(&doc).size(), 7);
        // Text after the link starts on a line of its own (our line break after the picture), and the block's
        // text otherwise is TeamSpeak's.
        const QTextBlock two = doc.findBlockByNumber(2);
        QVERIFY(two.text().contains(QString(QChar::ObjectReplacementCharacter) + QChar(QChar::LineSeparator) + QStringLiteral(" nice lake")));
        QCOMPARE(linkdoc::slotsIn(two).first().after, linkdoc::slotsIn(two).first().object + 1);
        // A card becomes the media and back; the picture's size follows.
        const linkdoc::Slot first = linkdoc::allSlots(&doc).first();
        QVERIFY(linkdoc::replaceObject(&doc, first.object, linkdoc::mediaPrefix() + QStringLiteral("abc"), QSizeF(300, 200)));
        QCOMPARE(linkdoc::allSlots(&doc).first().name, linkdoc::mediaPrefix() + QStringLiteral("abc"));
        QVERIFY(!linkdoc::allSlots(&doc).first().isCard());
        QVERIFY(linkdoc::resizeObject(&doc, first.object, QSizeF(320, 210)));
        QCOMPARE(linkdoc::allSlots(&doc).first().size, QSizeF(320, 210));
        QCOMPARE(keysOf(doc), keys);
    }

    // The plugin going, the setting going off: TeamSpeak's document back exactly. Also through the chat layout
    // (Cozy with bubbles and your messages on the right, Compact, Classic), text reaction rows after the slots and
    // the next lines TeamSpeak appends (they copy our picture's format).
    void restoreIsExact_data()
    {
        QTest::addColumn<int>("mode"); // 0 classic, 1 cozy bubbles own right, 2 compact
        QTest::addColumn<bool>("rows");
        QTest::addColumn<bool>("slotsFirst"); // our slots go before the layout gives its blocks back
        for (int mode = 0; mode < 3; ++mode) {
            for (const bool rows : {false, true}) {
                for (const bool first : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("mode %1 rows %2 slots first %3").arg(mode).arg(rows).arg(first))) << mode << rows << first;
            }
        }
    }
    void restoreIsExact()
    {
        QFETCH(int, mode);
        QFETCH(bool, rows);
        QFETCH(bool, slotsFirst);
        QTextDocument plain, doc;
        fillLinks(plain);
        fillLinks(doc);
        const Mode           m   = mode == 0 ? Mode::Classic : mode == 1 ? Mode::Cozy : Mode::Compact;
        const layoutdoc::Env env = envFor(m == Mode::Classic ? Mode::Cozy : m, mode == 1, mode == 1);
        const QMap<int, QString> keys = keysOf(doc);
        if (m != Mode::Classic)
            layoutdoc::styleAll(&doc, env);
        QCOMPARE(insertAll(doc, true), 7);
        if (rows) {
            for (auto it = keys.constBegin(); it != keys.constEnd(); ++it)
                textreactdoc::insertRow(&doc, it.key(), textreactdoc::objectPrefix() + QStringLiteral("r%1").arg(it.key()), it.value(), QSizeF(96, 26));
        }
        if (m != Mode::Classic) {
            QSet<int> own;
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                if (layoutformat::isOwnRight(b))
                    own.insert(b.blockNumber());
            }
            layoutdoc::settle(&doc, {}, env, own);
        }
        QCOMPARE(keysOf(doc), keys);
        // TeamSpeak's next lines: after a slot at a message's end (a picture's format is copied), typed and as HTML.
        const tschat::Skin s = tschat::darkSkin();
        for (QTextDocument* d : {&plain, &doc}) {
            tschat::append(d, tschat::messageHtml(s, QStringLiteral("20:07:00"), 17, kAlice, QStringLiteral("Alice"), link(s, kGif)));
            QTextCursor c(d);
            c.movePosition(QTextCursor::End);
            c.insertBlock();
            c.insertText(QStringLiteral("typed after"));
        }
        if (m != Mode::Classic)
            layoutNewLine(doc, env, doc.blockCount() - 2);
        // The new message gets its slot too; the typed line is no message.
        QCOMPARE(linkdoc::findLinks(&doc).size(), 8);
        const linkdoc::Link last = linkdoc::findLinks(&doc).last();
        QVERIFY(linkdoc::insertSlot(&doc, last, QStringLiteral("tsmedia-link:last"), QSizeF(340, 64)));
        QVERIFY(linkdoc::slotsIn(doc.lastBlock()).isEmpty());
        // A layout switch with the slots in place (what ChatLayout does: everything back, then the new look).
        if (m != Mode::Classic) {
            layoutdoc::restoreAll(&doc);
            layoutdoc::styleAll(&doc, envFor(m == Mode::Cozy ? Mode::Compact : Mode::Cozy, false));
            QCOMPARE(linkdoc::allSlots(&doc).size(), 8);
            layoutdoc::restoreAll(&doc);
            layoutdoc::styleAll(&doc, env);
        }
        // The plugin goes (ChatIntegration's order: rows, the layout, then the slots; or the setting goes off first).
        if (rows)
            textreactdoc::removeAll(&doc);
        if (slotsFirst) {
            QCOMPARE(linkdoc::removeAll(&doc), 8);
            if (m != Mode::Classic)
                layoutdoc::restoreAll(&doc);
        } else {
            if (m != Mode::Classic)
                layoutdoc::restoreAll(&doc);
            QCOMPARE(linkdoc::removeAll(&doc), 8);
        }
        QVERIFY(!hasSlotFormat(doc));
        const QString diff = compare(snapshot(plain), snapshot(doc));
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
    }

    // A slot in your message on the right stays inside its bubble (a card, a link's media), like a preview.
    void slotsInBubbles()
    {
        QTextDocument doc;
        fillLinks(doc);
        const layoutdoc::Env env = envFor(Mode::Cozy, true, true);
        layoutdoc::styleAll(&doc, env);
        QCOMPARE(insertAll(doc), 7);
        QSet<int> own;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (layoutformat::isOwnRight(b))
                own.insert(b.blockNumber());
        }
        QVERIFY(own.contains(3));
        layoutdoc::settle(&doc, {}, env, own);
        doc.documentLayout()->documentSize();
        for (const int n : {1, 2, 3, 6}) {
            const QTextBlock          b    = doc.findBlockByNumber(n);
            const layoutbubbles::Item item = layoutbubbles::itemOf(b, env);
            QCOMPARE(item.kind, layoutbubbles::Kind::Bubble);
            QCOMPARE(item.own, n == 3);
            for (const linkdoc::Slot& s : linkdoc::slotsIn(b)) {
                const QTextLine line = b.layout()->lineForTextPosition(s.object - b.position());
                const QRectF    br   = doc.documentLayout()->blockBoundingRect(b);
                const QRectF    pic(br.left() + line.cursorToX(s.object - b.position()), br.top() + line.y(), s.size.width(), line.height());
                QVERIFY2(item.rect.adjusted(-1, -1, 1, 1).contains(pic), qPrintable(QStringLiteral("block %1").arg(n)));
            }
        }
    }

    // ---- the cards ---------------------------------------------------------------------------------------------
    void cardGeometryAndHits()
    {
        using linkcard::Look;
        using linkcard::Zone;
        for (const bool dark : {false, true}) {
            for (const bool bubble : {false, true}) {
                const PreviewStyle style = styleFor(dark, 400, bubble);
                for (const Look look : {Look::Click, Look::Loading, Look::Error, Look::TooLarge, Look::YouTube, Look::Poster, Look::Hidden}) {
                    linkcard::Card card;
                    card.look = look;
                    card.kind = look == Look::YouTube ? Kind::YouTube : look == Look::Poster ? Kind::Video : Kind::Image;
                    card.host = QStringLiteral("cdn.example.org");
                    card.title  = QStringLiteral("A rather long title that needs two lines in the card, maybe even more than that, to be cut");
                    card.author = QStringLiteral("Some Channel");
                    const QString tag = QStringLiteral("look %1 dark %2 bubble %3").arg(static_cast<int>(look)).arg(dark).arg(bubble);
                    // The size never depends on the pointer, the progress or the thumbnail (no jumps; the title comes
                    // with the card).
                    const linkcard::Layout l = linkcard::layout(card, style);
                    QVERIFY2(!l.size.isEmpty() && l.size.width() <= 400, qPrintable(tag));
                    linkcard::Card loaded = card;
                    loaded.progress       = 0.5;
                    loaded.thumbnail      = QImage(64, 36, QImage::Format_RGB32);
                    loaded.thumbnailLoading = true;
                    QCOMPARE(linkcard::layout(loaded, style).size, l.size);
                    linkcard::Pointer over;
                    over.over  = true;
                    over.hover = Zone::Hide;
                    const QImage a = linkcard::render(card, style, linkcard::Pointer());
                    const QImage b = linkcard::render(loaded, style, over);
                    QCOMPARE(a.size(), b.size());
                    QCOMPARE(a.size(), l.size);
                    // Hits: outside nothing; the hidden pill only shows; the hide button only while over.
                    QCOMPARE(linkcard::zoneAt(card, l, QPointF(-1, -1), true), Zone::None);
                    QCOMPARE(linkcard::zoneAt(card, l, QPointF(l.size.width() + 2, 5), true), Zone::None);
                    if (look == Look::Hidden) {
                        QCOMPARE(linkcard::zoneAt(card, l, QPointF(l.size.width() / 2.0, l.size.height() / 2.0), true), Zone::Show);
                        continue;
                    }
                    QVERIFY2(l.size.width() - l.hide.right() >= -0.01 && l.hide.width() >= 16, qPrintable(tag));
                    QCOMPARE(linkcard::zoneAt(card, l, l.hide.center(), true), Zone::Hide);
                    QVERIFY(linkcard::zoneAt(card, l, l.hide.center(), false) != Zone::Hide);
                    const QPointF body = look == Look::YouTube || look == Look::Poster ? l.thumb.center() : QPointF(l.text.left() + 4, l.size.height() / 2.0);
                    const Zone    want = look == Look::Click ? Zone::Load : look == Look::Error ? Zone::Retry : look == Look::Loading ? Zone::Body : Zone::Open;
                    QCOMPARE(linkcard::zoneAt(card, l, body, true), want);
                    QCOMPARE(linkcard::isActionable(card, want), look != Look::Loading);
                    if (!l.action.isEmpty())
                        QCOMPARE(linkcard::zoneAt(card, l, l.action.center(), true), linkcard::primaryZone(card));
                }
            }
        }
        // Narrow chats: no card is ever wider than the chat's column (ChatLinkPreviews gives at least 160, like the file
        // card; a 300 px Cozy chat with bubbles gives about 200, where a 240 px YouTube card stuck out of its bubble);
        // a rich card never above 400, a strip never above 340.
        linkcard::Card yt;
        yt.look = linkcard::Look::YouTube;
        QCOMPARE(linkcard::layout(yt, styleFor(true, 120)).size.width(), 160);
        QCOMPARE(linkcard::layout(yt, styleFor(true, 900)).size.width(), 400);
        linkcard::Card strip;
        QCOMPARE(linkcard::layout(strip, styleFor(true, 120)).size.width(), 160);
        QCOMPARE(linkcard::layout(strip, styleFor(true, 900)).size.width(), 340);
        for (const int maxWidth : {160, 180, 200, 214, 240, 280, 340, 400}) {
            for (const bool bubble : {false, true}) {
                for (const linkcard::Look look : {linkcard::Look::Click, linkcard::Look::Loading, linkcard::Look::Error, linkcard::Look::TooLarge,
                                                  linkcard::Look::YouTube, linkcard::Look::Poster, linkcard::Look::Hidden}) {
                    linkcard::Card c;
                    c.look = look;
                    c.kind = look == linkcard::Look::YouTube ? Kind::YouTube : Kind::Video;
                    c.host = QStringLiteral("files.example.com");
                    c.size = 87LL * 1024 * 1024;
                    const PreviewStyle     st = styleFor(false, maxWidth, bubble);
                    const linkcard::Layout cl = linkcard::layout(c, st);
                    const QString          tag = QStringLiteral("look %1 width %2 bubble %3").arg(static_cast<int>(look)).arg(maxWidth).arg(bubble);
                    QVERIFY2(cl.size.width() <= maxWidth, qPrintable(tag));
                    // Its parts stay inside it, and the site line keeps some room next to the action.
                    for (const QRectF& part : {cl.icon, cl.text, cl.action, cl.hide, cl.thumb, cl.title, cl.footer})
                        QVERIFY2(part.isEmpty() || (part.left() >= -0.01 && part.right() <= cl.size.width() + 0.01), qPrintable(tag));
                    if (look == linkcard::Look::Poster)
                        QVERIFY2(cl.action.left() - cl.footer.left() >= 60, qPrintable(tag));
                }
            }
        }
        // 16:9 picture.
        const linkcard::Layout l = linkcard::layout(yt, styleFor(false, 400));
        QVERIFY(qAbs(l.thumb.width() * 9.0 / 16.0 - l.thumb.height()) <= 1.0);
        // A short title takes one line (no empty line above the picture, as on Discord), a long one two at most.
        yt.title = QStringLiteral("Night drive");
        const linkcard::Layout one = linkcard::layout(yt, styleFor(false, 400));
        yt.title = QStringLiteral("Lakeside timelapse: a whole day in four minutes, from sunrise to the stars, filmed from one spot");
        const linkcard::Layout two = linkcard::layout(yt, styleFor(false, 400));
        yt.title += QStringLiteral(" over the lake while the seasons change and the birds come and go and come back again");
        const linkcard::Layout three = linkcard::layout(yt, styleFor(false, 400));
        QVERIFY(one.title.height() > 0);
        QCOMPARE(two.title.height(), 2 * one.title.height());
        QCOMPARE(three.title.height(), two.title.height());
        QVERIFY(one.size.height() < two.size.height());
        QCOMPARE(one.thumb.top() - one.title.bottom(), two.thumb.top() - two.title.bottom());
    }

    void cardTexts()
    {
        linkcard::Card c;
        c.host = QStringLiteral("cdn.example.org");
        c.kind = Kind::Gif;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("GIF"));
        QVERIFY(linkcard::toolTip(c, linkcard::Zone::Load).contains(QStringLiteral("cdn.example.org")));
        QVERIFY(linkcard::toolTip(c, linkcard::Zone::Load).contains(QStringLiteral("IP address")));
        c.look     = linkcard::Look::Loading;
        c.progress = 0.454;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Loading preview… 45%"));
        c.look = linkcard::Look::TooLarge;
        c.size = 62LL * 1024 * 1024;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Too large to preview here · 62.0 MB"));
        // Titles and channel names from a website: no controls, no bidi tricks, bounded, never half a surrogate.
        const QString hostile = QStringLiteral("  Rick") + QChar(0x202e) + QChar(0x200f) + QStringLiteral("  Astley") + QChar(QChar::LineFeed) + QChar(QChar::Tabulation)
                                + QStringLiteral("- Never") + QChar(7) + QStringLiteral(" ");
        QCOMPARE(linkcard::cleanText(hostile), QStringLiteral("Rick Astley - Never"));
        QCOMPARE(linkcard::cleanText(QStringLiteral("abc\U0001F600def"), 4), QStringLiteral("abc"));
        QCOMPARE(linkcard::cleanText(QString(500, QLatin1Char('x'))).size(), 200);
        for (const PreviewColorPair& pair : linkcard::colorPairs(false) + linkcard::colorPairs(true))
            QVERIFY2(ui::contrastRatio(pair.foreground, pair.background) >= pair.minimum, qPrintable(pair.name));
    }

    // 2.4 auto-load: the waiting strips and the click card with a size say what they are, and every strip keeps one
    // size whatever it says (nothing jumps when a wait ends).
    void cardTextsForWaits()
    {
        using linkcard::Look;
        using linkcard::Waiting;
        using linkcard::Zone;
        linkcard::Card c;
        c.host = QStringLiteral("cdn.example.org");
        c.kind = Kind::Video;
        c.look = Look::Loading;
        c.waiting = Waiting::Own;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Making the preview…"));
        QVERIFY(linkcard::toolTip(c, Zone::Body).contains(QStringLiteral("Your TS Media")));
        c.progress = 0.4; // a wait never shows a percentage
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Making the preview…"));
        c.waiting = Waiting::Sender;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Waiting for the sender's preview…"));
        QVERIFY(linkcard::toolTip(c, Zone::Body).contains(QStringLiteral("doesn't contact cdn.example.org")));
        QVERIFY(!linkcard::toolTip(c, Zone::Body).contains(QStringLiteral("IP address")));
        // Still asking whether the sender makes one: no claim that they do (their PC may not), and honest that the
        // site may be contacted if not.
        c.waiting = Waiting::Checking;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Checking for the sender's preview…"));
        QVERIFY(!linkcard::toolTip(c, Zone::Body).contains(QStringLiteral("is sharing")));
        QVERIFY(!linkcard::toolTip(c, Zone::Body).contains(QStringLiteral("doesn't contact")));
        QVERIFY(linkcard::toolTip(c, Zone::Body).contains(QStringLiteral("cdn.example.org")));
        QVERIFY(linkcard::toolTip(c, Zone::Body).contains(QStringLiteral("IP address")));
        c.waiting  = Waiting::None;
        c.progress = -1;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Loading preview…"));
        // YouTube: the tool tip names YouTube, never the link's own host.
        linkcard::Card yt = c;
        yt.kind           = Kind::YouTube;
        yt.host           = QStringLiteral("youtu.be");
        yt.waiting        = Waiting::Sender;
        QVERIFY(linkcard::toolTip(yt, Zone::Body).contains(QStringLiteral("doesn't contact YouTube")));
        // The click card of a file above the automatic limit says its size; without one it is 2.4.0's.
        c.look = Look::Click;
        c.size = 38LL * 1024 * 1024;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Video · 38.0 MB"));
        QVERIFY(linkcard::toolTip(c, Zone::Load).contains(QStringLiteral("automatic downloads")));
        QVERIFY(linkcard::toolTip(c, Zone::Load).contains(QStringLiteral("IP address")));
        c.size = -1;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Video"));
        QVERIFY(!linkcard::toolTip(c, Zone::Load).contains(QStringLiteral("automatic downloads")));
        // Above the automatic limit while the site didn't say its size (no Content-Length): the limit as a lower bound,
        // and the tool tip says why it didn't load by itself.
        c.overAutoLimit = true;
        c.autoLimit     = 15LL * 1024 * 1024;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Video · over 15.0 MB"));
        QVERIFY(linkcard::toolTip(c, Zone::Load).contains(QStringLiteral("automatic downloads")));
        c.autoLimit = -1; // the limit isn't known (data saver since): the kind only, the reason still in the tip
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Video"));
        QVERIFY(linkcard::toolTip(c, Zone::Load).contains(QStringLiteral("automatic downloads")));
        c.overAutoLimit = false;

        // One size for every strip: Click, Click with a size, each Loading, Error and TooLarge, in every width.
        for (const int maxWidth : {160, 300, 520}) {
            for (const qreal dpr : {1.0, 2.0}) {
                for (const bool bubble : {false, true}) {
                    PreviewStyle style = styleFor(false, maxWidth, bubble);
                    style.dpr          = dpr;
                    QVector<linkcard::Card> strips;
                    linkcard::Card          base;
                    base.kind = Kind::Video;
                    base.host = QStringLiteral("files.example.com");
                    base.look = Look::Click;
                    strips << base;
                    base.size = 38LL * 1024 * 1024;
                    strips << base;
                    base.size          = -1;
                    base.overAutoLimit = true;
                    base.autoLimit     = 15LL * 1024 * 1024;
                    strips << base;
                    base.overAutoLimit = false;
                    base.autoLimit     = -1;
                    base.look          = Look::Loading;
                    for (const Waiting w : {Waiting::None, Waiting::Own, Waiting::Sender, Waiting::Checking}) {
                        base.waiting = w;
                        strips << base;
                    }
                    base.waiting = Waiting::None;
                    base.look    = Look::Error;
                    strips << base;
                    base.look = Look::TooLarge;
                    base.size = 62LL * 1024 * 1024;
                    strips << base;
                    const QString          tag   = QStringLiteral("width %1 dpr %2 bubble %3").arg(maxWidth).arg(dpr).arg(bubble);
                    const linkcard::Layout first   = linkcard::layout(strips.first(), style);
                    const QSize            pixels  = linkcard::render(strips.first(), style, linkcard::Pointer()).size();
                    for (const linkcard::Card& strip : qAsConst(strips)) {
                        const linkcard::Layout l = linkcard::layout(strip, style);
                        QVERIFY2(l.size == first.size, qPrintable(tag));
                        QVERIFY2(l.size.width() <= maxWidth, qPrintable(tag));
                        QCOMPARE(linkcard::render(strip, style, linkcard::Pointer()).size(), pixels);
                        // A waiting strip has nothing to click but its hide button.
                        if (strip.look == Look::Loading) {
                            QVERIFY(l.action.isEmpty());
                            QCOMPARE(linkcard::zoneAt(strip, l, QPointF(l.text.left() + 4, l.size.height() / 2.0), true), Zone::Body);
                            QVERIFY(!linkcard::isActionable(strip, Zone::Body));
                            QCOMPARE(linkcard::zoneAt(strip, l, l.hide.center(), true), Zone::Hide);
                        }
                    }
                }
            }
        }
    }

    // A YouTube card's title and channel come from the website or, for a sender's preview, from the sender's LP
    // (anyone with the plugin). QToolTip shows a string whose first line looks like HTML as rich text, and rich
    // text loads the pictures it names from disk; "file://host/share/x.png" is a Windows file share request to
    // that host (it gets the Windows account's NTLM hash). What the chat hands to QToolTip must stay plain text.
    void toolTipIsPlainText()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage probe(40, 300, QImage::Format_RGB32);
        probe.fill(Qt::red);
        const QString probePath = dir.path() + QStringLiteral("/probe.png");
        QVERIFY(probe.save(probePath, "PNG"));
        QCOMPARE(QUrl(QStringLiteral("file://share.example.com/s/x.png")).toLocalFile(), QStringLiteral("//share.example.com/s/x.png"));

        linkcard::Card c;
        c.look   = linkcard::Look::YouTube;
        c.kind   = Kind::YouTube;
        c.host   = QStringLiteral("www.youtube.com");
        c.title  = linkcard::cleanText(QStringLiteral("<img src=\"%1\"> & more").arg(QUrl::fromLocalFile(probePath).toString()), 200);
        c.author = linkcard::cleanText(QStringLiteral("<b>A channel</b>"), 100);
        QVERIFY(c.title.startsWith(QLatin1String("<img"))); // cleaning keeps it: it is ordinary text

        // The hazard: the plain text as it is would be rich text to QToolTip, and its picture is read from disk.
        const QString plain = linkcard::toolTip(c, linkcard::Zone::Body);
        QVERIFY(Qt::mightBeRichText(plain));
        QLabel raw; // QToolTip's label (Qt::AutoText)
        raw.setText(plain);
        QVERIFY(raw.sizeHint().height() >= 300);

        // What the chat shows (ChatLinkPreviews' ToolTip event): every zone of every look, escaped.
        for (const linkcard::Look look : {linkcard::Look::Click, linkcard::Look::Loading, linkcard::Look::Error, linkcard::Look::TooLarge,
                                          linkcard::Look::YouTube, linkcard::Look::Poster, linkcard::Look::Hidden}) {
            for (const linkcard::Zone zone : {linkcard::Zone::Body, linkcard::Zone::Load, linkcard::Zone::Retry, linkcard::Zone::Open,
                                              linkcard::Zone::Hide, linkcard::Zone::Show}) {
                for (const int variant : {0, 1, 2, 3, 4}) { // 2.4 auto-load: plain, waiting for us / the sender, with a size, checking
                    linkcard::Card any = c;
                    any.look           = look;
                    any.error          = c.title;
                    any.kind           = variant == 0 ? Kind::YouTube : Kind::Image;
                    any.host           = variant == 0 ? c.host : QStringLiteral("<img src=\"%1\">.example.org").arg(QUrl::fromLocalFile(probePath).toString());
                    any.waiting        = variant == 1   ? linkcard::Waiting::Own
                                         : variant == 2 ? linkcard::Waiting::Sender
                                         : variant == 4 ? linkcard::Waiting::Checking
                                                        : linkcard::Waiting::None;
                    any.size           = variant == 3 ? 38LL * 1024 * 1024 : -1;
                    any.overAutoLimit  = variant == 4;
                    const QString shown = linkcard::toolTipHtml(any, zone);
                    QCOMPARE(shown.isEmpty(), linkcard::toolTip(any, zone).isEmpty());
                    if (shown.isEmpty())
                        continue;
                    QLabel label;
                    label.setText(shown);
                    QVERIFY2(label.sizeHint().height() < 200, "the tool tip loaded a picture named in a title or a host");
                    QTextDocument doc;
                    doc.setHtml(shown);
                    QCOMPARE(doc.toPlainText(), linkcard::toolTip(any, zone)); // shown exactly as written, lines kept
                }
            }
        }
        QTextDocument doc;
        doc.setHtml(linkcard::toolTipHtml(c, linkcard::Zone::Body));
        QVERIFY(doc.toPlainText().contains(c.title));
        QVERIFY(doc.toPlainText().contains(c.author));
        // Live test: a YouTube card is one Open zone (zoneAt), so pointing at it shows that same tip, title and channel
        // in full; other cards' Open zone says where it opens.
        QCOMPARE(linkcard::primaryZone(c), linkcard::Zone::Open);
        QCOMPARE(linkcard::toolTipHtml(c, linkcard::Zone::Open), linkcard::toolTipHtml(c, linkcard::Zone::Body));
        linkcard::Card poster = c;
        poster.look           = linkcard::Look::Poster;
        QVERIFY(!linkcard::toolTip(poster, linkcard::Zone::Open).contains(c.title));
    }

    // ---- what a slot shows ----------------------------------------------------------------------------------------
    void decisions()
    {
        using linkcard::Look;
        using linkstate::Local;
        linkstate::LocalState  idle;
        linkstate::ServerState none;
        QCOMPARE(linkstate::decide(Kind::Image, false, idle, none).look, Look::Click);
        QCOMPARE(linkstate::decide(Kind::Image, true, idle, none).look, Look::Hidden);
        linkstate::LocalState loading = idle;
        loading.state                 = Local::Loading;
        QCOMPARE(linkstate::decide(Kind::Gif, false, loading, none).look, Look::Loading);
        linkstate::LocalState failed = idle;
        failed.state                 = Local::Failed;
        QCOMPARE(linkstate::decide(Kind::Gif, false, failed, none).look, Look::Error);
        linkstate::LocalState large = idle;
        large.state                 = Local::TooLarge;
        QCOMPARE(linkstate::decide(Kind::Video, false, large, none).look, Look::TooLarge);
        linkstate::LocalState loaded = idle;
        loaded.state                 = Local::Loaded;
        loaded.mediaKey              = QStringLiteral("webkey");
        linkstate::Decision d        = linkstate::decide(Kind::Image, false, loaded, none);
        QVERIFY(d.media);
        QCOMPARE(d.mediaKey, QStringLiteral("webkey"));
        QCOMPARE(linkstate::decide(Kind::YouTube, false, loaded, none).look, Look::YouTube);
        // A sender's preview upgrades a click card, a failed or loading one; never a hidden one or one loaded here.
        linkstate::ServerState server;
        server.announced = true;
        server.mainKey   = QStringLiteral("serverkey");
        server.hasMedia  = true;
        for (const linkstate::LocalState& l : {idle, loading, failed, large}) {
            d = linkstate::decide(Kind::Image, false, l, server);
            QVERIFY(d.media && d.fromServer);
            QCOMPARE(d.mediaKey, QStringLiteral("serverkey"));
        }
        QCOMPARE(linkstate::decide(Kind::Image, true, idle, server).look, Look::Hidden);
        QCOMPARE(linkstate::decide(Kind::Image, false, loaded, server).mediaKey, QStringLiteral("webkey"));
        QCOMPARE(linkstate::decide(Kind::YouTube, false, idle, server).look, Look::YouTube);
        QVERIFY(linkstate::decide(Kind::YouTube, false, idle, server).fromServer);
        linkstate::ServerState poster = server;
        poster.hasMedia               = false;
        QCOMPARE(linkstate::decide(Kind::Video, false, idle, poster).look, Look::Poster);
        QVERIFY(linkstate::decide(Kind::Video, false, idle, server).media);
        // Its files failing for good: back to the click card (the website can still be asked).
        linkstate::ServerState gone = server;
        gone.state                  = MediaState::Failed;
        gone.error                  = MediaError::NotFound;
        QCOMPARE(linkstate::decide(Kind::Image, false, idle, gone).look, Look::Click);
        QCOMPARE(linkstate::decide(Kind::Image, false, failed, gone).look, Look::Error);
        for (const MediaError forGood : {MediaError::Permission, MediaError::Password}) {
            gone.error = forGood;
            QCOMPARE(linkstate::decide(Kind::Image, false, idle, gone).look, Look::Click);
        }
        // A failure that may pass keeps the sender's preview (Core's "Click to try again" from the TeamSpeak
        // server), never a card that invites loading it from the website.
        for (const MediaError passing : {MediaError::Other, MediaError::NotConnected, MediaError::Quota, MediaError::Mismatch}) {
            linkstate::ServerState hiccup = server;
            hiccup.state                  = MediaState::Failed;
            hiccup.error                  = passing;
            d                             = linkstate::decide(Kind::Image, false, idle, hiccup);
            QVERIFY(d.media && d.fromServer);
            QCOMPARE(d.mediaKey, QStringLiteral("serverkey"));
            QCOMPARE(linkstate::decide(Kind::YouTube, false, idle, hiccup).look, Look::YouTube);
        }
    }

    // 2.4 auto-load: what a slot waits for comes last (nothing shown, loading, failed or too large is ever replaced
    // by a wait), and every wait is the loading strip.
    void decisionsWithWaits()
    {
        using linkcard::Look;
        using linkstate::Local;
        using linkstate::Wait;
        linkstate::LocalState  idle;
        linkstate::ServerState none;
        for (const Wait w : {Wait::Own, Wait::Sender, Wait::Checking, Wait::Auto}) {
            const linkstate::Decision d = linkstate::decide(Kind::Image, false, idle, none, w);
            QCOMPARE(d.look, Look::Loading);
            QCOMPARE(d.wait, w);
            QVERIFY(!d.media && !d.fromServer);
            QCOMPARE(linkstate::decide(Kind::YouTube, false, idle, none, w).look, Look::Loading);
            // Hidden beats every wait.
            QCOMPARE(linkstate::decide(Kind::Image, true, idle, none, w).look, Look::Hidden);
        }
        // The old decisions with the default argument: no wait.
        QCOMPARE(linkstate::decide(Kind::Image, false, idle, none).look, Look::Click);
        QCOMPARE(linkstate::decide(Kind::Image, false, idle, none).wait, Wait::None);
        QCOMPARE(linkstate::decide(Kind::Image, false, idle, none, Wait::None).look, Look::Click);
        // Loaded here beats every wait.
        linkstate::LocalState loaded = idle;
        loaded.state                 = Local::Loaded;
        loaded.mediaKey              = QStringLiteral("webkey");
        linkstate::LocalState youtube = idle;
        youtube.state                 = Local::Loaded;
        for (const Wait w : {Wait::Own, Wait::Sender, Wait::Checking, Wait::Auto}) {
            const linkstate::Decision d = linkstate::decide(Kind::Gif, false, loaded, none, w);
            QVERIFY(d.media);
            QCOMPARE(d.mediaKey, QStringLiteral("webkey"));
            QCOMPARE(d.wait, Wait::None);
            QCOMPARE(linkstate::decide(Kind::YouTube, false, youtube, none, w).look, Look::YouTube);
        }
        // The sender's preview beats every wait (it is what the wait was for).
        linkstate::ServerState server;
        server.announced = true;
        server.mainKey   = QStringLiteral("serverkey");
        server.hasMedia  = true;
        for (const Wait w : {Wait::Own, Wait::Sender, Wait::Checking, Wait::Auto}) {
            const linkstate::Decision d = linkstate::decide(Kind::Image, false, idle, server, w);
            QVERIFY(d.media && d.fromServer);
            QCOMPARE(d.wait, Wait::None);
        }
        // Loading, failed and too large here beat every wait (a running load shows its own progress: wait None).
        linkstate::LocalState loading = idle;
        loading.state                 = Local::Loading;
        loading.progress              = 0.3;
        linkstate::LocalState failed = idle;
        failed.state                 = Local::Failed;
        linkstate::LocalState large = idle;
        large.state                 = Local::TooLarge;
        for (const Wait w : {Wait::Own, Wait::Sender, Wait::Checking, Wait::Auto}) {
            linkstate::Decision d = linkstate::decide(Kind::Image, false, loading, none, w);
            QCOMPARE(d.look, Look::Loading);
            QCOMPARE(d.wait, Wait::None);
            QCOMPARE(linkstate::decide(Kind::Image, false, failed, none, w).look, Look::Error);
            QCOMPARE(linkstate::decide(Kind::Video, false, large, none, w).look, Look::TooLarge);
        }
        // Above the automatic limit: a click card, never an automatic load; waiting for our pipeline or the sender
        // still waits (their preview is small).
        linkstate::LocalState over = idle;
        over.state                 = Local::OverAutoLimit;
        over.size                  = 38LL * 1024 * 1024;
        QCOMPARE(linkstate::decide(Kind::Video, false, over, none).look, Look::Click);
        QCOMPARE(linkstate::decide(Kind::Video, false, over, none, Wait::Auto).look, Look::Click);
        QCOMPARE(linkstate::decide(Kind::Video, false, over, none, Wait::Auto).wait, Wait::None);
        QCOMPARE(linkstate::decide(Kind::Video, false, over, none, Wait::Sender).look, Look::Loading);
        QCOMPARE(linkstate::decide(Kind::Video, false, over, none, Wait::Own).wait, Wait::Own);
        QVERIFY(linkstate::decide(Kind::Video, false, over, server, Wait::Auto).fromServer);
        QCOMPARE(linkstate::decide(Kind::Video, true, over, none, Wait::Sender).look, Look::Hidden);
        // A sender's preview whose files failed for good: the website is all that is left (an automatic load may
        // start; R13).
        linkstate::ServerState gone = server;
        gone.state                  = MediaState::Failed;
        gone.error                  = MediaError::NotFound;
        QCOMPARE(linkstate::decide(Kind::Image, false, idle, gone, Wait::Auto).look, Look::Loading);
        QCOMPARE(linkstate::decide(Kind::Image, false, idle, gone, Wait::Auto).wait, Wait::Auto);
    }

    void serverAndWebLinks()
    {
        // A sender's files: the very link the sender-previews' linkFor() makes (one Core entry, whoever asks).
        linkpreview::Announcement a;
        a.messageKey  = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000);
        a.urlKey      = QStringLiteral("00112233445566778899aabbccddeeff");
        a.senderUid   = kAlice;
        a.kind        = Kind::Video;
        a.previewPath = QStringLiteral("/tsmedia/previews/p1.jpg");
        a.previewSha  = sha43('p');
        a.previewSize = 2345;
        a.mediaPath   = QStringLiteral("/tsmedia/v1.mp4");
        a.mediaSha    = sha43('m');
        a.mediaSize   = 123456;
        a.width       = 1280;
        a.height      = 720;
        a.durationMs  = 15000;
        MediaLink main;
        QVERIFY(!linkstate::serverLink(a, &main)); // no file browser: nothing
        a.channelId = 5;
        QVERIFY(!linkstate::serverLink(a, &main));
        a = linkstate::withFileBrowser(a, QStringLiteral("srv"), 9);
        QCOMPARE(a.serverUid, QStringLiteral("srv"));
        QCOMPARE(a.channelId, quint64(5)); // what the announcement says wins
        QVERIFY(linkstate::serverLink(a, &main));
        QCOMPARE(main.key(), linkpreview::linkFor(a).key());
        QCOMPARE(main.remoteFile(), QStringLiteral("/tsmedia/v1.mp4"));
        QCOMPARE(main.size, quint64(123456));
        QCOMPARE(main.sha256, QByteArray(32, 'm'));
        QCOMPARE(main.previewFile, QStringLiteral("/tsmedia/previews/p1.jpg"));
        QCOMPARE(main.previewSha, QByteArray(16, 'p'));
        QCOMPARE(main.durationMs, qint64(15000));
        QCOMPARE(kindForFileName(main.fileName), MediaKind::Video);
        QVERIFY(main.isTsMedia());
        // Without the full file (too large): the preview picture is the main file, checked against its own hash.
        a.mediaPath.clear();
        a.mediaSha.clear();
        a.mediaSize = 0;
        QVERIFY(linkstate::serverLink(a, &main));
        QCOMPARE(main.key(), linkpreview::linkFor(a).key());
        QCOMPARE(main.remoteFile(), QStringLiteral("/tsmedia/previews/p1.jpg"));
        QCOMPARE(main.sha256, QByteArray(32, 'p'));
        QCOMPARE(main.size, quint64(2345));
        QVERIFY(main.previewFile.isEmpty());
        // Nothing that can be checked, a path that climbs: nothing.
        linkpreview::Announcement bad = a;
        bad.previewSha.clear();
        QVERIFY(!linkstate::serverLink(bad, &main));
        bad             = a;
        bad.previewPath = QStringLiteral("/../etc/x.jpg");
        QVERIFY(!linkstate::serverLink(bad, &main));

        // A website's file: a made-up link, its name from the address, its extension from its content.
        const linkpreview::Target t = linkpreview::classify(QStringLiteral("https://cdn.example.org/pics/My%20Lake!.gif?x=1"));
        const MediaLink           w = linkstate::webLink(t, QStringLiteral("image/png"), 2048, 640, 480, 0);
        QVERIFY(w.isValid());
        QVERIFY(linkstate::isWebLink(w));
        QCOMPARE(w.fileName, QStringLiteral("My_Lake_.png"));
        QCOMPARE(kindForFileName(w.fileName), MediaKind::Image);
        QCOMPARE(linkstate::webLink(t, QString(), 1, 0, 0, 0).fileName, QStringLiteral("My_Lake_.gif"));
        QVERIFY(w.key() != linkstate::webLink(linkpreview::classify(kGif), QStringLiteral("image/png"), 2048, 640, 480, 0).key());
    }

    // Open in browser / Copy link: the link as it was written (a YouTube start time and host stay), always https.
    void webAddressIsTheLinkAsWritten()
    {
        const auto check = [](const QString& href, const QString& want) {
            const linkpreview::Target t = linkpreview::classify(href);
            QVERIFY2(t.isValid(), qPrintable(href));
            QCOMPARE(linkstate::webAddress(href, t).toString(QUrl::FullyEncoded), want);
        };
        check(QStringLiteral("https://youtu.be/aBcDeFgHiJk?t=90"), QStringLiteral("https://youtu.be/aBcDeFgHiJk?t=90"));
        check(QStringLiteral("http://www.youtube.com/watch?v=aBcDeFgHiJk&t=1m30s"), QStringLiteral("https://www.youtube.com/watch?v=aBcDeFgHiJk&t=1m30s"));
        check(QStringLiteral("http://youtube.com:80/shorts/aBcDeFgHiJk"), QStringLiteral("https://youtube.com/shorts/aBcDeFgHiJk"));
        check(QStringLiteral("https://cdn.example.org:443/pics/lake.png?w=2#top"), QStringLiteral("https://cdn.example.org/pics/lake.png?w=2#top"));
        // Not that link (or none): the checked address.
        const linkpreview::Target yt = linkpreview::classify(QStringLiteral("https://youtu.be/aBcDeFgHiJk"));
        QCOMPARE(linkstate::webAddress(QStringLiteral("https://youtu.be/xYz123AbC_d"), yt), yt.url);
        QCOMPARE(linkstate::webAddress(QString(), yt), yt.url);
        QCOMPARE(linkstate::webAddress(QStringLiteral("https://user@youtu.be/aBcDeFgHiJk"), yt), yt.url);
        QCOMPARE(yt.url.toString(), QStringLiteral("https://www.youtube.com/watch?v=aBcDeFgHiJk"));
    }

    // A failed website fetch: one neutral line for everything about the connection (the reason is logged).
    void errorTextIsNeutral()
    {
        namespace errors = linkpreview::errors;
        for (const char* code : {errors::kBlocked, errors::kOffline, errors::kTimeout, errors::kTls, errors::kRedirect, errors::kNoNetwork, errors::kForbidden,
                                 errors::kRateLimited, errors::kServerError, errors::kBadResponse, errors::kBusy, errors::kDisk, errors::kInternal, "anything"})
            QVERIFY2(linkstate::errorText(QString::fromLatin1(code)).isEmpty(), code);
        linkcard::Card c;
        c.look = linkcard::Look::Error;
        c.host = QStringLiteral("cdn.example.org");
        c.error = linkstate::errorText(QString::fromLatin1(errors::kBlocked));
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Couldn't load the preview"));
        c.error = linkstate::errorText(QString::fromLatin1(errors::kTimeout));
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Couldn't load the preview"));
        QVERIFY(!linkstate::errorText(QString::fromLatin1(errors::kUnavailable)).isEmpty());
        QVERIFY(!linkstate::errorText(QString::fromLatin1(errors::kNotFound)).isEmpty());
    }

    // The message's key with a link preview in it is the same on the sender (its BBCode, its own chat) and on a
    // receiver (TeamSpeak's document with our slot: a card, or the link's media as a "tsmedia:<key>" preview), so
    // the store finds the sender's announcement and the text reactions keep their rows.
    void linkPreviewKeepsTheMessageKey()
    {
        const tschat::Skin s     = tschat::darkSkin();
        const QString      bbcode = QStringLiteral("watch this [URL]") + kYouTube + QStringLiteral("[/URL] and [URL]") + kPng + QStringLiteral("[/URL] ok");
        const QString      html   = QStringLiteral("watch this ") + link(s, kYouTube) + QStringLiteral(" and ") + link(s, kPng) + QStringLiteral(" ok");
        const QDate        today(2026, 10, 10);
        const auto         keyOf = [&](QTextDocument& doc, int block) {
            const QVector<QDate>         dates = layoutdoc::datesOf(&doc, today);
            const textreactdoc::Identity id    = textreactdoc::identify(doc.findBlockByNumber(block));
            return textreactdoc::localKeyOf(id, textreactdoc::dayOf(dates.value(block), id.seconds, today, 23 * 3600), 0);
        };
        for (const bool sender : {true, false}) {
            QTextDocument doc;
            setup(doc);
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:01:00"), sender ? 3 : 17, kAlice, QStringLiteral("Alice"), html, sender));
            const textreactdoc::Identity before = textreactdoc::identify(doc.firstBlock());
            QVERIFY(before.eligible());
            // What the sender computes from its BBCode is what the chat document says.
            QCOMPARE(before.hash, linkpreview::message::identityHash(kAlice, bbcode));
            const QString key = keyOf(doc, 0);
            QVERIFY(!key.isEmpty());
            // The slots: the YouTube card, the picture as Core's preview object (as ChatIntegration draws it).
            const QVector<linkdoc::Link> links = linkdoc::findLinks(&doc);
            QCOMPARE(links.size(), 2);
            QVERIFY(linkdoc::insertSlot(&doc, links.at(1), linkdoc::mediaPrefix() + QStringLiteral("0123456789abcdef0123"), QSizeF(260, 188)));
            QVERIFY(linkdoc::insertSlot(&doc, links.at(0), linkdoc::cardPrefix() + QStringLiteral("t0"), QSizeF(340, 260)));
            QCOMPARE(linkdoc::allSlots(&doc).size(), 2);
            const textreactdoc::Identity after = textreactdoc::identify(doc.firstBlock());
            QVERIFY(after.eligible()); // still a text message, not a media one
            QCOMPARE(after.hash, before.hash);
            QCOMPARE(keyOf(doc, 0), key);
            // A card becoming the media (the sender's preview arrived) changes nothing either.
            const linkdoc::Slot card = linkdoc::allSlots(&doc).first();
            QVERIFY(linkdoc::replaceObject(&doc, card.object, linkdoc::mediaPrefix() + QStringLiteral("fedcba9876543210fedc"), QSizeF(400, 225)));
            QCOMPARE(keyOf(doc, 0), key);
            // The store finds the announcement the sender made under its own key (its echo time, a second off).
            linkpreview::PreviewStore store;
            linkpreview::Announcement a;
            qint64                    t = 0;
            QString                   hash;
            QVERIFY(textreact::parseLocalKey(key, &hash, &t));
            a.messageKey  = textreact::localKey(hash, t + 1);
            a.urlKey      = links.at(0).target.key;
            a.kind        = Kind::YouTube;
            a.senderUid   = kAlice;
            a.serverUid   = QString::fromLatin1(fakets3::kServerUid);
            a.channelId   = fakets3::kChannel;
            a.previewPath = QStringLiteral("/tsmedia/previews/yt1.jpg");
            a.previewSha  = sha43('y');
            a.previewSize = 1234;
            QCOMPARE(store.add(a, t + 5), linkpreview::PreviewStore::Add::Added);
            QVERIFY(store.find(keyOf(doc, 0), links.at(0).target.key));
            QVERIFY(linkdoc::removeAll(&doc) == 2);
            QCOMPARE(keyOf(doc, 0), key);
        }
    }

    // Core shows a website's file like received media and never asks a TeamSpeak server for it; a sender's
    // preview is downloaded from the TeamSpeak server like any TS Media file.
    void coreKeepsWebFilesLocal()
    {
        const TS3Functions savedFuncs = ts3::funcs;
        const Settings     saved      = Settings::instance();
        QTemporaryDir      dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        Settings::instance() = Settings();
        {
            auto core = std::make_unique<Core>();
            core->start();
            const QString links = core->cacheDir() + QStringLiteral("/links");
            QVERIFY(QDir().mkpath(links));
            const QString file = links + QStringLiteral("/lake.png");
            QImage        img(64, 48, QImage::Format_RGB32);
            img.fill(Qt::darkCyan);
            QVERIFY(img.save(file));
            const linkpreview::Target t    = linkpreview::classify(QUrl(kPng));
            const MediaLink           link = linkstate::webLink(t, QStringLiteral("image/png"), QFileInfo(file).size(), 64, 48, 0);
            const QString             key  = core->adoptLocalFile(link, file);
            QVERIFY(!key.isEmpty());
            QVERIFY(core->isLocalFile(key));
            const MediaEntry* e = core->entry(key);
            QVERIFY(e);
            QCOMPARE(e->state, MediaState::Ready);
            QCOMPARE(e->localPath, file);
            QCOMPARE(e->kind, MediaKind::Image);
            QCOMPARE(core->still(key, QSize(64, 48)).source, MediaStill::Full);
            core->download(key, false);
            core->retry(key);
            QVERIFY(fakets3::downloads().isEmpty());
            QVERIFY(core->adoptLocalFile(link, dir.path() + QStringLiteral("/missing.png")).isEmpty());
            // Clear cache: its file goes, the entry waits for a click (never a TeamSpeak download).
            core->clearCache();
            QVERIFY(core->entry(key)->state != MediaState::Ready);
            core->download(key, false);
            core->retry(key);
            QVERIFY(fakets3::downloads().isEmpty());

            // A sender's announced picture: from the TeamSpeak server (its own channel), checked against its hash.
            linkpreview::Announcement a;
            a.messageKey  = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000);
            a.urlKey      = linkpreview::classify(kPng).key;
            a.senderUid   = kAlice;
            a.kind        = Kind::Image;
            a.previewPath = QStringLiteral("/tsmedia/previews/p2.jpg");
            a.previewSha  = sha43('q');
            a.previewSize = 777;
            a             = linkstate::withFileBrowser(a, QString::fromLatin1(fakets3::kServerUid), fakets3::kChannel);
            MediaLink server;
            QVERIFY(linkstate::serverLink(a, &server));
            linkpreview::requestFiles(core.get(), a); // what the hub and the chat do
            QVERIFY(core->entry(server.key()));
            QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 1, 2000);
            QCOMPARE(fakets3::downloads().first().remotePath, QStringLiteral("/tsmedia/previews/p2.jpg"));
            QVERIFY(!core->isLocalFile(server.key()));
            // A real link's key is never taken over by a website's file.
            QVERIFY(core->adoptLocalFile(server, file).isEmpty());
        }
        ts3::funcs           = savedFuncs;
        Settings::instance() = saved;
    }

    // ---- 2.4 auto-load: caps, waits, the per-sender budget -------------------------------------------------------
    void autoCapPerKind()
    {
        using linkpreview::autoload::autoCap;
        using linkpreview::autoload::clickCap;
        constexpr qint64 MB = 1024 * 1024;
        // The click caps are 2.4.0's (ChatLinkPreviews::maxBytesFor returns them).
        QCOMPARE(clickCap(Kind::YouTube), 2 * MB);
        QCOMPARE(clickCap(Kind::Image), 25 * MB);
        QCOMPARE(clickCap(Kind::Gif), 25 * MB);
        QCOMPARE(clickCap(Kind::Video), 50 * MB);
        const QString server = QStringLiteral("serverA=");
        const QString other  = QStringLiteral("serverB=");
        Settings      s; // the defaults: pictures and GIFs up to 15 MB, videos when you press play
        QCOMPARE(autoCap(Kind::YouTube, s, server), 2 * MB);
        QCOMPARE(autoCap(Kind::Image, s, server), 15 * MB);
        QCOMPARE(autoCap(Kind::Gif, s, QString()), 15 * MB);
        // Link videos load by themselves by default: with the video limit at 0 ("when you press play", which is
        // about uploaded videos that show a poster anyway) they use the picture limit.
        QCOMPARE(s.videoAutoDownloadMB, 0);
        QCOMPARE(autoCap(Kind::Video, s, server), 15 * MB);
        QCOMPARE(autoCap(Kind::None, s, server), 0LL);
        s.autoDownloadMaxMB = 40;
        QCOMPARE(autoCap(Kind::Video, s, server), 40 * MB);
        s.autoDownloadMaxMB = 15;
        s.autoDownloadImages = false; // ... and with automatic picture downloads off, none of them
        QCOMPARE(autoCap(Kind::Video, s, server), 0LL);
        s.autoDownloadImages = true;
        // Never above what a click loads.
        s.autoDownloadMaxMB   = 400;
        s.videoAutoDownloadMB = 900;
        QCOMPARE(autoCap(Kind::Image, s, server), 25 * MB);
        QCOMPARE(autoCap(Kind::Video, s, server), 50 * MB);
        s.autoDownloadMaxMB   = 3;
        s.videoAutoDownloadMB = 20;
        QCOMPARE(autoCap(Kind::Image, s, server), 3 * MB);
        QCOMPARE(autoCap(Kind::Gif, s, server), 3 * MB);
        QCOMPARE(autoCap(Kind::Video, s, server), 20 * MB);
        // Automatic picture downloads off: no automatic picture or GIF; videos with their own limit and YouTube keep
        // theirs.
        s.autoDownloadImages = false;
        QCOMPARE(autoCap(Kind::Image, s, server), 0LL);
        QCOMPARE(autoCap(Kind::Gif, s, server), 0LL);
        QCOMPARE(autoCap(Kind::Video, s, server), 20 * MB);
        QCOMPARE(autoCap(Kind::YouTube, s, server), 2 * MB);
        s.autoDownloadImages = true;
        // Data saver for all servers: only YouTube.
        s.dataSaver = true;
        for (const Kind k : {Kind::Image, Kind::Gif, Kind::Video})
            QCOMPARE(autoCap(k, s, server), 0LL);
        QCOMPARE(autoCap(Kind::YouTube, s, server), 2 * MB);
        // A server's own data saver: that server only (on), or off there while it is on for the others.
        s.dataSaver = false;
        ServerOverrides own;
        own.dataSaver = true;
        s.servers.insert(serversettings::serverKey(server), own);
        QCOMPARE(autoCap(Kind::Image, s, server), 0LL);
        QCOMPARE(autoCap(Kind::Video, s, server), 0LL);
        QCOMPARE(autoCap(Kind::YouTube, s, server), 2 * MB);
        QCOMPARE(autoCap(Kind::Image, s, other), 3 * MB);
        QCOMPARE(autoCap(Kind::Image, s, QString()), 3 * MB);
        s.dataSaver = true;
        own.dataSaver = false;
        s.servers.insert(serversettings::serverKey(server), own);
        QCOMPARE(autoCap(Kind::Image, s, server), 3 * MB);
        QCOMPARE(autoCap(Kind::Image, s, other), 0LL);
    }

    void waitRules_data()
    {
        using namespace linkpreview::autoload;
        QTest::addColumn<int>("rule");
        QTest::addColumn<int>("want");
        // One row per rule (waitFor's order); the input is built in waitRules from the rule number.
        const int none = static_cast<int>(Wait::None), own = static_cast<int>(Wait::Own), sender = static_cast<int>(Wait::Sender);
        const int checking = static_cast<int>(Wait::Checking);
        QTest::newRow("own: pipeline queued or fetching") << 1 << own;
        QTest::newRow("own: pending also with auto off") << 2 << own;
        QTest::newRow("own: no record, fresh (grace)") << 3 << own;
        QTest::newRow("own: no record, grace edge 8 s") << 4 << own;
        QTest::newRow("own: no record, grace over") << 5 << none;
        QTest::newRow("own: no record, Make off") << 6 << none;
        QTest::newRow("own: no record, server chat") << 7 << none;
        QTest::newRow("own: record done") << 8 << none;
        QTest::newRow("own: no key") << 9 << none;
        QTest::newRow("own: never waits for a sender") << 10 << none;
        QTest::newRow("others: auto off") << 11 << none;
        QTest::newRow("others: server chat") << 12 << none;
        QTest::newRow("others: unknown chat") << 13 << none;
        QTest::newRow("others: no key") << 14 << none;
        QTest::newRow("others: past the budget") << 15 << none;
        QTest::newRow("others: just inside the budget") << 16 << sender;
        QTest::newRow("others: a time far ahead") << 17 << none;
        QTest::newRow("others: a little ahead") << 18 << sender;
        QTest::newRow("others: skip notice") << 19 << none;
        QTest::newRow("others: sender gone") << 20 << none;
        QTest::newRow("others: cap yes") << 21 << sender;
        QTest::newRow("others: cap checking (no claim)") << 22 << checking;
        QTest::newRow("others: cap no") << 23 << none;
        QTest::newRow("others: cap unknown") << 24 << none;
        QTest::newRow("others: private chat") << 25 << sender;
        QTest::newRow("others: checking, then its time is up") << 26 << none;
        QTest::newRow("others: checking, sender gone") << 27 << none;
    }

    void waitRules()
    {
        using namespace linkpreview::autoload;
        QFETCH(int, rule);
        QFETCH(int, want);
        constexpr qint64 now = 1760000000;
        Input            in;
        in.nowSec    = now;
        in.chat      = Chat::Channel;
        in.timeSec   = now - 2;
        in.budgetSec = waitSec(Kind::Image);
        in.cap       = SenderCap::Yes;
        in.senderHere = true;
        if (rule <= 10) {
            in.own = true;
            in.cap = SenderCap::Unknown;
        }
        switch (rule) {
        case 1: in.ownRecord = OwnRecord::Pending; in.timeSec = now - 100; break;
        case 2: in.ownRecord = OwnRecord::Pending; in.autoOn = false; in.makeOn = false; break;
        case 3: in.autoOn = false; break; // the grace doesn't depend on the switch either
        case 4: in.timeSec = now - kOwnGraceSec; break;
        case 5: in.timeSec = now - kOwnGraceSec - 1; break;
        case 6: in.makeOn = false; break;
        case 7: in.chat = Chat::Server; break;
        case 8: in.ownRecord = OwnRecord::Done; break;
        case 9: in.timeSec = -1; break;
        case 10: in.ownRecord = OwnRecord::Done; in.cap = SenderCap::Yes; break;
        case 11: in.autoOn = false; break;
        case 12: in.chat = Chat::Server; break;
        case 13: in.chat = Chat::Unknown; break;
        case 14: in.timeSec = -1; break;
        case 15: in.timeSec = now - in.budgetSec; break;
        case 16: in.timeSec = now - in.budgetSec + 1; break;
        case 17: in.timeSec = now + 301; break;
        case 18: in.timeSec = now + 20; break;
        case 19: in.skipped = true; break;
        case 20: in.senderHere = false; break;
        case 21: break;
        case 22: in.cap = SenderCap::Checking; break;
        case 23: in.cap = SenderCap::No; break;
        case 24: in.cap = SenderCap::Unknown; break;
        case 25: in.chat = Chat::Private; break;
        case 26: in.cap = SenderCap::Checking; in.timeSec = now - in.budgetSec; break;
        case 27: in.cap = SenderCap::Checking; in.senderHere = false; break;
        default: QFAIL("no such rule");
        }
        QCOMPARE(static_cast<int>(waitFor(in)), want);
    }

    void waitBudgets()
    {
        using namespace linkpreview::autoload;
        QCOMPARE(waitSec(Kind::YouTube), 30);
        QCOMPARE(waitSec(Kind::Image), 45);
        QCOMPARE(waitSec(Kind::Gif), 45);
        QCOMPARE(waitSec(Kind::Video), 120);
        QVERIFY(waitSec(Kind::Video) > 67); // the TeaSpeak video's 67 s fit
        QCOMPARE(waitSec(Kind::None), 0);
        // The longest: the sender's queue, then its own wait.
        QCOMPARE(kSenderQueueSec * 1000LL, linkpreview::SenderPipeline::Limits().queuedTtlMs);
        QCOMPARE(maxWaitSec(Kind::YouTube), 150);
        QCOMPARE(maxWaitSec(Kind::Image), 165);
        QCOMPARE(maxWaitSec(Kind::Gif), 165);
        QCOMPARE(maxWaitSec(Kind::Video), 240);
        QCOMPARE(maxWaitSec(Kind::None), 0);

        constexpr qint64 t = 1760000000;
        SenderQueue      q;
        QCOMPARE(q.add(kBob, t, Kind::YouTube), 30); // one link: its own wait
        QCOMPARE(q.add(kBob, t, Kind::Video), 150);  // the message's next link: after the first
        // A message 10 s later waits for the earlier links' turn too (its job only starts after theirs), as long as the
        // sender keeps a queued job: started 120 s after it was queued at the latest (else dropped, with LPNO).
        QCOMPARE(q.add(kBob, t + 10, Kind::Image), 165);
        QCOMPARE(q.add(kAlice, t + 10, Kind::Image), 45);   // another sender's queue is their own
        QCOMPARE(q.add(kBob, t + 1000, Kind::YouTube), 30); // long after: its own wait only
        // A job queued longer than the sender keeps it is dropped (and LPNO comes): never more than maxWaitSec.
        SenderQueue busy;
        for (int i = 0; i < 8; ++i)
            QVERIFY(busy.add(kBob, t, Kind::Video) <= maxWaitSec(Kind::Video));
        QCOMPARE(busy.add(kBob, t, Kind::Video), maxWaitSec(Kind::Video));
        QCOMPARE(busy.add(kBob, t + 30, Kind::YouTube), maxWaitSec(Kind::YouTube));
        // No key or no previewable link: no wait, nothing counted.
        SenderQueue keyless;
        QCOMPARE(keyless.add(kBob, -1, Kind::Video), 0);
        QCOMPARE(keyless.add(kBob, t, Kind::None), 0);
        QCOMPARE(keyless.add(kBob, t, Kind::YouTube), 30);
        keyless.clear();
        QCOMPARE(keyless.add(kBob, t, Kind::YouTube), 30);

        // The review's flow: Bob posts a video link, then 10 s later a YouTube link. His pipeline makes the video's
        // preview first (67 s on TeaSpeak), so the YouTube link still waits for his preview once its own 30 s are
        // over, and loads by itself only when the video's turn and its own are both over.
        SenderQueue chat;
        QCOMPARE(chat.add(kBob, t, Kind::Video), 120);
        Input in;
        in.chat       = Chat::Channel;
        in.cap        = SenderCap::Yes;
        in.senderHere = true;
        in.timeSec    = t + 10;
        in.budgetSec  = chat.add(kBob, t + 10, Kind::YouTube);
        QCOMPARE(in.budgetSec, 140);
        in.nowSec = t + 10 + waitSec(Kind::YouTube) + 1;
        QCOMPARE(waitFor(in), Wait::Sender);
        in.nowSec = t + 67 + 20; // the video's preview came at 67 s; his YouTube job is on its way
        QCOMPARE(waitFor(in), Wait::Sender);
        in.nowSec = t + 10 + in.budgetSec - 1;
        QCOMPARE(waitFor(in), Wait::Sender);
        in.nowSec = t + 10 + in.budgetSec;
        QCOMPARE(waitFor(in), Wait::None);
    }

    // A sender's presence is one answer for every chat kind (a private chat's lookup never makes a channel sender look
    // absent), and someone connected twice with one identity is in our channel when either client is.
    void presenceForEveryChat()
    {
        using namespace linkpreview::autoload;
        PresenceMemo memo;
        int          lookups = 0;
        const auto   lookup  = [&lookups](quint64, const QString&) {
            ++lookups;
            Presence p;
            p.client       = 12;
            p.inOurChannel = true;
            return p;
        };
        QVERIFY(senderHere(Chat::Private, memo.get(1, kBob, 1000, lookup)));
        QVERIFY(senderHere(Chat::Channel, memo.get(1, kBob, 1500, lookup))); // the private chat's lookup, still right
        QCOMPARE(lookups, 1);
        memo.get(1, kBob, 1000 + PresenceMemo::kTtlMs, lookup); // a second later: looked up again
        QCOMPARE(lookups, 2);
        memo.get(2, kBob, 1000 + PresenceMemo::kTtlMs, lookup); // another connection: its own
        QCOMPARE(lookups, 3);
        for (int i = 0; i < 2 * PresenceMemo::kMaxEntries; ++i)
            memo.get(1, QString::number(i), 5000, lookup);
        QVERIFY(memo.size() <= PresenceMemo::kMaxEntries);
        memo.clear();
        QCOMPARE(memo.size(), 0);
        Presence elsewhere;
        elsewhere.client = 12;
        QVERIFY(senderHere(Chat::Private, elsewhere));
        QVERIFY(!senderHere(Chat::Channel, elsewhere));
        QVERIFY(!senderHere(Chat::Private, Presence()));
        Presence here     = elsewhere;
        here.inOurChannel = true;
        QVERIFY(senderHere(Chat::Channel, here));
        QVERIFY(!senderHere(Chat::Server, here));
        QVERIFY(!senderHere(Chat::Unknown, here));

        const TS3Functions savedFuncs = ts3::funcs;
        QTemporaryDir      dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setOwnUid(kMe);
        fakets3::addClient({20, kBob, 9, QStringLiteral("Bob")}); // listed first, in another channel
        fakets3::addClient({21, kBob, fakets3::kChannel, QStringLiteral("Bob (2)")});
        fakets3::addClient({22, kAlice, 9, QStringLiteral("Alice")});
        ts3::UidPresence bob = ts3::uidPresence(fakets3::kConnection, kBob);
        QCOMPARE(bob.client, anyID(21));
        QVERIFY(bob.inOwnChannel);
        const ts3::UidPresence alice = ts3::uidPresence(fakets3::kConnection, kAlice);
        QCOMPARE(alice.client, anyID(22));
        QVERIFY(!alice.inOwnChannel);
        QCOMPARE(ts3::uidPresence(fakets3::kConnection, QStringLiteral("q0Xn6Nobody0000000000000000=")).client, anyID(0));
        QCOMPARE(ts3::uidPresence(fakets3::kConnection, QString()).client, anyID(0));
        fakets3::moveClient(21, 9);
        bob = ts3::uidPresence(fakets3::kConnection, kBob);
        QVERIFY(bob.client == 20 || bob.client == 21);
        QVERIFY(!bob.inOwnChannel);
        ts3::funcs = savedFuncs;
    }

    void senderBudget()
    {
        using linkpreview::autoload::SenderBudget;
        SenderBudget  budget; // 6 at once, then one per 10 s
        const QString alice = QStringLiteral("s/alice");
        const QString bob   = QStringLiteral("s/bob");
        qint64        now   = 1000000;
        QVERIFY(budget.has(alice, now));
        QCOMPARE(budget.size(), 0); // has() takes nothing, keeps nothing
        for (int i = 0; i < 6; ++i)
            QVERIFY(budget.take(alice, now));
        QVERIFY(!budget.has(alice, now));
        QVERIFY(!budget.take(alice, now));
        QVERIFY(budget.take(bob, now)); // one person's flood leaves the others theirs
        now += 9999;
        QVERIFY(!budget.take(alice, now));
        now += 1;
        QVERIFY(budget.has(alice, now));
        QVERIFY(budget.has(alice, now)); // asking twice takes nothing
        QVERIFY(budget.take(alice, now));
        QVERIFY(!budget.take(alice, now));
        now += 25000; // two and a half tokens back
        QVERIFY(budget.take(alice, now));
        QVERIFY(budget.take(alice, now));
        QVERIFY(!budget.take(alice, now));
        now += 3600 * 1000; // never more than the burst
        for (int i = 0; i < 6; ++i)
            QVERIFY(budget.take(alice, now));
        QVERIFY(!budget.take(alice, now));
        // The clock going back: full again rather than stuck.
        QVERIFY(budget.take(alice, now - 5000));

        // Bounded: 256 senders; full buckets are forgotten first, else a new sender gets nothing.
        SenderBudget::Limits limits;
        limits.maxSenders = 4;
        SenderBudget small(limits);
        now = 5000000;
        for (int i = 0; i < 4; ++i)
            QVERIFY(small.take(QStringLiteral("s/%1").arg(i), now));
        QCOMPARE(small.size(), 4);
        QVERIFY(!small.has(QStringLiteral("s/new"), now));
        QVERIFY(!small.take(QStringLiteral("s/new"), now));
        QCOMPARE(small.size(), 4);
        now += 10000; // every bucket full again
        QVERIFY(small.has(QStringLiteral("s/new"), now));
        QVERIFY(small.take(QStringLiteral("s/new"), now));
        QVERIFY(small.size() <= 4);
        SenderBudget defaults;
        for (int i = 0; i < 256; ++i)
            QVERIFY(defaults.take(QStringLiteral("s/%1").arg(i), now));
        QCOMPARE(defaults.size(), 256);
        QVERIFY(!defaults.take(QStringLiteral("s/257"), now));
        defaults.clear();
        QCOMPARE(defaults.size(), 0);
        QVERIFY(defaults.take(QStringLiteral("s/257"), now));
        // No burst at all: nothing ever.
        limits.burst = 0;
        SenderBudget never(limits);
        QVERIFY(!never.has(alice, now));
        QVERIFY(!never.take(alice, now));
    }

    // ---- settings -------------------------------------------------------------------------------------------------
    void linkSettingsOnByDefault()
    {
        const Settings d;
        QVERIFY(d.showLinkPreviews);
        QVERIFY(d.autoLoadLinkPreviews);
        QVERIFY(d.makeLinkPreviews);
        QTemporaryDir dir;
        const QString file = dir.path() + QStringLiteral("/settings.ini");
        Settings      s;
        s.showLinkPreviews     = false;
        s.autoLoadLinkPreviews = false;
        s.makeLinkPreviews     = false;
        s.save(file);
        Settings back;
        back.load(file);
        QVERIFY(!back.showLinkPreviews);
        QVERIFY(!back.autoLoadLinkPreviews);
        QVERIFY(!back.makeLinkPreviews);
        // Each one on its own (the ini key is its own).
        s.autoLoadLinkPreviews = true;
        s.save(file);
        back.load(file);
        QVERIFY(back.autoLoadLinkPreviews);
        QVERIFY(!back.showLinkPreviews);
        QVERIFY(!back.makeLinkPreviews);
        // A 2.4.0 test install's ini has no key yet: on.
        const QString older = dir.path() + QStringLiteral("/older.ini");
        {
            QSettings ini(older, QSettings::IniFormat);
            ini.setValue(QStringLiteral("showLinkPreviews"), true);
            ini.setValue(QStringLiteral("makeLinkPreviews"), true);
        }
        Settings fromOlder;
        fromOlder.autoLoadLinkPreviews = false;
        fromOlder.load(older);
        QVERIFY(fromOlder.autoLoadLinkPreviews);
        // The Privacy & updates tab: three boxes in this order (decision A: loading automatically under showing),
        // each with a hint that says who contacts the website.
        PrivacySection section;
        section.load(d);
        QList<QCheckBox*> boxes;
        for (QCheckBox* box : section.findChildren<QCheckBox*>()) {
            if (box->text().contains(QStringLiteral("other websites")) || box->text().contains(QStringLiteral("automatically"))
                || box->text().contains(QStringLiteral("links I send")))
                boxes << box;
        }
        QCOMPARE(boxes.size(), 3);
        section.resize(600, 800);
        section.show();
        QTest::qWaitForWindowExposed(&section);
        std::sort(boxes.begin(), boxes.end(), [](QCheckBox* a, QCheckBox* b) { return a->mapTo(a->window(), QPoint()).y() < b->mapTo(b->window(), QPoint()).y(); });
        QVERIFY(boxes.at(0)->text().contains(QStringLiteral("other websites")));
        QCOMPARE(boxes.at(1)->text(), QStringLiteral("&Load link previews automatically"));
        QVERIFY(boxes.at(2)->text().contains(QStringLiteral("links I send")));
        // Loading automatically and making previews of your links need showing them (Settings: the sender needs
        // both): greyed out while the first is off, their ticks kept.
        for (int i : {1, 2})
            QVERIFY(boxes.at(i)->isEnabled());
        boxes.at(0)->setChecked(false);
        for (int i : {1, 2}) {
            QVERIFY(!boxes.at(i)->isEnabled());
            QVERIFY(boxes.at(i)->isChecked());
        }
        boxes.at(0)->setChecked(true);
        for (int i : {1, 2})
            QVERIFY(boxes.at(i)->isEnabled());
        Settings off = d;
        off.showLinkPreviews = false;
        section.load(off);
        for (int i : {1, 2})
            QVERIFY(!boxes.at(i)->isEnabled());
        section.load(d);
        for (int i : {1, 2})
            QVERIFY(boxes.at(i)->isEnabled());
        for (QCheckBox* box : qAsConst(boxes)) {
            QVERIFY(box->isChecked());
            QVERIFY(!box->accessibleDescription().isEmpty());
        }
        QVERIFY(boxes.at(0)->accessibleDescription().contains(QStringLiteral("connects your PC")));
        QVERIFY(boxes.at(1)->accessibleDescription().contains(QStringLiteral("IP address")));
        QVERIFY(boxes.at(1)->accessibleDescription().contains(QStringLiteral("Load preview")));
        // store() writes each box to its own field.
        boxes.at(1)->setChecked(false);
        Settings stored = d;
        section.store(stored, d);
        QVERIFY(stored.showLinkPreviews);
        QVERIFY(!stored.autoLoadLinkPreviews);
        QVERIFY(stored.makeLinkPreviews);
        for (QCheckBox* box : qAsConst(boxes))
            box->setChecked(false);
        stored = d;
        section.store(stored, d);
        QVERIFY(!stored.showLinkPreviews);
        QVERIFY(!stored.autoLoadLinkPreviews);
        QVERIFY(!stored.makeLinkPreviews);
        // load() puts each one back.
        Settings mixed = d;
        mixed.autoLoadLinkPreviews = false;
        section.load(mixed);
        QVERIFY(boxes.at(0)->isChecked());
        QVERIFY(!boxes.at(1)->isChecked());
        QVERIFY(boxes.at(2)->isChecked());
    }
};

TSMEDIA_REGISTER_TEST(TestLinkUi)
#include "tst_linkui.moc"
