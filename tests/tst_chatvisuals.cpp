// 2.3 chat-visuals: message bubbles (Cozy), centred system rows on pills, and where avatars come from, on
// TeamSpeak-like chat documents (tests/tschat.h) and a fake TeamSpeak (tests/fakets3.h). Made-up people only.

#include <QAbstractButton>
#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QThread>
#include <QUrl>
#include <QtTest>

#include "chatavatars.h"
#include "chatlayoutsection.h"
#include "emojisection.h"
#include "fakets3.h"
#include "layoutart.h"
#include "layoutbubbles.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "replies.h"
#include "replydoc.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"
#include "tschat.h"
#include "uiutil.h"

namespace {

using layoutart::Mode;

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

layoutdoc::Env envFor(Mode mode, bool bubbles, bool dark = true, int width = 512)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false, bubbles);
    env.colors       = layoutart::colorsFor(dark, dark ? QColor(0x2f, 0x31, 0x36) : QColor(Qt::white), dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black),
                                            dark ? QColor(0x1c, 0xb0, 0xf4) : QColor(0x1c, 0x82, 0xcc));
    env.dpr          = 1.0;
    env.contentWidth = width;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("v");
    env.mentions     = true;
    env.ownNick      = QStringLiteral("Sam");
    env.ownUid       = kMe;
    return env;
}

// A chat: a status line, your three messages, Alice mentioning you, a TS Media line, Bob with a picture, a kick.
void fillChat(QTextDocument& doc)
{
    setup(doc);
    const tschat::Skin s   = tschat::darkSkin();
    const auto         msg = [&](const char* time, int clid, const QString& uid, const QString& nick, const QString& html, bool out = false) {
        tschat::append(&doc, tschat::messageHtml(s, QString::fromLatin1(time), clid, uid, nick, html, out));
    };
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"))));
    msg("16:00:17", 7, kMe, QStringLiteral("Sam"), tschat::linkHtml(s, QStringLiteral("https://example.com/live/first-stream")), true);
    msg("16:00:57", 7, kMe, QStringLiteral("Sam"), QStringLiteral("this one is live now, come watch"), true);
    msg("16:01:30", 7, kMe, QStringLiteral("Sam"), QStringLiteral("ok"), true);
    msg("17:45:37", 17, kAlice, QStringLiteral("Alice"), QStringLiteral("@Sam are you coming tonight? The usual server, around nine."));
    msg("17:46:02", 17, kAlice, QStringLiteral("Alice"), QStringLiteral("line one<br>line two<br>line three"));
    tschat::append(&doc, tschat::printHtml(s, QStringLiteral("Created the server group \"tsmediachat\". To let someone send files, right-click them and choose Server Groups > tsmediachat.")));
    msg("18:07:10", 18, kBob, QStringLiteral("Bob"), tschat::linkHtml(s, QStringLiteral("ts3file://127.0.0.1?filename=lake.jpg"), QStringLiteral("lake.jpg")));
    QTextCursor c(&doc);
    c.movePosition(QTextCursor::End);
    c.insertText(QString(QChar::LineSeparator));
    QTextImageFormat preview;
    preview.setName(QStringLiteral("tsmedia:preview"));
    preview.setWidth(240);
    preview.setHeight(150);
    c.insertImage(preview);
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("18:25:00"), tschat::clientHtml(s, 18, kBob, QStringLiteral("Bob")) + QStringLiteral(" was kicked from the server (spam)")));
}

int blockWith(QTextDocument& doc, const QString& text)
{
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (b.text().contains(text))
            return b.blockNumber();
    }
    return -1;
}

// Every character's and block's format (an object index as what that object is): what the document is.
QVariantMap propsOf(const QTextFormat& f, QTextDocument* doc)
{
    QVariantMap out;
    const QMap<int, QVariant> props = f.properties();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (it.key() == QTextFormat::ObjectIndex) {
            QTextObject* o = doc->object(it.value().toInt());
            out.insert(QStringLiteral("object"), o ? QVariant(propsOf(o->format(), doc)) : QVariant(QStringLiteral("none")));
            continue;
        }
        out.insert(QString::number(it.key()), it.value());
    }
    return out;
}

QList<QVariant> snapshot(QTextDocument& doc)
{
    QList<QVariant> out;
    out << doc.toPlainText();
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        out << propsOf(b.blockFormat(), &doc) << b.isVisible();
        for (auto it = b.begin(); !it.atEnd(); ++it)
            out << it.fragment().length() << propsOf(it.fragment().charFormat(), &doc);
    }
    return out;
}

layoutbubbles::Item itemAt(QTextDocument& doc, int n, const layoutdoc::Env& env)
{
    return layoutbubbles::itemOf(doc.findBlockByNumber(n), env);
}

// A fake ts3Functions.getAvatar: client 7 has a file, client 9 one not downloaded yet, client 5 none at all
// (the SDK's ERROR_database_empty_result), anything else an error.
QByteArray g_avatarFile;
unsigned int fakeGetAvatar(uint64, anyID clientId, char* result, size_t maxLen)
{
    if (clientId == 9) {
        if (maxLen)
            result[0] = '\0';
        return ERROR_ok;
    }
    if (clientId == 5)
        return ERROR_database_empty_result;
    if (clientId != 7)
        return ERROR_client_invalid_id;
    qstrncpy(result, g_avatarFile.constData(), static_cast<uint>(maxLen));
    return ERROR_ok;
}

bool writePicture(const QString& path, const QColor& color, const QSize& size = QSize(64, 40), const char* format = "PNG")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage img(size, QImage::Format_ARGB32);
    img.fill(color);
    // No extension, like TeamSpeak's cache files: written through a QFile.
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return img.save(&f, format);
}

} // namespace

class TestChatVisuals : public QObject
{
    Q_OBJECT

  private slots:
    // ---- bubbles -------------------------------------------------------------------------------------------
    void tokensOnlyInCozy()
    {
        const layoutart::Tokens plain   = layoutart::tokensFor(Mode::Cozy, chatFont(), false);
        const layoutart::Tokens bubbles = layoutart::tokensFor(Mode::Cozy, chatFont(), false, true);
        const layoutart::Tokens compact = layoutart::tokensFor(Mode::Compact, chatFont(), false, true);
        QVERIFY(!plain.bubbles);
        QVERIFY(bubbles.bubbles);
        QVERIFY(!compact.bubbles); // Compact never has bubbles
        QCOMPARE(plain.G, 48);     // 2.2.1's column without them
        QVERIFY(bubbles.G > plain.G);
        // The avatar keeps its air from the bubble.
        QVERIFY2(bubbles.G - bubbles.bubblePadX - (bubbles.avatarX + bubbles.avatar) >= 8, qPrintable(QString::number(bubbles.G)));
        // Off: nothing is shaped (2.2.1's look).
        QTextDocument doc;
        fillChat(doc);
        const layoutdoc::Env env = envFor(Mode::Cozy, false);
        layoutdoc::styleAll(&doc, env);
        doc.documentLayout()->documentSize();
        QVERIFY(layoutbubbles::itemsIn(&doc, 0, doc.blockCount() - 1, env).isEmpty());
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (b.blockFormat().intProperty(layoutformat::kKind) == layoutformat::RowMessage)
                QCOMPARE(b.blockFormat().rightMargin(), 0.0);
        }
    }

    void bubbleGeometry()
    {
        QTextDocument doc;
        fillChat(doc);
        const layoutdoc::Env     env = envFor(Mode::Cozy, true);
        const layoutart::Tokens& t   = env.tokens;
        layoutdoc::styleAll(&doc, env);
        doc.documentLayout()->documentSize();
        const qreal margin = doc.documentMargin();
        const qreal right  = doc.textWidth() - margin; // the chat's right edge (document coordinates)
        QVector<layoutbubbles::Item> bubbles;
        for (const layoutbubbles::Item& item : layoutbubbles::itemsIn(&doc, 0, doc.blockCount() - 1, env)) {
            if (item.kind == layoutbubbles::Kind::Bubble)
                bubbles.append(item);
        }
        QCOMPARE(bubbles.size(), 6); // every message
        for (const layoutbubbles::Item& item : qAsConst(bubbles)) {
            const QTextBlock b = doc.findBlockByNumber(item.block);
            QCOMPARE(b.blockFormat().intProperty(layoutformat::kKind), static_cast<int>(layoutformat::RowMessage));
            // Left of the text column by its padding, never past the chat's right edge, at least its minimum.
            QCOMPARE(item.rect.left(), std::floor(margin + t.G - t.bubblePadX));
            QVERIFY2(item.rect.right() <= right + 0.5, qPrintable(QStringLiteral("%1: %2 > %3").arg(item.block).arg(item.rect.right()).arg(right)));
            QVERIFY(item.rect.width() >= t.bubbleMinWidth);
            // It holds the block's text: every line inside, padded.
            const QRectF br = doc.documentLayout()->blockBoundingRect(b);
            QVERIFY2(item.rect.top() <= br.top() + 0.5 && item.rect.bottom() >= br.bottom() + t.bubblePadBottom - 1, qPrintable(QString::number(item.block)));
            for (int i = 1; i < b.layout()->lineCount(); ++i) {
                const QRectF line = b.layout()->lineAt(i).naturalTextRect().translated(br.topLeft());
                QVERIFY2(line.right() <= item.rect.right() - t.bubblePadX + 1 && line.left() >= item.rect.left() + t.bubblePadX - 1,
                         qPrintable(QStringLiteral("block %1 line %2").arg(item.block).arg(i)));
            }
        }
        // Your messages are yours, the others not; a group's bubbles meet with small corners.
        const int first = blockWith(doc, QStringLiteral("first-stream"));
        const int ok    = blockWith(doc, QStringLiteral("ok"));
        QVERIFY(itemAt(doc, first, env).own);
        QVERIFY(itemAt(doc, first, env).joinBelow);
        QVERIFY(!itemAt(doc, first, env).joinAbove);
        QVERIFY(itemAt(doc, first + 1, env).joinAbove && itemAt(doc, first + 1, env).joinBelow);
        QVERIFY(itemAt(doc, ok, env).joinAbove && !itemAt(doc, ok, env).joinBelow);
        QVERIFY(!itemAt(doc, blockWith(doc, QStringLiteral("line two")), env).own);
        QVERIFY(itemAt(doc, blockWith(doc, QStringLiteral("are you coming")), env).mention);
        // Bubbles never overlap: a group's by the gap at least, groups by more.
        for (int i = 1; i < bubbles.size(); ++i) {
            const qreal gap = bubbles.at(i).rect.top() - bubbles.at(i - 1).rect.bottom();
            // MinimumHeight puts a line's extra pixels above its text: the bubble hugs the text, the gap grows.
            const QTextLine line  = doc.findBlockByNumber(bubbles.at(i).block).layout()->lineAt(0);
            const qreal     extra = qMax(0.0, std::ceil(t.lineMin - line.height()));
            if (bubbles.at(i).joinAbove)
                QVERIFY2(gap >= t.bubbleGap - 1 && gap <= t.bubbleGap + extra + 1, qPrintable(QStringLiteral("%1: %2 (line y %3 height %4, lineMin %5)")
                                                                                         .arg(bubbles.at(i).block)
                                                                                         .arg(gap)
                                                                                         .arg(doc.findBlockByNumber(bubbles.at(i).block).layout()->lineAt(0).y())
                                                                                         .arg(doc.findBlockByNumber(bubbles.at(i).block).layout()->lineAt(0).height())
                                                                                         .arg(t.lineMin)));
            else
                QVERIFY2(gap >= 8, qPrintable(QStringLiteral("%1: %2").arg(bubbles.at(i).block).arg(gap)));
        }
        // A head's bubble starts at its name row: the avatar beside it, outside it.
        const layoutbubbles::Item head = itemAt(doc, first, env);
        QVERIFY(margin + t.avatarX + t.avatar <= head.rect.left() - 8);
        // The picture is inside the bubble.
        const int  bob     = blockWith(doc, QStringLiteral("lake.jpg"));
        const auto picture = itemAt(doc, bob, env);
        QVERIFY(picture.rect.height() > 150 + 24);
        QVERIFY(picture.rect.width() >= 240 + 2 * t.bubblePadX - 1);
        // The outline: rounded, its corners small where the group meets.
        const QPainterPath path = layoutbubbles::pathOf(itemAt(doc, first + 1, env), t);
        QVERIFY(path.contains(path.boundingRect().center()));
        QVERIFY(!path.contains(path.boundingRect().topRight() + QPointF(-1, 1)));
    }

    // 2.3 live test: in Cozy with bubbles, hovering the last message near the chat's bottom pushed the action
    // bar up (it stays inside the chat) over the text of the bubble above. Now it never covers a message's text
    // where that can be helped: up or down along the bubble, further right, into the bubble's top right corner;
    // the usual places stay as they were.
    void actionBarNeverOverText()
    {
        const QSize bar(118, 34); // Cozy: four 28 px buttons (actionbar.cpp)
        const QRect area(0, 0, 600, 400);
        const QRect inside = area.adjusted(2, 2, -2, -2);
        const int   right  = 12;
        const auto  cover  = [](const QRect& r, const QVector<QRect>& keep) {
            qint64 covered = 0;
            for (const QRect& k : keep) {
                const QRect i = r & k;
                if (!i.isEmpty())
                    covered += qint64(i.width()) * i.height();
            }
            return covered;
        };
        // The usual places (nothing in the way): beside a one-line bubble, centred on it; on a wide one's top edge.
        {
            const QRect bubble(50, 100, 200, 25);
            const QVector<QRect> keep{QRect(60, 104, 180, 16), QRect(60, 60, 300, 16), QRect(60, 140, 100, 16)};
            QCOMPARE(layoutbubbles::barPlace(bar, area, bubble, keep, right), QRect(bubble.right() + 6, 100 - 4, 118, 34));
            QCOMPARE(layoutbubbles::barPlace(bar, area, QRect(50, 200, 530, 60), {QRect(60, 200, 150, 24)}, right), QRect(600 - 12 - 118, 200 - 17, 118, 34));
        }
        // The live test's case: the last message, one line, at the chat's bottom, the one above it wider.
        {
            const QRect          bubble(50, 376, 130, 24); // to 399: the chat's bottom
            const QRect          above(60, 352, 180, 17);  // the text above, to 368
            const QVector<QRect> keep{above, QRect(60, 380, 110, 16)};
            const QRect          before(bubble.right() + 6, qBound(2, bubble.top() - 5, 400 - 34 - 2), 118, 34); // 2.3: pushed up into the chat
            QVERIFY(before.intersects(above));
            const QRect r = layoutbubbles::barPlace(bar, area, bubble, keep, right);
            QCOMPARE(cover(r, keep), qint64(0));
            QVERIFY(inside.contains(r));
            QVERIFY(r.bottom() >= bubble.top() - 4 && r.top() <= bubble.bottom() + 4); // still the bubble's
            QCOMPARE(r.left(), above.right() + 1 + 6);                                 // right of the text above
            // (With the bubbles' padding: clear of the bubble above too.)
            QCOMPARE(layoutbubbles::barPlace(bar, area, bubble, keep, right, 10).left(), above.right() + 1 + 10 + 6);
            // The text above as wide as the chat: no room anywhere. What covers the least, never more than before.
            const QVector<QRect> wide{QRect(60, 352, 520, 17), QRect(60, 380, 110, 16)};
            const QRect          least = layoutbubbles::barPlace(bar, area, bubble, wide, right);
            QVERIFY(inside.contains(least));
            QVERIFY(cover(least, wide) <= cover(before, wide));
        }
        // A wide head bubble under a long line: into its top right corner, beside its name and time.
        {
            const QRect          bubble(50, 200, 530, 60);
            const QVector<QRect> keep{QRect(60, 170, 510, 17), QRect(60, 200, 150, 24), QRect(60, 228, 500, 17)};
            QCOMPARE(layoutbubbles::barPlace(bar, area, bubble, keep, right), QRect(600 - 12 - 118, 187, 118, 34));
        }
        // The first message at the chat's top: pushed down, never over the one below.
        {
            const QRect          bubble(50, 0, 130, 24);
            const QVector<QRect> keep{QRect(60, 4, 110, 16), QRect(60, 30, 200, 17)};
            const QRect          r = layoutbubbles::barPlace(bar, area, bubble, keep, right);
            QCOMPARE(cover(r, keep), qint64(0));
            QVERIFY(inside.contains(r));
        }

        // A chat built like TeamSpeak's, the live test's messages (a line that wraps, two people): every message
        // hovered with the chat at its end, at its start and with the message in the middle.
        for (const int width : {1200, 700, 420}) {
            QTextDocument doc;
            setup(doc, width);
            const tschat::Skin s   = tschat::darkSkin();
            const auto         msg = [&](const char* time, int clid, const QString& uid, const QString& nick, const QString& html) {
                tschat::append(&doc, tschat::messageHtml(s, QString::fromLatin1(time), clid, uid, nick, html));
            };
            msg("12:42:01", 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Good morning, this is the first line of the bubble test"));
            msg("12:42:05", 17, kAlice, QStringLiteral("Alice"), QStringLiteral("A second line right after it"));
            msg("12:42:09", 17, kAlice, QStringLiteral("Alice"),
                QStringLiteral("A third, longer line so that the bubble has to wrap onto a second line inside the chat column when the chat is narrow enough"));
            msg("12:42:20", 18, kBob, QStringLiteral("Bob"), QStringLiteral("Hi from Bob, replying in the bubble test"));
            msg("12:42:31", 18, kBob, QStringLiteral("Bob"), QStringLiteral("Bob again with a short one"));
            const layoutdoc::Env env = envFor(Mode::Cozy, true, true, width - 8);
            layoutdoc::styleAll(&doc, env);
            const int docHeight = static_cast<int>(std::ceil(doc.documentLayout()->documentSize().height()));
            const int height    = 150; // a short chat: it scrolls
            QVERIFY(docHeight > height);
            const QRect view(0, 0, width, height);
            // The head's name row only as wide as the name and time: the rest of its picture is free.
            const int                  head = blockWith(doc, QStringLiteral("Good morning"));
            const layoutbubbles::Item  item = itemAt(doc, head, env);
            const QVector<QRectF>      keep = layoutbubbles::keepOut(&doc, head, head, env);
            QVERIFY(keep.size() >= 2);
            QVERIFY2(keep.first().width() < item.rect.width() - 2 * env.tokens.bubblePadX, qPrintable(QString::number(keep.first().width())));
            int checked = 0, freeOnes = 0, fixed = 0;
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                const layoutbubbles::Item it = itemAt(doc, b.blockNumber(), env);
                if (it.kind != layoutbubbles::Kind::Bubble)
                    continue;
                for (const int wanted : {docHeight - height, 0, qRound(it.rect.center().y()) - height / 2, qRound(it.rect.bottom()) - height + 2}) {
                    const int   sy     = qBound(0, wanted, docHeight - height);
                    const QRect bubble = it.rect.translated(0, -sy).toAlignedRect();
                    if (!view.intersects(bubble))
                        continue;
                    QVector<QRect> rects;
                    for (const QRectF& r : layoutbubbles::keepOutNear(&doc, b.blockNumber(), bar.height() + 8, env))
                        rects << r.translated(0, -sy).toAlignedRect();
                    const QRect r = layoutbubbles::barPlace(bar, view, bubble, rects, right, env.tokens.bubblePadX);
                    QString what = QStringLiteral("width %1, block %2 (%3), scrolled %4: %5,%6 for bubble %7,%8 %9x%10; keep")
                                       .arg(width)
                                       .arg(b.blockNumber())
                                       .arg(b.text().right(20))
                                       .arg(sy)
                                       .arg(r.x())
                                       .arg(r.y())
                                       .arg(bubble.x())
                                       .arg(bubble.y())
                                       .arg(bubble.width())
                                       .arg(bubble.height());
                    for (const QRect& k : qAsConst(rects))
                        what += QStringLiteral(" %1,%2 %3x%4").arg(k.x()).arg(k.y()).arg(k.width()).arg(k.height());
                    QVERIFY2(view.adjusted(2, 2, -2, -2).contains(r), qPrintable(what));
                    QVERIFY2(r.bottom() >= bubble.top() - 4 && r.top() <= bubble.bottom() + 4, qPrintable(what));
                    // Every place the bar could take (its own: by the bubble's right side or top right corner):
                    // when one of them is free, the bar is on a free one; else it covers no more than 2.3 did.
                    const int minX = 2, maxX = width - 2 - bar.width(), lo = qMax(2, bubble.top() - 4 - bar.height() + 1), hi = qMin(height - 2 - bar.height(), bubble.bottom() + 4);
                    const int edgeX  = width - right - bar.width();
                    const bool beside = bubble.right() + 6 <= edgeX;
                    const QRect before(qBound(minX, beside ? bubble.right() + 6 : edgeX, maxX),
                                       qBound(2, beside ? bubble.top() - qMax(0, (bar.height() - bubble.height()) / 2) : bubble.top() - bar.height() / 2, height - 2 - bar.height()),
                                       bar.width(), bar.height());
                    bool free = false;
                    for (int x = qBound(minX, bubble.right() + 1 - 6 - bar.width(), maxX); x <= maxX && !free; ++x) {
                        for (int y = lo; y <= hi && !free; ++y)
                            free = cover(QRect(x, y, bar.width(), bar.height()), rects) == 0;
                    }
                    if (free) {
                        QVERIFY2(cover(r, rects) == 0, qPrintable(what));
                        ++freeOnes;
                    } else {
                        QVERIFY2(cover(r, rects) <= cover(before, rects), qPrintable(what));
                    }
                    if (cover(before, rects) > 0 && free)
                        ++fixed; // 2.3 covered a message's text there
                    ++checked;
                }
            }
            // (How many places are free depends on the fonts: -platform offscreen without a font folder draws
            // wide boxes. Both ways are checked above.)
            QVERIFY(checked >= 15);
            QVERIFY2(freeOnes >= 1, qPrintable(QStringLiteral("%1 of %2").arg(freeOnes).arg(checked)));
            qDebug("width %d: %d hovers, a free place for %d, 2.3 covered text in %d of those", width, checked, freeOnes, fixed);
        }
    }

    void nothingSticksOut()
    {
        // With bubbles a message's lines are bubbleRight narrower: a head (a long name and time, a long reply
        // row) still fits its line, so the chat never gets wider than the window (TeamSpeak's chat would get a
        // horizontal scroll bar) and no picture reaches past the bubble.
        for (const bool bubbles : {false, true}) {
            for (const int width : {260, 320, 520}) {
                QTextDocument doc;
                setup(doc, width);
                const tschat::Skin s = tschat::darkSkin();
                tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:02"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Hello world")));
                replies::Original alice;
                alice.nick     = QStringLiteral("Alice");
                alice.uid      = kAlice;
                alice.clientId = 17;
                alice.minutes  = 21 * 60 + 14;
                alice.text     = QStringLiteral("Hello world");
                QString quote  = replies::quoteLine(alice);
                quote.replace(QStringLiteral("[i]"), QStringLiteral("<i>")).replace(QStringLiteral("[/i]"), QStringLiteral("</i>"));
                quote.replace(QRegularExpression(QStringLiteral("\\[URL=([^\\]]+)\\]")), QStringLiteral("<a href=\"\\1\">")).replace(QStringLiteral("[/URL]"), QStringLiteral("</a>"));
                tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:15:30"), 18, kBob, QStringLiteral("Bob"), quote + QStringLiteral("<br>my reply")));
                tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:16:00"), 19, kMe, QStringLiteral("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"), QStringLiteral("gg"), true));
                for (const replydoc::Message& m : replydoc::scan(&doc)) {
                    if (m.hasQuote)
                        QVERIFY(replydoc::collapse(&doc, m, replydoc::objectPrefix() + QStringLiteral("t.%1").arg(m.block), QSizeF(200, 14)));
                }
                layoutdoc::Env env = envFor(Mode::Cozy, bubbles, true, width - static_cast<int>(2 * doc.documentMargin()));
                env.replyHeader    = [](int, replyart::Header* h) {
                    h->nick    = QStringLiteral("Alice");
                    h->uid     = kAlice;
                    h->snippet = QStringLiteral("Hello world, and a lot more words than any chat this narrow has room for in one row");
                    h->found   = true;
                    return true;
                };
                QCOMPARE(layoutdoc::styleAll(&doc, env), 3);
                doc.documentLayout()->documentSize();
                const QString what = QStringLiteral("bubbles %1 width %2").arg(bubbles).arg(width);
                for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                    for (int i = 0; i < b.layout()->lineCount(); ++i) {
                        const QTextLine line = b.layout()->lineAt(i);
                        QVERIFY2(line.naturalTextWidth() <= line.width() + 0.5, qPrintable(QStringLiteral("%1: block %2 line %3: %4 > %5")
                                                                                                 .arg(what)
                                                                                                 .arg(b.blockNumber())
                                                                                                 .arg(i)
                                                                                                 .arg(line.naturalTextWidth())
                                                                                                 .arg(line.width())));
                    }
                }
                QVERIFY2(doc.documentLayout()->documentSize().width() <= doc.textWidth() + 0.5, qPrintable(what));
                if (bubbles) {
                    // The bubbles keep the chat's right edge (their padding inside the window).
                    for (const layoutbubbles::Item& item : layoutbubbles::itemsIn(&doc, 0, doc.blockCount() - 1, env))
                        QVERIFY2(item.rect.right() <= doc.textWidth() - doc.documentMargin() + 0.5, qPrintable(what));
                }
            }
        }
    }

    void gutterTimeBesideTheBubble()
    {
        // A hovered continuation's time sits in the gutter: never on its bubble, at any font size.
        for (const int px : {11, 12, 16, 24}) {
            QFont f = chatFont();
            f.setPixelSize(px);
            for (const bool ampm : {false, true}) {
                const layoutart::Tokens t = layoutart::tokensFor(Mode::Cozy, f, ampm, true);
                const qreal right = layoutart::gutterTimeRight(t);
                QVERIFY2(right <= t.G - t.bubblePadX - 3, qPrintable(QStringLiteral("%1 px: %2 vs bubble %3").arg(px).arg(right).arg(t.G - t.bubblePadX)));
                // In the chat's gutter (its document margin, 4 px, on the left): AM/PM left out if need be.
                const QFontMetricsF fm(t.time);
                const qreal         room = 4 + right - 1;
                const QString       full = ampm ? QStringLiteral("12:59 PM") : QStringLiteral("00:00");
                const QString       text = layoutart::gutterTimeText(full, t, room);
                QVERIFY(text.startsWith(full.section(QLatin1Char(' '), 0, 0)));
                // (-platform offscreen without a font folder draws boxes: widths only with real fonts)
                if (!QFontDatabase().families().isEmpty()) {
                    QVERIFY2(fm.horizontalAdvance(text) <= room,
                             qPrintable(QStringLiteral("%1 px: %2 is %3 px, room %4").arg(px).arg(text).arg(fm.horizontalAdvance(text)).arg(room)));
                    if (!ampm)
                        QCOMPARE(text, full); // a 24-hour time always fits
                }
                QCOMPARE(layoutart::gutterTimeText(full, t, 1000), full);
                QCOMPARE(layoutart::gutterTimeText(QStringLiteral("12:59 PM"), t, 1), QStringLiteral("12:59"));
                // Without bubbles: 2.2.1's place.
                QCOMPARE(layoutart::gutterTimeRight(layoutart::tokensFor(Mode::Cozy, f, ampm, false)), qreal(layoutart::tokensFor(Mode::Cozy, f, ampm, false).G - 8));
            }
        }
    }

    void reactionRowBelowTheBubble()
    {
        // A picture marked kBelowBubble at a message's end (a row of reactions) stays under its bubble; any other
        // picture there is inside.
        for (const bool below : {false, true}) {
            QTextDocument doc;
            setup(doc);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("12:00:00"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("hello there")));
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("12:00:20"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("and more")));
            const layoutdoc::Env env = envFor(Mode::Cozy, true);
            layoutdoc::styleAll(&doc, env);
            doc.documentLayout()->documentSize();
            const qreal before = itemAt(doc, 0, env).rect.bottom();
            QVERIFY(itemAt(doc, 0, env).joinBelow && itemAt(doc, 1, env).joinAbove);
            QTextCursor c(doc.firstBlock());
            c.movePosition(QTextCursor::EndOfBlock);
            c.insertText(QString(QChar::LineSeparator));
            QTextImageFormat row;
            row.setName(QStringLiteral("tsmedia:reactions"));
            row.setWidth(120);
            row.setHeight(28);
            if (below)
                row.setProperty(layoutformat::kBelowBubble, true);
            c.insertImage(row);
            doc.documentLayout()->documentSize();
            const layoutbubbles::Item item = itemAt(doc, 0, env);
            // A row below the bubble rounds both bubbles' corners there; inside, they still meet.
            QCOMPARE(item.joinBelow, !below);
            QCOMPARE(itemAt(doc, 1, env).joinAbove, !below);
            if (below)
                QCOMPARE(item.rect.bottom(), before);
            else
                QVERIFY2(item.rect.bottom() >= before + 26, qPrintable(QStringLiteral("kind %1 rect %2,%3 %4x%5 before %6 lines %7")
                                                                         .arg(static_cast<int>(item.kind))
                                                                         .arg(item.rect.x())
                                                                         .arg(item.rect.y())
                                                                         .arg(item.rect.width())
                                                                         .arg(item.rect.height())
                                                                         .arg(before)
                                                                         .arg(doc.firstBlock().layout()->lineCount())));
        }
    }

    void roundTripWithBubbles()
    {
        // Given back exactly: margins, alignment, our properties, the trailer of every row.
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            for (const bool bubbles : {true, false}) {
                QTextDocument plain, styled;
                fillChat(plain);
                fillChat(styled);
                const QList<QVariant> expected = snapshot(plain);
                layoutdoc::styleAll(&styled, envFor(mode, bubbles));
                QVERIFY(snapshot(styled) != expected);
                layoutdoc::restoreAll(&styled);
                QVERIFY2(snapshot(styled) == expected, qPrintable(QStringLiteral("mode %1 bubbles %2").arg(static_cast<int>(mode)).arg(bubbles)));
                for (QTextBlock b = styled.begin(); b.isValid(); b = b.next())
                    QVERIFY(!layoutdoc::hasOurs(b));
            }
        }
        // A line TeamSpeak appends after a centred row takes its format: sanitize gives it TeamSpeak's back.
        QTextDocument doc;
        setup(doc);
        tschat::append(&doc, tschat::systemHtml(tschat::darkSkin(), QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(tschat::darkSkin(), QStringLiteral("Home"))));
        layoutdoc::styleAll(&doc, envFor(Mode::Cozy, true));
        QTextCursor c(&doc);
        c.movePosition(QTextCursor::End);
        c.insertBlock();
        c.insertText(QStringLiteral("typed after"));
        QVERIFY((doc.lastBlock().blockFormat().alignment() & Qt::AlignHorizontal_Mask) == Qt::AlignHCenter); // took ours over
        QVERIFY(layoutdoc::sanitize(&doc, doc.lastBlock().blockNumber()));
        QVERIFY(!layoutdoc::hasOurs(doc.lastBlock()));
        QVERIFY((doc.lastBlock().blockFormat().alignment() & Qt::AlignHCenter) == 0);
        QCOMPARE(doc.lastBlock().blockFormat().rightMargin(), 0.0);
    }

    // ---- centred system rows ---------------------------------------------------------------------------------
    void systemRowsAreCentred()
    {
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            for (const bool bubbles : {true, false}) {
                QTextDocument doc;
                fillChat(doc);
                const layoutdoc::Env env = envFor(mode, bubbles);
                layoutdoc::styleAll(&doc, env);
                doc.documentLayout()->documentSize();
                const qreal center = doc.textWidth() / 2.0;
                int         rows   = 0;
                for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                    const layoutdoc::Row row = layoutdoc::read(b);
                    if (row.type != layoutdoc::RowType::System)
                        continue;
                    ++rows;
                    const QTextBlockFormat bf = b.blockFormat();
                    QVERIFY((bf.alignment() & Qt::AlignHorizontal_Mask) == Qt::AlignHCenter);
                    QCOMPARE(bf.leftMargin(), bf.rightMargin());
                    QCOMPARE(bf.textIndent(), 0.0);
                    QCOMPARE(layoutformat::leadOf(b).kind, layoutformat::SystemPrefix);
                    // Every line's text is centred in the chat (within a pixel).
                    const QRectF br = doc.documentLayout()->blockBoundingRect(b);
                    for (int i = 0; i < b.layout()->lineCount(); ++i) {
                        const QRectF line = b.layout()->lineAt(i).naturalTextRect().translated(br.topLeft());
                        QVERIFY2(qAbs(line.center().x() - center) <= 1.5, qPrintable(QStringLiteral("mode %1 block %2 line %3: %4 vs %5")
                                                                                         .arg(static_cast<int>(mode))
                                                                                         .arg(b.blockNumber())
                                                                                         .arg(i)
                                                                                         .arg(line.center().x())
                                                                                         .arg(center)));
                    }
                    // The time after the text (Compact too), no "*** ": "... Home  15:56" (no-break spaces).
                    if (!row.ownPrint)
                        QVERIFY2(b.text().endsWith(QString(2, QChar::Nbsp) + layoutart::shortTime(row.time, false)), qPrintable(b.text()));
                    // With bubbles a pill round its text; without, none.
                    const layoutbubbles::Item item = layoutbubbles::itemOf(b, env);
                    QCOMPARE(item.kind, bubbles && mode == Mode::Cozy ? layoutbubbles::Kind::Pill : layoutbubbles::Kind::None);
                    if (item.kind == layoutbubbles::Kind::Pill) {
                        QVERIFY2(qAbs(item.rect.center().x() - center) <= 1.5, qPrintable(QStringLiteral("block %1: pill %2..%3, centre %4, br %5..%6, line0 x %7 natural %8..%9")
                                                                                            .arg(b.blockNumber())
                                                                                            .arg(item.rect.left())
                                                                                            .arg(item.rect.right())
                                                                                            .arg(center)
                                                                                            .arg(br.left())
                                                                                            .arg(br.right())
                                                                                            .arg(b.layout()->lineAt(0).x())
                                                                                            .arg(b.layout()->lineAt(0).naturalTextRect().left())
                                                                                            .arg(b.layout()->lineAt(0).naturalTextRect().right())));
                        QVERIFY(item.rect.contains(br.adjusted(0, 0, 0, -1).center()));
                    }
                }
                QCOMPARE(rows, 3); // the channel line, TS Media's line, the kick
            }
        }
        // TS Media's long line wraps into centred lines, one pill round all of them.
        QTextDocument doc;
        fillChat(doc);
        doc.setTextWidth(300);
        layoutdoc::Env env = envFor(Mode::Cozy, true, true, 292);
        env.tokens         = layoutart::tokensFor(Mode::Cozy, chatFont(), true, true); // "6:25 PM"
        layoutdoc::styleAll(&doc, env);
        doc.documentLayout()->documentSize();
        // A wrapped row's time never sits alone on its last line (it keeps the last word with it).
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            const layoutdoc::Row row = layoutdoc::read(b);
            if (row.type != layoutdoc::RowType::System || row.ownPrint || b.layout()->lineCount() < 2)
                continue;
            const QTextLine last = b.layout()->lineAt(b.layout()->lineCount() - 1);
            const QString   text = b.text().mid(last.textStart(), last.textLength());
            QVERIFY2(text.trimmed().size() > layoutart::shortTime(row.time, true).size() + 2, qPrintable(text));
        }
        const QTextBlock print = doc.findBlockByNumber(blockWith(doc, QStringLiteral("tsmediachat")));
        QVERIFY(print.layout()->lineCount() >= 3);
        const layoutbubbles::Item pill = layoutbubbles::itemOf(print, env);
        QCOMPARE(pill.kind, layoutbubbles::Kind::Pill);
        QVERIFY(pill.rect.height() >= print.layout()->lineCount() * 14);
        QVERIFY(pill.rect.right() <= 300);
    }

    void foldedRunsAreCentred()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        for (int i = 0; i < 5; ++i)
            tschat::append(&doc, tschat::statusHtml(s, QStringLiteral("21:00:%1").arg(i, 2, 10, QLatin1Char('0')),
                                                     QStringLiteral("You switched from channel ") + tschat::channelHtml(s, i % 2 ? 4 : 1, i % 2 ? QStringLiteral("Two") : QStringLiteral("One"))
                                                         + QStringLiteral(" to ") + tschat::channelHtml(s, i % 2 ? 1 : 4, i % 2 ? QStringLiteral("One") : QStringLiteral("Two"))));
        layoutdoc::Env env = envFor(Mode::Cozy, true);
        env.collapse       = true;
        layoutdoc::styleAll(&doc, env);
        int serial = 1;
        for (const layoutdoc::RunPlan& run : layoutdoc::planRuns(&doc, 0, doc.blockCount() - 1, {}))
            layoutdoc::applyRun(&doc, run, env, &serial);
        doc.documentLayout()->documentSize();
        const QTextBlock first = doc.firstBlock();
        QVERIFY(!layoutdoc::chipsOf(first).isEmpty()); // "+4 more events"
        const QRectF br   = doc.documentLayout()->blockBoundingRect(first);
        const QRectF line = first.layout()->lineAt(0).naturalTextRect().translated(br.topLeft());
        QVERIFY(qAbs(line.center().x() - doc.textWidth() / 2.0) <= 1.5);
        // Hidden rows have no pill.
        const QVector<layoutbubbles::Item> items = layoutbubbles::itemsIn(&doc, 0, doc.blockCount() - 1, env);
        QCOMPARE(items.size(), 1);
    }

    // ---- colours -----------------------------------------------------------------------------------------------
    void bubbleColours()
    {
        struct Skin {
            const char* name;
            bool        dark;
            QColor      base, text, link;
        };
        const QVector<Skin> skins = {
            {"dark", true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4)},
            {"light", false, QColor(0xff, 0xff, 0xff), QColor(0, 0, 0), QColor(0x1c, 0x82, 0xcc)},
            {"synthetic black", true, QColor(0x10, 0x10, 0x10), QColor(0xc0, 0xc0, 0xc0), QColor(0x30, 0x60, 0xd0)},
            {"synthetic grey", false, QColor(0xe8, 0xe8, 0xe8), QColor(0x30, 0x30, 0x30), QColor(0x50, 0x90, 0xe0)},
            {"synthetic blue", true, QColor(0x1e, 0x2a, 0x3a), QColor(0xd0, 0xd8, 0xe0), QColor(0x60, 0x90, 0xff)},
        };
        for (const Skin& s : skins) {
            const layoutart::Colors c = layoutart::colorsFor(s.dark, s.base, s.text, s.link);
            // A bubble is a step off the chat (visible), yours another step off theirs.
            const QColor bubble = layoutart::bubbleBack(c, false);
            const QColor own    = layoutart::bubbleBack(c, true);
            QVERIFY2(bubble != c.base && own != bubble, s.name);
            QVERIFY2(s.dark ? bubble.lightness() > c.base.lightness() : bubble.lightness() < c.base.lightness(), s.name); // raised on dark, sunk on light
            QVERIFY2(own.blue() > own.red(), s.name); // the accent's tint
            // Everything on them keeps its contrast (contrastPairs has every state).
            int pairs = 0;
            for (const layoutart::Pair& p : layoutart::contrastPairs(c)) {
                if (!p.what.contains(QLatin1String("bubble")) && !p.what.contains(QLatin1String("pill")))
                    continue;
                ++pairs;
                const double ratio = ui::contrastRatio(p.fore, p.back);
                QVERIFY2(ratio >= p.minimum - 0.005, qPrintable(QStringLiteral("%1: %2 %3 on %4: %5").arg(QString::fromLatin1(s.name), p.what, p.fore.name(), p.back.name()).arg(ratio)));
            }
            QVERIFY(pairs >= 40);
        }
        // Links in a message get the bubble's link colour where the skin's doesn't read on it.
        QTextDocument doc;
        setup(doc);
        const tschat::Skin skin = tschat::darkSkin();
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("12:00:00"), 7, kMe, QStringLiteral("Sam"),
                                                 QStringLiteral("<a href=\"https://example.com/\" style=\"color:#3a62a0\">example</a>"), true));
        const layoutdoc::Env env = envFor(Mode::Cozy, true);
        layoutdoc::styleAll(&doc, env);
        for (auto it = doc.firstBlock().begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (f.isAnchor() && f.anchorHref().startsWith(QLatin1String("https")))
                QCOMPARE(f.foreground().color(), env.colors.linkOnBubble);
        }
    }

    // ---- avatars -------------------------------------------------------------------------------------------------
    void avatarFileNames()
    {
        // The uid's 20 bytes in hex, 0-f written a-p.
        const QString uid = QString::fromLatin1(QByteArray::fromHex("00112233445566778899aabbccddeeff01234567").toBase64());
        QCOMPARE(chatavatars::cacheFileName(uid), QStringLiteral("avatar_aabbccddeeffgghhiijjkkllmmnnooppabcdefgh"));
        // Nothing that isn't a uid (no way out of the cache folder).
        QVERIFY(chatavatars::cacheFileName(QStringLiteral("../../x")).isEmpty());
        QVERIFY(chatavatars::cacheFileName(QString()).isEmpty());
        QVERIFY(chatavatars::cacheFileName(QString(200, QLatin1Char('A'))).isEmpty());
        QVERIFY(chatavatars::cacheFileName(QStringLiteral("abc\\def=")).isEmpty());
        const QString path = chatavatars::cachePath(QStringLiteral("C:/cfg/"), QStringLiteral("serverUid+/="), uid);
        QCOMPARE(path, QStringLiteral("C:/cfg/cache/") + QString::fromLatin1(QByteArray("serverUid+/=").toBase64()) + QStringLiteral("/clients/") + chatavatars::cacheFileName(uid));
        QVERIFY(chatavatars::cachePath(QString(), QStringLiteral("s"), uid).isEmpty());
        QVERIFY(chatavatars::cachePath(QStringLiteral("C:/cfg"), QString(), uid).isEmpty());
    }

    void avatarLookupOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString config = dir.path();
        const QString uid    = kAlice;
        const QString name   = chatavatars::cacheFileName(uid);
        QVERIFY(!name.isEmpty());
        const QString here  = chatavatars::cachePath(config, QStringLiteral("serverA"), uid);
        const QString other = chatavatars::cachePath(config, QStringLiteral("serverB"), uid);
        const QString newer = chatavatars::cachePath(config, QStringLiteral("serverC"), uid);
        const QString own   = config + QStringLiteral("/teamspeak/own_avatar");
        QVERIFY(writePicture(other, Qt::red));
        QVERIFY(writePicture(newer, Qt::green, QSize(40, 64), "PNG"));
        {
            QFile older(other);
            QVERIFY(older.open(QIODevice::ReadWrite));
            QVERIFY(older.setFileTime(QDateTime::currentDateTime().addDays(-3), QFileDevice::FileModificationTime));
        }

        chatavatars::Query q;
        q.paths << QString() << here; // nothing on this server yet
        q.configDir = config;
        q.fileName  = name;
        // Another server's copy, the newest first.
        chatavatars::Found found = chatavatars::find(q);
        QVERIFY(!found.image.isNull());
        QCOMPARE(found.file, newer);
        QCOMPARE(found.image.size(), QSize(chatavatars::kPixels, chatavatars::kPixels)); // a centred square
        QVERIFY(chatavatars::unchanged(found));
        // This server's file comes before them.
        QVERIFY(writePicture(here, Qt::blue));
        found = chatavatars::find(q);
        QCOMPARE(found.file, here);
        QCOMPARE(found.image.pixelColor(24, 24), QColor(Qt::blue));
        // TeamSpeak's own file first of all.
        QVERIFY(writePicture(own, Qt::yellow));
        q.paths.prepend(own);
        QCOMPARE(chatavatars::find(q).file, own);
        // A file that isn't a picture, or too big, is passed over.
        {
            QFile f(own);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("not a picture at all");
        }
        QCOMPARE(chatavatars::find(q).file, here);
        {
            QFile f(here);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QByteArray(chatavatars::kMaxBytes + 1, 'x'));
        }
        QCOMPARE(chatavatars::find(q).file, newer);
        QVERIFY(!chatavatars::unchanged(chatavatars::Found())); // a changed file is read again
        // Nothing anywhere: none.
        chatavatars::Query none;
        none.configDir = config;
        none.fileName  = chatavatars::cacheFileName(kBob);
        QVERIFY(chatavatars::find(none).image.isNull());
        // Never outside the cache: a file name that isn't TeamSpeak's.
        none.fileName = QStringLiteral("../x");
        QVERIFY(chatavatars::find(none).image.isNull());

        // The query ChatLayout makes: TeamSpeak's file, the one it told us about, this server's cache, the
        // others; nothing at all (initials) when TeamSpeak says the person has no avatar, though caches have one.
        const QString told = config + QStringLiteral("/teamspeak/told");
        QVERIFY(writePicture(told, Qt::cyan));
        const chatavatars::Query all = chatavatars::queryFor(own, false, told, config, QStringLiteral("serverA"), uid);
        QCOMPARE(all.paths, (QStringList{own, told, here}));
        QCOMPARE(all.configDir, config);
        QCOMPARE(all.fileName, name);
        QCOMPARE(chatavatars::find(all).file, told); // (own isn't a picture any more, here is too big)
        const chatavatars::Query pending = chatavatars::queryFor(QString(), false, QString(), config, QStringLiteral("serverA"), uid);
        QCOMPARE(pending.paths, QStringList{here});
        QCOMPARE(chatavatars::find(pending).file, newer); // not downloaded yet: another server's copy meanwhile
        const chatavatars::Query nothing = chatavatars::queryFor(own, true, told, config, QStringLiteral("serverA"), uid);
        QVERIFY(nothing.isEmpty());
        QVERIFY(chatavatars::find(nothing).image.isNull());
        QVERIFY(chatavatars::queryFor(own, false, told, config, QStringLiteral("serverA"), QStringLiteral("../../x")).isEmpty()); // not a uid
    }

    void avatarFromTeamSpeak()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        QVERIFY(chatavatars::fromTeamSpeak(fakets3::kConnection, 7).isEmpty()); // no getAvatar: nothing
        ts3::funcs.getAvatar = fakeGetAvatar;
        const QString file   = dir.path() + QStringLiteral("/cache/x/clients/avatar_own");
        g_avatarFile         = QDir::toNativeSeparators(file).toUtf8();
        bool none = true;
        QCOMPARE(chatavatars::fromTeamSpeak(fakets3::kConnection, 7, &none), file);
        QVERIFY(!none);
        QVERIFY(chatavatars::fromTeamSpeak(fakets3::kConnection, 9, &none).isEmpty()); // not downloaded yet
        QVERIFY(!none);
        QVERIFY(chatavatars::fromTeamSpeak(fakets3::kConnection, 8, &none).isEmpty()); // an error
        QVERIFY(!none);
        QVERIFY(chatavatars::fromTeamSpeak(fakets3::kConnection, 5, &none).isEmpty()); // no avatar at all
        QVERIFY(none);
        QVERIFY(chatavatars::fromTeamSpeak(fakets3::kConnection, 5).isEmpty());
        QVERIFY(chatavatars::fromTeamSpeak(0, 7).isEmpty());
        g_avatarFile = QByteArray(2000, 'a'); // longer than any path: cut, then refused
        QVERIFY(chatavatars::fromTeamSpeak(fakets3::kConnection, 7).isEmpty());
        ts3::funcs.getAvatar = nullptr;
    }

    // ---- settings and size ---------------------------------------------------------------------------------------
    void bubblesSetting()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.path() + QStringLiteral("/settings.ini");
        Settings      s;
        QVERIFY(s.chatBubbles); // on by default
        s.chatBubbles = false;
        s.save(file);
        Settings loaded;
        loaded.load(file);
        QVERIFY(!loaded.chatBubbles);
        loaded.chatBubbles = true;
        loaded.save(file);
        Settings again;
        again.chatBubbles = false;
        again.load(file);
        QVERIFY(again.chatBubbles);
    }

    void generalTabAccessKeys()
    {
        // The chat layout and emoji sections share the General tab: no access key twice (Alt+B would only
        // move between "bubbles" and "emoji button").
        QWidget      tab;
        QVector<QWidget*> sections = {new ChatLayoutSection(&tab), new EmojiSection(&tab)};
        QMap<QChar, QString> keys;
        for (QWidget* section : qAsConst(sections)) {
            for (const QAbstractButton* button : section->findChildren<QAbstractButton*>()) {
                const QString text = button->text();
                for (int i = text.indexOf(QLatin1Char('&')); i >= 0 && i + 1 < text.size(); i = text.indexOf(QLatin1Char('&'), i + 2)) {
                    if (text.at(i + 1) == QLatin1Char('&'))
                        continue;
                    const QChar key = text.at(i + 1).toLower();
                    QVERIFY2(!keys.contains(key), qPrintable(QStringLiteral("%1: \"%2\" and \"%3\"").arg(key, keys.value(key), text)));
                    keys.insert(key, text);
                    break;
                }
            }
        }
        QVERIFY(keys.size() >= 10);
        const auto* bubbles = tab.findChild<ChatLayoutSection*>();
        QVERIFY(bubbles);
    }

    void fourThousandMessages()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        for (int i = 0; i < 4000; ++i)
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:%1:%2").arg((i / 60) % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0')), 17,
                                                      i % 3 ? kAlice : kBob, i % 3 ? QStringLiteral("Alice") : QStringLiteral("Bob"), QStringLiteral("message number %1 with some words").arg(i)));
        const layoutdoc::Env env = envFor(Mode::Cozy, true);
        layoutdoc::styleAll(&doc, env);
        doc.documentLayout()->documentSize();
        // What ChatLayout does per paint: the shapes of a screen of blocks.
        QElapsedTimer clock;
        clock.start();
        int shapes = 0;
        for (int round = 0; round < 20; ++round)
            shapes += layoutbubbles::itemsIn(&doc, 3000 + round, 3040 + round, env).size();
        const qint64 ms = clock.elapsed();
        QVERIFY(shapes >= 20 * 40);
        QVERIFY2(ms < 200, qPrintable(QStringLiteral("%1 ms for 20 screens").arg(ms)));
        // Painting one screen.
        QImage      canvas(600, 800, QImage::Format_ARGB32_Premultiplied);
        QPainter    p(&canvas);
        const auto  items = layoutbubbles::itemsIn(&doc, 3000, 3040, env);
        clock.restart();
        for (int round = 0; round < 10; ++round)
            layoutbubbles::paint(p, items, env, QPointF(0, -items.first().rect.top()), layoutbubbles::State());
        p.end();
        QVERIFY2(clock.elapsed() < 400, qPrintable(QStringLiteral("%1 ms for 10 paints").arg(clock.elapsed())));
    }
};

TSMEDIA_REGISTER_TEST(TestChatVisuals)
#include "tst_chatvisuals.moc"
