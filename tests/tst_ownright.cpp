// 2.3.1: your messages on the right (Cozy with bubbles, like Telegram) and the chat's room at its end, on
// TeamSpeak-like chat documents (tests/tschat.h; made-up people only, right-to-left text as \u escapes). The
// geometry (flush right, as wide as the content, right-to-left text right-aligned, several lines, cards, polls,
// reaction rows, reply rows, the time never on the text), byte-exact restores (unload, layout and setting
// switches), the chat's end staying the end (a QTextBrowser), hit tests on the moved blocks, the action bar on the
// left, fitting again after another module's edit, the setting, the time for 4000 blocks, and the same right edge
// while the chat's scroll bar is hidden.

#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QtTest>

#include <cmath>
#include <functional>

#include "chatemoji.h"
#include "chatlayoutsection.h"
#include "chatscroll.h"
#include "layoutart.h"
#include "layoutbubbles.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "polldoc.h"
#include "polls.h"
#include "reactionart.h"
#include "replies.h"
#include "replydoc.h"
#include "settings.h"
#include "testmain.h"
#include "textreactdoc.h"
#include "tschat.h"

namespace {

using layoutart::Mode;

const QString kAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");
const QString kMe    = QStringLiteral("q0Xn6Mine000000000000000000=");

constexpr int kPreviewSeparator = QTextFormat::UserProperty + 0x7453; // ChatIntegration's line break before a picture

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

layoutdoc::Env envFor(Mode mode, bool bubbles, bool ownRight, int width = 512)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false, bubbles && mode == Mode::Cozy);
    env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
    env.dpr          = 1.0;
    env.contentWidth = width;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("o");
    env.ownNick      = QStringLiteral("Sam");
    env.ownUid       = kMe;
    env.mentions     = true;
    env.ownRight     = ownRight;
    env.replyHeader  = [](int, replyart::Header* h) {
        h->nick    = QStringLiteral("Alice");
        h->uid     = kAlice;
        h->snippet = QStringLiteral("Who is in for tonight?");
        h->found   = true;
        return true;
    };
    return env;
}

// ---- TeamSpeak's document, exactly (as tst_chatlayout compares it; the root frame's format too) ------------

struct Snapshot {
    QString              text;
    QVector<QVariantMap> chars;
    QVector<QVariantMap> blocks;
    QVector<bool>        visible;
    QVariantMap          frame;
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
    s.text  = doc.toPlainText();
    s.frame = propsOf(doc.rootFrame()->frameFormat());
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
    if (a.text != b.text) {
        int at = 0;
        while (at < a.text.size() && at < b.text.size() && a.text.at(at) == b.text.at(at))
            ++at;
        return QStringLiteral("text differs at %1: [%2] vs [%3]").arg(at).arg(a.text.mid(qMax(0, at - 30), 60), b.text.mid(qMax(0, at - 30), 60));
    }
    if (a.frame != b.frame)
        return QStringLiteral("the root frame's format differs (%1 vs %2 properties)").arg(a.frame.size()).arg(b.frame.size());
    if (a.blocks.size() != b.blocks.size())
        return QStringLiteral("block count %1 vs %2").arg(a.blocks.size()).arg(b.blocks.size());
    for (int i = 0; i < a.blocks.size(); ++i) {
        if (a.blocks.at(i) != b.blocks.at(i))
            return QStringLiteral("block %1 format differs (%2 vs %3 properties)").arg(i).arg(a.blocks.at(i).size()).arg(b.blocks.at(i).size());
        if (a.visible.at(i) != b.visible.at(i))
            return QStringLiteral("block %1 visibility differs").arg(i);
    }
    if (a.chars.size() != b.chars.size())
        return QStringLiteral("char count %1 vs %2").arg(a.chars.size()).arg(b.chars.size());
    for (int i = 0; i < a.chars.size(); ++i) {
        if (a.chars.at(i) != b.chars.at(i))
            return QStringLiteral("char %1 format differs").arg(i);
    }
    return {};
}

// ---- the chat ------------------------------------------------------------------------------------------------

// "Hello! I'm coming at nine tonight" and a longer one that wraps (Persian, right-to-left).
QString rtlShort()
{
    return QString::fromUtf8("\u0633\u0644\u0627\u0645! \u0627\u0645\u0634\u0628 \u0633\u0627\u0639\u062a \u0646\u0647 \u0645\u06cc\u0627\u0645");
}

QString rtlLong()
{
    return QString::fromUtf8("\u0627\u06cc\u0646 \u06cc\u06a9 \u067e\u06cc\u0627\u0645 \u0637\u0648\u0644\u0627\u0646\u06cc \u0627\u0633\u062a \u06a9\u0647 \u0628\u0627\u06cc\u062f \u062f\u0631 "
                             "\u0686\u0646\u062f \u062e\u0637 \u0634\u06a9\u0633\u062a\u0647 \u0634\u0648\u062f \u062a\u0627 \u0628\u0628\u06cc\u0646\u06cc\u0645 \u0645\u062a\u0646 "
                             "\u0631\u0627\u0633\u062a\u0686\u06cc\u0646 \u062f\u0631\u0633\u062a \u06a9\u0646\u0627\u0631 \u0647\u0645 \u0645\u06cc\u0645\u0627\u0646\u062f \u0648 "
                             "\u062d\u0628\u0627\u0628 \u0628\u0647 \u0627\u0646\u062f\u0627\u0632\u0647 \u0645\u062a\u0646 \u0627\u0633\u062a");
}

polls::Poll samplePoll()
{
    polls::Poll p;
    p.key       = QStringLiteral("3f9a1c2e5b6d7e80");
    p.question  = QStringLiteral("Which map tonight?");
    p.options   = QStringList{QStringLiteral("Harbor"), QStringLiteral("Desert Town"), QStringLiteral("Night Market")};
    p.channelId = 12;
    return p;
}

QString pollHtml(const tschat::Skin& s, const polls::Poll& poll)
{
    QString body = QStringLiteral("\U0001F4CA Poll: ") + tschat::escaped(poll.question);
    for (int i = 0; i < poll.options.size(); ++i)
        body += QStringLiteral("<br>%1. %2").arg(i + 1).arg(tschat::escaped(poll.options.at(i)));
    body += QStringLiteral("<br>") + tschat::linkHtml(s, QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat#") + polls::markerFragment(poll),
                                                      QStringLiteral("Vote with TS Media chat"));
    return body;
}

QString replyHtml(const QString& replyText)
{
    replies::Original alice;
    alice.nick     = QStringLiteral("Alice");
    alice.uid      = kAlice;
    alice.clientId = 17;
    alice.minutes  = 16 * 60;
    alice.text     = QStringLiteral("Who is in for tonight? The usual server, around nine.");
    QString quote  = replies::quoteLine(alice);
    quote.replace(QStringLiteral("[i]"), QStringLiteral("<i>")).replace(QStringLiteral("[/i]"), QStringLiteral("</i>"));
    quote.replace(QRegularExpression(QStringLiteral("\\[URL=([^\\]]+)\\]")), QStringLiteral("<a href=\"\\1\">")).replace(QStringLiteral("[/URL]"), QStringLiteral("</a>"));
    return quote + QStringLiteral("<br>") + tschat::escaped(replyText);
}

// As ChatIntegration::insertPreview: our line break, then the picture, at the block's end.
void insertPicture(QTextDocument& doc, int block, const QString& name, const QSizeF& size)
{
    const QTextBlock b = doc.findBlockByNumber(block);
    QTextCursor      c(&doc);
    c.setPosition(b.position() + b.length() - 1);
    QTextCharFormat separator;
    separator.setProperty(kPreviewSeparator, true);
    c.insertText(QString(QChar::LineSeparator), separator);
    QTextImageFormat f;
    f.setName(name);
    f.setWidth(size.width());
    f.setHeight(size.height());
    c.insertImage(f);
}

struct Ids {
    int alice = -1, shortOwn = -1, longOwn = -1, rtlShort = -1, rtlLong = -1, link = -1, card = -1, reply = -1, poll = -1, bob = -1, last = -1;
    QVector<int> own() const { return {shortOwn, longOwn, rtlShort, rtlLong, link, card, reply, poll, last}; }
};

// Alice, your messages (short, long, right-to-left short and long, a link, a file card, a reply to Alice, a poll),
// Bob, your last one. The modules come first, as in the plugin: the card, the poll card, the reply line.
Ids fillChat(QTextDocument& doc, int width = 520)
{
    setup(doc, width);
    const tschat::Skin s   = tschat::darkSkin();
    const auto         msg = [&](const char* time, int clid, const QString& uid, const QString& nick, const QString& html, bool out = false) {
        tschat::append(&doc, tschat::messageHtml(s, QString::fromLatin1(time), clid, uid, nick, html, out));
        return doc.blockCount() - 1;
    };
    const auto me = [&](const char* time, const QString& html) { return msg(time, 7, kMe, QStringLiteral("Sam"), html, true); };
    Ids        ids;
    tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Lobby"))));
    ids.alice    = msg("16:00:05", 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Who is in for tonight? The usual server, around nine."));
    ids.shortOwn = me("16:01:02", QStringLiteral("Yes! I'll bring the new map"));
    ids.longOwn  = me("16:01:40", QStringLiteral("Also, I can host the voice channel if nobody else wants to. It has the new map rotation and the music bot set up, so we "
                                                "can start right away, and I will keep the server running all night for everyone."));
    ids.rtlShort = me("16:02:30", tschat::escaped(rtlShort()));
    ids.rtlLong  = me("16:02:41", tschat::escaped(rtlLong()));
    ids.link     = me("16:02:55", tschat::linkHtml(s, QStringLiteral("https://example.com/maps/harbor-night")));
    ids.card     = me("16:03:20", tschat::linkHtml(s, QStringLiteral("ts3file://127.0.0.1?filename=project-files.zip"), QStringLiteral("project-files.zip")));
    ids.reply    = me("16:04:12", replyHtml(QStringLiteral("count me in, I'll be a bit late")));
    ids.poll     = me("16:05:00", pollHtml(s, samplePoll()));
    ids.bob      = msg("16:06:00", 18, kBob, QStringLiteral("Bob"), QStringLiteral("that was close"));
    ids.last     = me("16:06:30", QStringLiteral("ok"));
    // (Sized for the chat as ChatIntegration and ChatPolls size them: within the content column.)
    insertPicture(doc, ids.card, QStringLiteral("tsmedia:card"), QSizeF(qMin(240, width - 100), 64));
    int n = 0;
    for (const polldoc::Found& f : polldoc::scan(&doc)) {
        if (!f.collapsed)
            polldoc::collapse(&doc, f, polldoc::objectPrefix() + QStringLiteral("o%1").arg(++n), QSizeF(qMin(300, width - 80), 180));
    }
    polldoc::normalizeAll(&doc);
    for (const replydoc::Message& m : replydoc::scan(&doc)) {
        if (m.hasQuote && !m.restyled)
            replydoc::collapse(&doc, m, replydoc::objectPrefix() + QStringLiteral("o.%1").arg(m.block), QSizeF(0, 0));
    }
    return ids;
}

// The layout as ChatLayout puts it on (its step, then ChatPolls following its look, which ChatLayout measures again).
void styleOwnRight(QTextDocument& doc, const layoutdoc::Env& env)
{
    layoutdoc::styleAll(&doc, env);
    polldoc::normalizeAll(&doc);
    QSet<int> own;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (layoutformat::isOwnRight(b))
            own.insert(b.blockNumber());
    }
    layoutdoc::settle(&doc, {}, env, own);
    layoutdoc::applyEndGap(&doc, env);
    doc.documentLayout()->documentSize();
}

QVector<QRectF> lineRects(const QTextBlock& b, int from = 0)
{
    QVector<QRectF>    out;
    const QTextLayout* tl     = b.layout();
    const QPointF      origin = b.document()->documentLayout()->blockBoundingRect(b).topLeft();
    for (int i = from; tl && i < tl->lineCount(); ++i)
        out.append(tl->lineAt(i).naturalTextRect().translated(origin));
    return out;
}

// Where ChatIntegration (previewRect) and ChatPolls find a picture at position: from the line's cursorToX.
QRectF pictureRect(QTextDocument& doc, int position)
{
    const QTextBlock block = doc.findBlock(position);
    const int        rel   = position - block.position();
    const QTextLine  line  = block.layout()->lineForTextPosition(rel);
    if (!line.isValid())
        return {};
    QTextCursor c(&doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    const QTextImageFormat image = c.charFormat().toImageFormat();
    const QRectF           br    = doc.documentLayout()->blockBoundingRect(block);
    const qreal            x1    = line.cursorToX(rel);
    const qreal            x2    = line.cursorToX(rel + 1);
    const qreal            left  = qAbs(x2 - x1) > 0.5 ? qMin(x1, x2) : x1;
    const qreal            top   = qMax(line.y(), line.y() + line.ascent() - image.height());
    return QRectF(br.left() + left, br.top() + top, image.width(), image.height());
}

int objectPosition(const QTextBlock& b, const QString& prefix)
{
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextCharFormat cf = it.fragment().charFormat();
        if (cf.isImageFormat() && cf.toImageFormat().name().startsWith(prefix))
            return it.fragment().position();
    }
    return -1;
}

QMap<int, QString> keysOf(QTextDocument& doc)
{
    QMap<int, QString>   keys;
    const QDate          today = QDate(2026, 10, 10);
    const QVector<QDate> dates = layoutdoc::datesOf(&doc, today);
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const textreactdoc::Identity id = textreactdoc::identify(b);
        if (id.eligible())
            keys.insert(b.blockNumber(), textreactdoc::localKeyOf(id, textreactdoc::dayOf(dates.value(b.blockNumber()), id.seconds, today, 23 * 3600), 0));
    }
    return keys;
}

// ChatLayout's step for blocks (they are styled again: given back, then planned and applied), then settle().
void restyle(QTextDocument& doc, const layoutdoc::Env& env, const QVector<int>& blocks)
{
    {
        QTextCursor batch(&doc);
        batch.beginEditBlock();
        const QVector<QDate> dates = layoutdoc::datesOf(&doc, env.today);
        for (const int n : blocks) {
            layoutdoc::unstyle(&doc, n);
            if (layoutdoc::hasOurs(doc.findBlockByNumber(n)))
                layoutdoc::sanitize(&doc, n);
            const QTextBlock     b   = doc.findBlockByNumber(n);
            const layoutdoc::Row row = layoutdoc::read(b);
            if (row.type == layoutdoc::RowType::Unknown || row.styled)
                continue;
            layoutdoc::apply(&doc, n, layoutdoc::plan(env, b, row, layoutdoc::previousOf(b), dates.value(n), 0, 5000 + n), env);
        }
        batch.endEditBlock();
    }
    layoutdoc::settle(&doc, blocks, env);
    // ChatPolls follows the new look (ChatLayout's documentEdited); ChatLayout measures what it changed again.
    if (polldoc::normalizeAll(&doc) > 0) {
        QSet<int> own;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (layoutformat::isOwnRight(b))
                own.insert(b.blockNumber());
        }
        layoutdoc::settle(&doc, {}, env, own);
    }
    layoutdoc::applyEndGap(&doc, env);
    doc.documentLayout()->documentSize();
}

// ChatLayout giving everything back (the room at the end and every block), ChatPolls following.
void giveBackLayout(QTextDocument& doc)
{
    layoutdoc::removeEndGap(&doc);
    layoutdoc::restoreAll(&doc);
    polldoc::normalizeAll(&doc);
}

} // namespace

class TestOwnRight : public QObject
{
    Q_OBJECT

  private slots:
    // ---- 1. geometry --------------------------------------------------------------------------------------------
    void ownMessagesSitOnTheRight()
    {
        for (const int width : {300, 520, 900}) {
            QTextDocument  doc;
            const Ids      ids = fillChat(doc, width);
            layoutdoc::Env env = envFor(Mode::Cozy, true, true, width - 8);
            styleOwnRight(doc, env);
            const layoutart::Tokens& t = env.tokens;
            const qreal              R = layoutdoc::ownRightEdge(env) + doc.documentMargin(); // the text's right edge
            QVERIFY2(doc.documentLayout()->documentSize().width() <= doc.textWidth() + 0.5, qPrintable(QString::number(width)));
            for (const int n : ids.own()) {
                const QTextBlock          b    = doc.findBlockByNumber(n);
                const layoutbubbles::Item item = layoutbubbles::itemOf(b, env);
                const QString             what = QStringLiteral("width %1 block %2").arg(width).arg(n);
                QVERIFY2(layoutformat::isOwnRight(b) && item.kind == layoutbubbles::Kind::Bubble && item.right && item.own, qPrintable(what));
                // Flush right: every bubble of yours ends at the same x, the right margin of the column.
                QCOMPARE(item.rect.right(), R + t.bubblePadX);
                // Never past the chat's right edge, never left of the content column (a picture may need more).
                QVERIFY2(item.rect.right() <= doc.textWidth() - doc.documentMargin(), qPrintable(what));
                QVERIFY2(item.rect.left() >= doc.documentMargin() + t.G - t.bubblePadX - 1, qPrintable(what));
                const layoutdoc::OwnPlace place = layoutdoc::ownPlace(b, env);
                QVERIFY2(place.valid, qPrintable(what));
                // The time is on the bubble, never on the text.
                QCOMPARE(place.time == layoutformat::TimeNone, false);
                QVERIFY2(item.rect.contains(place.timeRect), qPrintable(what));
                for (const QRectF& r : lineRects(b, place.first))
                    QVERIFY2(!r.intersects(place.timeRect), qPrintable(what + QStringLiteral(": the time on a line")));
                QCOMPARE(item.time, layoutart::shortTime(layoutdoc::read(b).time, false));
                // No avatar and no name: a picture only for a reply row.
                const layoutformat::Lead lead = layoutformat::leadOf(b);
                if (lead.kind == layoutformat::Head && lead.reply < 0) {
                    QCOMPARE(lead.format.toImageFormat().name(), QStringLiteral("tsmedia-layout:blank"));
                    QVERIFY(layoutdoc::render(env, b, env.today, layoutdoc::Look(), nullptr).isNull());
                }
            }
            for (const int n : {ids.alice, ids.bob}) {
                const layoutbubbles::Item item = layoutbubbles::itemOf(doc.findBlockByNumber(n), env);
                QVERIFY(item.kind == layoutbubbles::Kind::Bubble && !item.right && !item.own);
                QCOMPARE(item.rect.left(), doc.documentMargin() + t.G - t.bubblePadX); // as in 2.3: on the left
            }

            // As wide as the content: a short one ("ok") is its line, the gap and the time.
            {
                const QTextBlock          b     = doc.findBlockByNumber(ids.last);
                const layoutdoc::OwnPlace place = layoutdoc::ownPlace(b, env);
                QString                   lines;
                for (int i = 0; i < b.layout()->lineCount(); ++i)
                    lines += QStringLiteral(" [%1+%2 w%3 nat%4]").arg(b.layout()->lineAt(i).textStart()).arg(b.layout()->lineAt(i).textLength()).arg(b.layout()->lineAt(i).width()).arg(b.layout()->lineAt(i).naturalTextWidth());
                QVERIFY2(b.layout()->lineCount() == 1, qPrintable(QStringLiteral("width %1: fit %2 left %3 indent %4:%5").arg(width).arg(b.blockFormat().doubleProperty(layoutformat::kOwnFit))
                                                                       .arg(b.blockFormat().leftMargin()).arg(b.blockFormat().textIndent()).arg(lines)));
                QCOMPARE(place.time, int(layoutformat::TimeInline));
                const qreal line = b.layout()->lineAt(0).naturalTextWidth();
                const qreal want = std::ceil(line + t.ownTimeGap + layoutart::ownTimeWidth(item0Time(b), t) - 0.02);
                QVERIFY2(qAbs((place.right - place.left) - want) <= 1.0, qPrintable(QStringLiteral("%1 vs %2").arg(place.right - place.left).arg(want)));
                QVERIFY(b.blockFormat().alignment() & Qt::AlignLeft);
                // Left-aligned text from the bubble's left padding.
                QCOMPARE(lineRects(b).first().left(), place.left);
            }
            // Several lines: left-aligned from one edge, the line breaks of the whole column kept.
            {
                const QTextBlock b = doc.findBlockByNumber(ids.longOwn);
                QVERIFY(b.layout()->lineCount() >= 2);
                const QVector<QRectF> lines = lineRects(b);
                for (const QRectF& r : lines)
                    QCOMPARE(r.left(), lines.first().left());
                QTextDocument* wide = doc.clone();
                {
                    QTextBlock       w  = wide->findBlockByNumber(ids.longOwn);
                    QTextBlockFormat bf = w.blockFormat();
                    bf.setLeftMargin(qMax(0.0, layoutdoc::ownRightEdge(env) - layoutdoc::ownRoom(w, env)));
                    QTextCursor(w).setBlockFormat(bf);
                }
                wide->documentLayout()->documentSize();
                const QTextLayout* a = b.layout();
                const QTextLayout* c = wide->findBlockByNumber(ids.longOwn).layout();
                QCOMPARE(a->lineCount(), c->lineCount());
                for (int i = 0; i < a->lineCount(); ++i)
                    QCOMPARE(a->lineAt(i).textStart(), c->lineAt(i).textStart());
                delete wide;
                // At most Telegram's width in a wide chat.
                const layoutdoc::OwnPlace lp = layoutdoc::ownPlace(b, env);
                QVERIFY(lp.right - lp.left <= 36 * t.f + 1);
            }
            // Right-to-left text: right-aligned, its time where it ends (on the left).
            for (const int n : {ids.rtlShort, ids.rtlLong}) {
                const QTextBlock b = doc.findBlockByNumber(n);
                QVERIFY(b.blockFormat().alignment() & Qt::AlignRight);
                QCOMPARE(b.blockFormat().layoutDirection(), Qt::LeftToRight); // the block stays left-to-right (heads, hit tests)
                const layoutdoc::OwnPlace place = layoutdoc::ownPlace(b, env);
                const QVector<QRectF>     lines = lineRects(b);
                for (const QRectF& r : lines)
                    QVERIFY2(qAbs(r.right() - R) <= 1.5, qPrintable(QStringLiteral("%1 vs %2").arg(r.right()).arg(R)));
                QVERIFY(place.timeRect.right() <= lines.last().left() + 0.5);
                QVERIFY(qAbs(place.timeRect.left() - place.left) < 0.01);
            }
            QVERIFY(doc.findBlockByNumber(ids.rtlLong).layout()->lineCount() >= 2);
            // A card (a picture at the end): the time on a row of its own under it; the next block makes room.
            {
                const QTextBlock          b       = doc.findBlockByNumber(ids.card);
                const layoutdoc::OwnPlace place   = layoutdoc::ownPlace(b, env);
                const QRectF              picture = pictureRect(doc, objectPosition(b, QStringLiteral("tsmedia:card")));
                QCOMPARE(place.time, int(layoutformat::TimeRow));
                QVERIFY(place.timeRect.top() >= picture.bottom() - 0.5);
                QVERIFY(layoutbubbles::itemOf(b, env).rect.contains(picture));
                // Flush right: the widest line (the card, or its file name in a narrow chat) ends at the edge.
                qreal widest = 0;
                for (const QRectF& r : lineRects(b))
                    widest = qMax(widest, r.right());
                QCOMPARE(qRound(widest), qRound(R));
                QVERIFY(picture.right() <= R + 0.5);
                QCOMPARE(b.blockFormat().doubleProperty(layoutformat::kOwnTimeRow), qreal(t.ownTimeRow));
                const QTextBlock next = doc.findBlockByNumber(ids.reply);
                QCOMPARE(next.blockFormat().doubleProperty(layoutformat::kTopExtra), qreal(t.ownTimeRow));
                // The bubbles don't touch: the next one starts below this one.
                QVERIFY(layoutbubbles::itemOf(next, env).rect.top() > layoutbubbles::itemOf(b, env).rect.bottom());
            }
            // The reply row above your bubble, flush right with it; the head shows nothing else.
            {
                const QTextBlock         b    = doc.findBlockByNumber(ids.reply);
                const layoutformat::Lead lead = layoutformat::leadOf(b);
                QVERIFY(lead.kind == layoutformat::Head && lead.reply >= 0);
                const QRectF picture = pictureRect(doc, lead.object);
                QCOMPARE(qRound(picture.right()), qRound(R));
                QVERIFY(picture.top() < layoutbubbles::itemOf(b, env).rect.top());
                layoutart::HeadGeometry g;
                QSizeF                  size;
                QVERIFY(layoutdoc::headGeometry(env, b, env.today, &g, &size));
                QVERIFY(g.name.isEmpty() && g.time.isEmpty() && g.avatar.isEmpty());
                QCOMPARE(g.replyRow, QRectF(0, 0, size.width(), t.replyRow));
                const QImage head = layoutdoc::render(env, b, env.today, layoutdoc::Look(), nullptr);
                QVERIFY(!head.isNull());
                QCOMPARE(head.size(), size.toSize());
            }
            // The poll card inside your bubble.
            {
                const QTextBlock b = doc.findBlockByNumber(ids.poll);
                const QRectF     card = pictureRect(doc, objectPosition(b, polldoc::objectPrefix()));
                QVERIFY(!card.isEmpty());
                QVERIFY(layoutbubbles::itemOf(b, env).rect.contains(card));
            }
            // Groups: your bubbles meet with small corners on the right.
            {
                const layoutbubbles::Item a = layoutbubbles::itemOf(doc.findBlockByNumber(ids.shortOwn), env);
                const layoutbubbles::Item c = layoutbubbles::itemOf(doc.findBlockByNumber(ids.longOwn), env);
                QVERIFY(a.joinBelow && c.joinAbove);
                const QPainterPath path = layoutbubbles::pathOf(c, t);
                // The top left corner is round (far from the corner point), the top right one small.
                QVERIFY(!path.contains(c.rect.topLeft() + QPointF(1.5, 1.5)));
                QVERIFY(path.contains(c.rect.topRight() + QPointF(-2.5, 2.5)));
            }
        }
    }

    // Live test: TeamSpeak shows a message that starts right-to-left as LRM, RLE, the text, PDF, LRM. It is right-to-left
    // all the same (right-aligned, its time where it ends), also as a reply; left-to-right text with a Persian word in
    // it, or after an LRM of its own, stays left-aligned.
    void teamSpeaksBidiMarks()
    {
        const QString lrm  = QString(QChar(0x200E));
        const QString wrap = lrm + QChar(0x202B);
        const QString end  = QString(QChar(0x202C)) + lrm;
        for (const int width : {300, 520}) {
            QTextDocument doc;
            setup(doc, width);
            const tschat::Skin s  = tschat::darkSkin();
            const auto         me = [&](const char* time, const QString& html) {
                tschat::append(&doc, tschat::messageHtml(s, QString::fromLatin1(time), 7, kMe, QStringLiteral("Sam"), html, true));
                return doc.blockCount() - 1;
            };
            tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
            const int shortRtl = me("16:02:30", tschat::escaped(wrap + rtlShort() + end));
            const int longRtl  = me("16:02:41", tschat::escaped(wrap + rtlLong() + end));
            const int rlm      = me("16:02:45", tschat::escaped(QString(QChar(0x200F)) + rtlShort()));
            const int reply    = me("16:02:50", replyHtml(wrap + rtlShort() + end));
            const int mixed    = me("16:02:55", tschat::escaped(QStringLiteral("See you at ") + rtlShort().left(4) + QStringLiteral(" tonight")));
            const int marked   = me("16:03:00", tschat::escaped(lrm + QStringLiteral("Plain words after a mark")));
            for (const replydoc::Message& m : replydoc::scan(&doc)) {
                if (m.hasQuote && !m.restyled)
                    replydoc::collapse(&doc, m, replydoc::objectPrefix() + QStringLiteral("o.%1").arg(m.block), QSizeF(0, 0));
            }
            const layoutdoc::Env env = envFor(Mode::Cozy, true, true, width - 8);
            styleOwnRight(doc, env);
            const qreal R = layoutdoc::ownRightEdge(env) + doc.documentMargin();
            for (const int n : {shortRtl, longRtl, rlm, reply}) {
                const QTextBlock b    = doc.findBlockByNumber(n);
                const QString    what = QStringLiteral("width %1 block %2").arg(width).arg(n);
                QVERIFY2(layoutformat::isOwnRight(b) && layoutformat::isOwnRightAligned(b), qPrintable(what));
                const layoutdoc::OwnPlace place = layoutdoc::ownPlace(b, env);
                const QVector<QRectF>     lines = lineRects(b, place.first);
                QVERIFY2(!lines.isEmpty(), qPrintable(what));
                for (const QRectF& r : lines)
                    QVERIFY2(qAbs(r.right() - R) <= 1.5, qPrintable(what + QStringLiteral(": a line ends at %1, not %2").arg(r.right()).arg(R)));
                if (place.time == layoutformat::TimeRow) // (no room after the line: on a row of its own, under the text's left end)
                    QVERIFY2(place.timeRect.top() >= lines.last().bottom() - 0.5 && qAbs(place.timeRect.left() - place.left) < 0.01, qPrintable(what));
                else
                    QVERIFY2(place.time == layoutformat::TimeInline && place.timeRect.right() <= lines.last().left() + 0.5, qPrintable(what + QStringLiteral(": the time is not where the text ends")));
                QCOMPARE(layoutbubbles::itemOf(b, env).rect.right(), R + env.tokens.bubblePadX);
            }
            QVERIFY(doc.findBlockByNumber(longRtl).layout()->lineCount() >= 2);
            for (const int n : {mixed, marked}) {
                const QTextBlock b = doc.findBlockByNumber(n);
                QVERIFY(layoutformat::isOwnRight(b) && !layoutformat::isOwnRightAligned(b));
            }
        }
    }

    // Reaction rows (ChatTextReactions) in your bubbles: the time after the row; a row added later fits again.
    void reactionRowsAndFittingAgain()
    {
        QTextDocument  doc;
        const Ids      ids = fillChat(doc);
        layoutdoc::Env env = envFor(Mode::Cozy, true, true);
        const QMap<int, QString> keys = keysOf(doc);
        QVERIFY(keys.contains(ids.shortOwn));
        textreactdoc::insertRow(&doc, ids.shortOwn, textreactdoc::objectPrefix() + QStringLiteral("o1"), keys.value(ids.shortOwn), QSizeF(96, 26));
        styleOwnRight(doc, env);
        {
            const QTextBlock          b     = doc.findBlockByNumber(ids.shortOwn);
            const QRectF              row   = textreactdoc::rowRect(b);
            const layoutdoc::OwnPlace place = layoutdoc::ownPlace(b, env);
            QVERIFY(!row.isEmpty());
            QCOMPARE(place.time, int(layoutformat::TimeInline));
            QVERIFY(place.timeRect.left() >= row.right() + env.tokens.ownTimeGap - 0.5); // right of the row, on its line
            QVERIFY(place.timeRect.bottom() <= row.bottom() + 1 && place.timeRect.top() >= row.top() - 1);
            QVERIFY(layoutbubbles::itemOf(b, env).rect.contains(row));
        }
        // Settled: nothing more to do (ChatLayout measures again after other modules' edits; that never loops).
        QSet<int> all;
        for (const int n : ids.own())
            all.insert(n);
        QCOMPARE(layoutdoc::settle(&doc, {}, env, all), 0);
        // A row added later (a reaction came in): measured again, the bubble holds it, flush right still.
        const int n = ids.longOwn;
        textreactdoc::insertRow(&doc, n, textreactdoc::objectPrefix() + QStringLiteral("o2"), keys.value(n), QSizeF(140, 26));
        doc.documentLayout()->documentSize();
        layoutdoc::settle(&doc, {}, env, {n}); // (narrower than the text: its place stays, the time goes after the row)
        doc.documentLayout()->documentSize();
        const QTextBlock b = doc.findBlockByNumber(n);
        QVERIFY(layoutbubbles::itemOf(b, env).rect.contains(textreactdoc::rowRect(b)));
        QVERIFY(layoutdoc::ownPlace(b, env).timeRect.left() > textreactdoc::rowRect(b).right());
        QCOMPARE(layoutbubbles::itemOf(b, env).rect.right(), layoutdoc::ownRightEdge(env) + doc.documentMargin() + env.tokens.bubblePadX);
        QCOMPARE(layoutdoc::settle(&doc, {}, env, {n}), 0);
        // A wider picture (a preview that got its real size): the bubble grows to the left.
        const int   card   = ids.card;
        const qreal before = layoutbubbles::itemOf(doc.findBlockByNumber(card), env).rect.left();
        {
            const int   at = objectPosition(doc.findBlockByNumber(card), QStringLiteral("tsmedia:card"));
            QTextCursor c(&doc);
            c.setPosition(at);
            c.setPosition(at + 1, QTextCursor::KeepAnchor);
            QTextImageFormat f = c.charFormat().toImageFormat();
            f.setWidth(380);
            c.setCharFormat(f);
        }
        doc.documentLayout()->documentSize();
        QVERIFY(layoutdoc::settle(&doc, {}, env, {card}) > 0);
        doc.documentLayout()->documentSize();
        const layoutbubbles::Item grown = layoutbubbles::itemOf(doc.findBlockByNumber(card), env);
        QVERIFY(grown.rect.left() < before - 100);
        QCOMPARE(grown.rect.right(), layoutdoc::ownRightEdge(env) + doc.documentMargin() + env.tokens.bubblePadX);
        QVERIFY(doc.documentLayout()->documentSize().width() <= doc.textWidth() + 0.5);
    }

    // Review (visual): the room over a group is the same whoever's it is. Your bubble starts bubblePadTop over its
    // text, someone else's at its name row: yours got 4 px less (7 px under Alice, 11 under yours).
    void groupsSpacedAlike()
    {
        for (const int width : {300, 520, 900}) {
            QTextDocument  doc;
            const Ids      ids = fillChat(doc, width);
            layoutdoc::Env env = envFor(Mode::Cozy, true, true, width - 8);
            styleOwnRight(doc, env);
            const auto  rect     = [&](int n) { return layoutbubbles::itemOf(doc.findBlockByNumber(n), env).rect; };
            const qreal toYours  = rect(ids.shortOwn).top() - rect(ids.alice).bottom(); // Alice, then you
            const qreal toTheirs = rect(ids.bob).top() - rect(ids.poll).bottom();       // you (a time row), then Bob
            const qreal back     = rect(ids.last).top() - rect(ids.bob).bottom();       // Bob, then you again
            const QString what   = QStringLiteral("width %1: %2 / %3 / %4").arg(width).arg(toYours).arg(toTheirs).arg(back);
            QVERIFY2(qAbs(toYours - toTheirs) <= 1.5 && qAbs(back - toTheirs) <= 1.5, qPrintable(what));
            QVERIFY2(toYours > 2 * env.tokens.bubbleGap + 2, qPrintable(what)); // well apart from a group's own gap
            // A group of yours keeps its small gap.
            const qreal joined = rect(ids.longOwn).top() - rect(ids.shortOwn).bottom();
            QVERIFY2(joined >= env.tokens.bubbleGap - 1 && joined + 3 <= toTheirs, qPrintable(what + QStringLiteral(", joined %1").arg(joined)));
        }
    }

    // Review (visual): the reaction row of your right-to-left message is right-aligned with its text. Drawn as laid
    // out it ended in the add pill's empty slot, its pills about 38 px short of the text's right edge: it is drawn
    // (and pointed at) mirrored, its pills flush right, the slot on the left.
    void rightToLeftReactionRowsMirrored()
    {
        ReactionView view;
        for (int i = 0; i < 3; ++i) {
            ReactionView::Entry e;
            e.reaction = 100 + i;
            e.count    = 3 - i;
            e.mine     = i == 0;
            view.entries.append(e);
        }
        const rx::MeasureText measure = [](const QString& text, bool) { return 7.0 * text.size(); };
        const rx::RowLayout   row     = rx::layoutRow(view, 0, 400, measure);
        const rx::RowLayout   m       = rx::mirrored(row);
        QVERIFY(!row.isEmpty());
        QCOMPARE(m.size, row.size);
        const qreal width     = row.size.width();
        qreal       rowRight  = 0;
        qreal       mineRight = 0;
        for (int i = 0; i < row.pills.size(); ++i) {
            rowRight  = qMax(rowRight, row.pills.at(i).right());
            mineRight = qMax(mineRight, m.pills.at(i).right());
            QCOMPARE(m.pills.at(i).size(), row.pills.at(i).size());
            QCOMPARE(m.pills.at(i).top(), row.pills.at(i).top());
            QCOMPARE(m.pills.at(i).left(), width - row.pills.at(i).right());
            // Pointing finds the pill where it is drawn.
            const rx::ZoneHit hit = rx::zoneAt(QSizeF(0, 0), m, false, m.pills.at(i).center());
            QCOMPARE(int(hit.zone), int(rx::Zone::Pill));
            QCOMPARE(hit.index, row.reactions.at(i));
        }
        QVERIFY(rowRight < width - rx::kAddPillWidth); // laid out: the add pill's slot at the right end
        QCOMPARE(mineRight, width);                   // mirrored: the pills end at the row's right edge
        QVERIFY(m.addPill.left() >= 0 && m.addPill.left() < 1.0);
        QCOMPARE(int(rx::zoneAt(QSizeF(0, 0), m, false, m.addPill.center()).zone), int(rx::Zone::AddPill));
        // A row on two lines: every line from the right.
        const rx::RowLayout two = rx::mirrored(rx::layoutRow(view, 0, 120, measure));
        QVERIFY(two.size.height() > rx::kRowTopMargin + rx::kPillHeight);
        QMap<int, qreal> lineRight;
        for (const QRectF& r : two.pills)
            lineRight[qRound(r.top())] = qMax(lineRight.value(qRound(r.top())), r.right());
        QVERIFY(lineRight.size() >= 2);
        for (const qreal r : lineRight)
            QCOMPARE(r, qreal(two.size.width()));

        // In the chat: which rows are drawn mirrored (yours with right-to-left text only), and that row's picture
        // ends at its text's right edge.
        QTextDocument            doc;
        const Ids                ids  = fillChat(doc);
        const QMap<int, QString> keys = keysOf(doc);
        for (const int n : {ids.rtlShort, ids.shortOwn, ids.alice})
            QVERIFY(textreactdoc::insertRow(&doc, n, textreactdoc::objectPrefix() + QStringLiteral("m%1").arg(n), keys.value(n), QSizeF(row.size)));
        layoutdoc::Env env = envFor(Mode::Cozy, true, true);
        styleOwnRight(doc, env);
        const QTextBlock rtl = doc.findBlockByNumber(ids.rtlShort);
        QVERIFY(layoutformat::isOwnRightAligned(rtl));
        QVERIFY(!layoutformat::isOwnRightAligned(doc.findBlockByNumber(ids.shortOwn)));
        QVERIFY(!layoutformat::isOwnRightAligned(doc.findBlockByNumber(ids.alice)));
        const QRectF picture = textreactdoc::rowRect(rtl);
        QVERIFY(!picture.isEmpty());
        QVERIFY2(qAbs(picture.right() - lineRects(rtl).first().right()) <= 1.5, qPrintable(QStringLiteral("%1 vs %2").arg(picture.right()).arg(lineRects(rtl).first().right())));
        QVERIFY(layoutbubbles::itemOf(rtl, env).rect.contains(picture));
        // The setting off: on the left, as laid out.
        restyle(doc, envFor(Mode::Cozy, true, false), {ids.rtlShort});
        QVERIFY(!layoutformat::isOwnRightAligned(doc.findBlockByNumber(ids.rtlShort)));
    }

    // ---- 2. restored exactly ---------------------------------------------------------------------------------------
    void restoredExactly()
    {
        QTextDocument  plain, doc;
        const Ids      ids = fillChat(plain);
        fillChat(doc);
        const Snapshot before = snapshot(doc);
        QCOMPARE(compare(before, snapshot(plain)), QString());
        const layoutdoc::Env on = envFor(Mode::Cozy, true, true);
        styleOwnRight(doc, on);
        QVERIFY(layoutdoc::hasEndGap(&doc));
        QVERIFY(layoutformat::isOwnRight(doc.findBlockByNumber(ids.card)));
        QVERIFY(compare(before, snapshot(doc)) != QString());
        // The plugin going (ChatLayout's restore: the gap and every block; ChatPolls following).
        QVERIFY(layoutdoc::removeEndGap(&doc));
        QVERIFY(layoutdoc::restoreAll(&doc) > 0);
        polldoc::normalizeAll(&doc);
        QCOMPARE(compare(before, snapshot(doc)), QString());
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            QVERIFY(!layoutdoc::hasOurs(b));
        // restoreAll alone gives the frame back too (a retired document: giveBack).
        styleOwnRight(doc, on);
        QVERIFY(layoutdoc::giveBack(&doc) > 0);
        polldoc::normalizeAll(&doc);
        QCOMPARE(compare(before, snapshot(doc)), QString());

        // The setting switched off and on again (ChatLayout restyles every block): yours on the left, then back.
        styleOwnRight(doc, on);
        QVector<int> all;
        for (int n = doc.blockCount() - 1; n >= 0; --n)
            all.append(n); // the screen bottom up, as ChatLayout's step
        const layoutdoc::Env off = envFor(Mode::Cozy, true, false);
        restyle(doc, off, all);
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            QVERIFY(!layoutformat::isOwnRight(b));
            QVERIFY(!b.blockFormat().hasProperty(layoutformat::kTopExtra));
            QVERIFY(!b.blockFormat().hasProperty(layoutformat::kOwnTimeRow));
        }
        QVERIFY(!layoutbubbles::itemOf(doc.findBlockByNumber(ids.card), off).right);
        {
            // The same as styled with the setting off from the start (margins, the time row's room gone).
            QTextDocument direct;
            fillChat(direct);
            layoutdoc::styleAll(&direct, off);
            layoutdoc::applyEndGap(&direct, off);
            for (QTextBlock a = doc.begin(), b = direct.begin(); a.isValid() && b.isValid(); a = a.next(), b = b.next()) {
                QCOMPARE(a.blockFormat().topMargin(), b.blockFormat().topMargin());
                QCOMPARE(a.blockFormat().bottomMargin(), b.blockFormat().bottomMargin());
                QCOMPARE(a.blockFormat().leftMargin(), b.blockFormat().leftMargin());
                QCOMPARE(a.blockFormat().rightMargin(), b.blockFormat().rightMargin());
            }
        }
        restyle(doc, on, all);
        QVERIFY(layoutbubbles::itemOf(doc.findBlockByNumber(ids.card), on).right);
        QCOMPARE(doc.findBlockByNumber(ids.reply).blockFormat().doubleProperty(layoutformat::kTopExtra), qreal(on.tokens.ownTimeRow));
        // A layout switch: Compact (nothing on the right), back to Cozy, then the plugin going.
        giveBackLayout(doc);
        QCOMPARE(compare(before, snapshot(doc)), QString());
        const layoutdoc::Env compact = envFor(Mode::Compact, false, true);
        styleOwnRight(doc, compact);
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            QVERIFY(!layoutformat::isOwnRight(b));
        giveBackLayout(doc);
        styleOwnRight(doc, on);
        giveBackLayout(doc);
        QCOMPARE(compare(before, snapshot(doc)), QString());
    }

    // Compact and Cozy without bubbles look exactly as before with the setting on (it is for bubbles only).
    void onlyCozyWithBubbles()
    {
        for (const auto& look : {qMakePair(Mode::Compact, false), qMakePair(Mode::Cozy, false)}) {
            QTextDocument a, b;
            fillChat(a);
            fillChat(b);
            layoutdoc::styleAll(&a, envFor(look.first, look.second, true));
            layoutdoc::styleAll(&b, envFor(look.first, look.second, false));
            QCOMPARE(compare(snapshot(a), snapshot(b)), QString());
        }
    }

    // ---- 3. the chat's end -----------------------------------------------------------------------------------------
    void theEndStaysTheEnd()
    {
        QTextBrowser browser;
        browser.setAttribute(Qt::WA_DontShowOnScreen);
        browser.resize(560, 260);
        QTextDocument* doc = browser.document();
        fillChat(*doc, 540);
        for (int i = 0; i < 12; ++i)
            tschat::append(doc, tschat::messageHtml(tschat::darkSkin(), QStringLiteral("16:07:%1").arg(i, 2, 10, QLatin1Char('0')), i % 2 ? 7 : 18, i % 2 ? kMe : kBob,
                                                     i % 2 ? QStringLiteral("Sam") : QStringLiteral("Bob"), QStringLiteral("line %1").arg(i), i % 2));
        browser.show();
        QCoreApplication::processEvents();
        QScrollBar* bar = browser.verticalScrollBar();
        chatscroll::syncRange(&browser);
        bar->setValue(bar->maximum());
        const layoutdoc::Env env = envFor(Mode::Cozy, true, true, browser.viewport()->width() - 8);
        const auto           step = [&](const std::function<void()>& edit) {
            const chatscroll::Place place = chatscroll::capture(&browser);
            {
                layoutformat::LayoutEditScope editing;
                edit();
            }
            chatscroll::restore(&browser, place);
        };
        const auto roomUnderLast = [&] {
            QTextBlock last = doc->lastBlock();
            const QRectF r  = layoutbubbles::itemOf(last, env).rect;
            return (bar->value() + browser.viewport()->height()) - r.bottom();
        };
        step([&] { styleOwnRight(*doc, env); });
        QCOMPARE(bar->value(), bar->maximum());
        QVERIFY(layoutdoc::endGapExtra(doc) >= env.tokens.chatBottom);
        QVERIFY2(roomUnderLast() >= env.tokens.chatBottom - 2, qPrintable(QString::number(roomUnderLast())));
        // New lines (yours with a card last: its time row under it): TeamSpeak appends, QTextEdit::append keeps the
        // end; or TeamSpeak shows only up to the last line (the room hidden): ChatLayout's step brings the end back.
        for (int round = 0; round < 3; ++round) {
            const bool atMaximum = bar->value() >= bar->maximum();
            tschat::append(doc, tschat::messageHtml(tschat::darkSkin(), QStringLiteral("16:09:%1").arg(round, 2, 10, QLatin1Char('0')), 7, kMe, QStringLiteral("Sam"),
                                                     tschat::linkHtml(tschat::darkSkin(), QStringLiteral("ts3file://127.0.0.1?filename=a.zip"), QStringLiteral("a.zip")), true));
            const int n = doc->blockCount() - 1;
            if (round == 1)
                insertPicture(*doc, n, QStringLiteral("tsmedia:late"), QSizeF(240, 64));
            chatscroll::syncRange(&browser);
            if (round == 2)
                bar->setValue(bar->maximum() - qRound(layoutdoc::endGapExtra(doc))); // only the last line in view
            else if (atMaximum)
                bar->setValue(bar->maximum());
            step([&] { restyle(*doc, env, {n}); });
            QCOMPARE(bar->value(), bar->maximum());
            QVERIFY2(roomUnderLast() >= env.tokens.chatBottom - 2, qPrintable(QStringLiteral("round %1: %2").arg(round).arg(roomUnderLast())));
            // A card last: the room grew by its time row (Qt leaves the last block's bottom margin out).
            QCOMPARE(layoutdoc::endGapExtra(doc), qreal(env.tokens.chatBottom + (round == 1 ? env.tokens.ownTimeRow : 0)));
        }
        // A layout switch at the end stays at the end (with Compact's room).
        const layoutdoc::Env compact = envFor(Mode::Compact, false, true, browser.viewport()->width() - 8);
        step([&] {
            giveBackLayout(*doc);
            styleOwnRight(*doc, compact);
        });
        QCOMPARE(bar->value(), bar->maximum());
        QCOMPARE(layoutdoc::endGapExtra(doc), qreal(compact.tokens.chatBottom));
        // TeamSpeak classic: no room of ours, still at the end.
        step([&] { giveBackLayout(*doc); });
        QCOMPARE(bar->value(), bar->maximum());
        QCOMPARE(layoutdoc::endGapExtra(doc), 0.0);
        QVERIFY(!layoutdoc::hasEndGap(doc));
    }

    // TeamSpeak's own margins: given back exactly; set again by TeamSpeak meanwhile: theirs stay.
    void endGapGivesTeamSpeaksMarginsBack()
    {
        QTextDocument doc;
        fillChat(doc);
        doc.setDocumentMargin(4);
        const QVariantMap    original = propsOf(doc.rootFrame()->frameFormat());
        const layoutdoc::Env env      = envFor(Mode::Cozy, true, true);
        QVERIFY(layoutdoc::applyEndGap(&doc, env));
        QVERIFY(!layoutdoc::applyEndGap(&doc, env)); // nothing more
        QCOMPARE(doc.rootFrame()->frameFormat().bottomMargin(), 4.0 + env.tokens.chatBottom);
        QCOMPARE(doc.rootFrame()->frameFormat().topMargin(), 4.0 + env.tokens.chatTop);
        QCOMPARE(doc.rootFrame()->frameFormat().leftMargin(), 4.0);
        QCOMPARE(doc.documentMargin(), 4.0);
        QVERIFY(layoutdoc::removeEndGap(&doc));
        QCOMPARE(propsOf(doc.rootFrame()->frameFormat()), original);
        QVERIFY(!layoutdoc::removeEndGap(&doc));
        // TeamSpeak sets its margin meanwhile: ours goes on top of the new one, and its value stays at the end.
        QVERIFY(layoutdoc::applyEndGap(&doc, env));
        doc.setDocumentMargin(6);
        QVERIFY(layoutdoc::applyEndGap(&doc, env));
        QCOMPARE(doc.rootFrame()->frameFormat().bottomMargin(), 6.0 + env.tokens.chatBottom);
        QVERIFY(layoutdoc::removeEndGap(&doc));
        QCOMPARE(doc.rootFrame()->frameFormat().bottomMargin(), 6.0);
        QCOMPARE(doc.rootFrame()->frameFormat().topMargin(), 6.0);
        QVERIFY(!doc.rootFrame()->frameFormat().hasProperty(layoutformat::kFrameOurs));
    }

    // ---- 4. hit tests, copying, keys and replies on blocks moved to the right ---------------------------------------
    void hitTestsAndReadersOnTheRight()
    {
        QTextDocument  plain, doc;
        const Ids      ids = fillChat(plain);
        fillChat(doc);
        const QMap<int, QString> keys = keysOf(plain);
        const layoutdoc::Env     env  = envFor(Mode::Cozy, true, true);
        const QString            text = ChatEmoji::originalText(&plain, 0, plain.characterCount() - 1);
        textreactdoc::insertRow(&doc, ids.shortOwn, textreactdoc::objectPrefix() + QStringLiteral("o1"), keys.value(ids.shortOwn), QSizeF(96, 26));
        textreactdoc::insertRow(&plain, ids.shortOwn, textreactdoc::objectPrefix() + QStringLiteral("o1"), keys.value(ids.shortOwn), QSizeF(96, 26));
        styleOwnRight(doc, env);
        QAbstractTextDocumentLayout* layout = doc.documentLayout();
        // Pictures (a card, a poll card, a reaction row's pills): where the modules find them is where they are.
        for (const auto& p : {qMakePair(ids.card, QStringLiteral("tsmedia:card")), qMakePair(ids.poll, polldoc::objectPrefix()),
                              qMakePair(ids.shortOwn, textreactdoc::objectPrefix())}) {
            const QTextBlock b  = doc.findBlockByNumber(p.first);
            const int        at = objectPosition(b, p.second);
            QVERIFY(at >= 0);
            // (The row is bottom-aligned on its line: ChatTextReactions' own rect.)
            const QRectF r = p.second == textreactdoc::objectPrefix() ? textreactdoc::rowRect(b) : pictureRect(doc, at);
            QVERIFY(!r.isEmpty());
            QVERIFY(layoutbubbles::itemOf(b, env).rect.contains(r));
            QCOMPARE(layout->hitTest(QPointF(r.left() + 3, r.center().y()), Qt::ExactHit), at);
            const int right = layout->hitTest(QPointF(r.right() - 3, r.center().y()), Qt::ExactHit);
            QVERIFY(right == at || right == at + 1); // on the picture still
        }
        // A link: its text under the pointer is the link.
        {
            const QTextBlock b = doc.findBlockByNumber(ids.link);
            int              at = -1;
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                if (it.fragment().charFormat().anchorHref().startsWith(QLatin1String("https://")))
                    at = it.fragment().position() + 3;
            }
            QVERIFY(at > 0);
            const QTextLine line = b.layout()->lineForTextPosition(at - b.position());
            const QPointF   origin = layout->blockBoundingRect(b).topLeft();
            const QPointF   point(origin.x() + line.cursorToX(at - b.position()) + 2, origin.y() + line.y() + line.height() / 2);
            QCOMPARE(layout->anchorAt(point), QStringLiteral("https://example.com/maps/harbor-night"));
            QVERIFY(layoutbubbles::itemOf(b, env).rect.contains(point));
        }
        // Right-to-left text: a point on its last line hits that line.
        {
            const QTextBlock b    = doc.findBlockByNumber(ids.rtlLong);
            const QRectF     last = lineRects(b).last();
            const int        hit  = layout->hitTest(last.center(), Qt::ExactHit);
            QVERIFY(hit >= b.position() + b.layout()->lineAt(b.layout()->lineCount() - 1).textStart());
            QVERIFY(hit <= b.position() + b.length());
        }
        // Copying gives TeamSpeak's text, the reply line still finds its original, every key stays.
        QCOMPARE(ChatEmoji::originalText(&doc, 0, doc.characterCount() - 1), ChatEmoji::originalText(&plain, 0, plain.characterCount() - 1));
        QVERIFY(text.contains(QStringLiteral("\"Sam\": Yes! I'll bring the new map")));
        QCOMPARE(keysOf(doc), keysOf(plain));
        const replydoc::Message a = replydoc::parseBlock(plain.findBlockByNumber(ids.reply));
        const replydoc::Message b = replydoc::parseBlock(doc.findBlockByNumber(ids.reply));
        QCOMPARE(b.text, a.text);
        QCOMPARE(b.nick, a.nick);
        QCOMPARE(b.minutes, a.minutes);
        QVERIFY(b.hasQuote);
        QCOMPARE(replydoc::scan(&doc).size(), replydoc::scan(&plain).size());
        // Selecting your message's text selects its text.
        {
            const QTextBlock bl = doc.findBlockByNumber(ids.shortOwn);
            QTextCursor      c(&doc);
            c.setPosition(layoutformat::bodyStart(bl));
            c.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
            QCOMPARE(c.selectedText(), QStringLiteral("Yes"));
        }
    }

    // ---- 5. the action bar --------------------------------------------------------------------------------------------
    void actionBarOnTheLeftOfYours()
    {
        const QSize bar(118, 34);
        const QRect area(0, 0, 600, 400);
        // Beside the top left corner of a bubble on the right (as beside the top right one of others').
        const QRect bubble(400, 100, 180, 30);
        const QRect r = layoutbubbles::barPlace(bar, area, bubble, {}, 12, 10, true);
        QCOMPARE(r.right() + 6, bubble.left());
        QCOMPARE(r.top(), 100 - 2);
        // The mirror of the other side, whatever is in the way.
        const auto mirror = [&](const QRect& q) { return QRect(area.left() + area.right() - q.right(), q.top(), q.width(), q.height()); };
        for (int i = 0; i < 40; ++i) {
            const QRect         b(60 + (i * 37) % 300, 30 + (i * 53) % 300, 60 + (i * 29) % 240, 18 + (i * 11) % 60);
            const QVector<QRect> keep = {QRect(b.left() - 40, b.top() - 30, 120, 20), QRect(b.right() - 30, b.bottom() + 4, 80, 18)};
            QVector<QRect>       mirrored;
            for (const QRect& k : keep)
                mirrored << mirror(k);
            QCOMPARE(layoutbubbles::barPlace(bar, area, b, keep, 12, 10, true), mirror(layoutbubbles::barPlace(bar, area, mirror(b), mirrored, 12, 10, false)));
        }
        // On a chat: never over a text, on the left of yours, on the right of others'.
        QTextDocument  doc;
        const Ids      ids = fillChat(doc);
        layoutdoc::Env env = envFor(Mode::Cozy, true, true);
        styleOwnRight(doc, env);
        for (const int n : ids.own() + QVector<int>{ids.alice, ids.bob}) {
            const layoutbubbles::Item item = layoutbubbles::itemOf(doc.findBlockByNumber(n), env);
            const QRect               view(0, qRound(item.rect.top()) - 150, 520, 400);
            QVector<QRect>            keep;
            for (const QRectF& k : layoutbubbles::keepOutNear(&doc, n, bar.height() + 8, env))
                keep << k.toAlignedRect();
            const QRect place = layoutbubbles::barPlace(bar, view, item.rect.toAlignedRect(), keep, 12, env.tokens.bubblePadX, item.right);
            // Beside the bubble when there is room (left of yours, right of others'): never over a text then.
            const bool room = item.right ? item.rect.left() - 6 - bar.width() >= view.left() + 12 : item.rect.right() + 6 + bar.width() <= view.right() - 12;
            for (const QRect& k : keep)
                QVERIFY2(!room || !place.intersects(k), qPrintable(QStringLiteral("block %1: bar %2,%3 %4x%5 over %6,%7 %8x%9 (bubble %10,%11 %12x%13)")
                                                              .arg(n).arg(place.x()).arg(place.y()).arg(place.width()).arg(place.height()).arg(k.x()).arg(k.y()).arg(k.width()).arg(k.height())
                                                              .arg(item.rect.x()).arg(item.rect.y()).arg(item.rect.width()).arg(item.rect.height())));
            if (item.right)
                QVERIFY(place.center().x() < item.rect.center().x());
            else
                QVERIFY(place.center().x() > item.rect.left() + 40);
        }
        // The time of yours is kept clear too.
        const layoutdoc::OwnPlace place = layoutdoc::ownPlace(doc.findBlockByNumber(ids.shortOwn), env);
        QVERIFY(layoutbubbles::keepOut(&doc, ids.shortOwn, ids.shortOwn, env).contains(place.timeRect));
    }

    // ---- 6. the setting ---------------------------------------------------------------------------------------------
    void theSetting()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.path() + QStringLiteral("/settings.ini");
        Settings      s;
        QVERIFY(s.chatOwnRight); // on by default
        s.chatOwnRight = false;
        s.save(file);
        Settings loaded;
        loaded.load(file);
        QVERIFY(!loaded.chatOwnRight);
        loaded.chatOwnRight = true;
        loaded.save(file);
        Settings again;
        again.chatOwnRight = false;
        again.load(file);
        QVERIFY(again.chatOwnRight);
        // Settings > Chat layout: only for Cozy with bubbles.
        ChatLayoutSection section;
        QCheckBox*        box = nullptr;
        QCheckBox*        bubbles = nullptr;
        for (QCheckBox* c : section.findChildren<QCheckBox*>()) {
            if (c->text() == QStringLiteral("Show &my messages on the right"))
                box = c;
            if (c->text().contains(QStringLiteral("b&ubbles")))
                bubbles = c;
        }
        QVERIFY(box && bubbles);
        Settings cozy;
        section.load(cozy);
        QVERIFY(box->isChecked() && box->isEnabled());
        bubbles->setChecked(false);
        QVERIFY(!box->isEnabled());
        bubbles->setChecked(true);
        QVERIFY(box->isEnabled());
        box->setChecked(false);
        Settings stored = cozy;
        section.store(stored, cozy);
        QVERIFY(!stored.chatOwnRight);
        Settings compact;
        compact.chatLayout = 2;
        section.load(compact);
        QVERIFY(!box->isEnabled());
        QVERIFY(box->accessibleDescription().contains(QStringLiteral("Cozy with bubbles")));
    }

    // ---- 7. 4000 blocks -------------------------------------------------------------------------------------------------
    void fourThousandBlocks()
    {
        const auto fill = [](QTextDocument& doc) {
            setup(doc);
            const tschat::Skin s = tschat::darkSkin();
            for (int i = 0; i < 4000; ++i) {
                const QString time = QStringLiteral("%1:%2:%3").arg(10 + i / 600, 2, 10, QLatin1Char('0')).arg((i / 10) % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
                if (i % 9 == 0)
                    tschat::append(&doc, tschat::systemHtml(s, time, QStringLiteral("Bob connected")));
                else if (i % 2)
                    tschat::append(&doc, tschat::messageHtml(s, time, 7, kMe, QStringLiteral("Sam"),
                                                             i % 5 == 1 ? tschat::escaped(rtlShort()) : QStringLiteral("my message %1 with a few more words to wrap").arg(i), true));
                else
                    tschat::append(&doc, tschat::messageHtml(s, time, 17, kAlice, QStringLiteral("Alice"), tschat::linkHtml(s, QStringLiteral("https://example.com/%1").arg(i))));
            }
            doc.documentLayout()->documentSize();
        };
        qint64 ms[2] = {0, 0};
        for (const bool ownRight : {false, true}) {
            QTextDocument doc;
            fill(doc);
            const Snapshot before = snapshot(doc);
            QElapsedTimer  clock;
            clock.start();
            const layoutdoc::Env env = envFor(Mode::Cozy, true, ownRight);
            QCOMPARE(layoutdoc::styleAll(&doc, env), 4000);
            layoutdoc::applyEndGap(&doc, env);
            doc.documentLayout()->documentSize();
            ms[ownRight ? 1 : 0] = clock.elapsed();
            if (ownRight) {
                int fitted = 0;
                for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
                    fitted += layoutformat::isOwnRight(b) && b.blockFormat().doubleProperty(layoutformat::kOwnFit) >= 0 ? 1 : 0;
                QVERIFY(fitted > 1700);
                // What ChatLayout does per paint: the shapes of a screen of blocks (yours with their times).
                QElapsedTimer shapes;
                shapes.start();
                int count = 0;
                for (int round = 0; round < 20; ++round)
                    count += layoutbubbles::itemsIn(&doc, 3000 + round, 3040 + round, env).size();
                QVERIFY(count >= 20 * 35);
                qInfo("20 screens of shapes, yours on the right: %lld ms", shapes.elapsed());
                QVERIFY2(shapes.elapsed() < 200, qPrintable(QString::number(shapes.elapsed())));
                // A new line of yours on it: what ChatLayout does (style, settle, the end's room), one line's work.
                tschat::append(&doc, tschat::messageHtml(tschat::darkSkin(), QStringLiteral("23:59:00"), 7, kMe, QStringLiteral("Sam"), QStringLiteral("a new one"), true));
                doc.documentLayout()->documentSize();
                QElapsedTimer one;
                one.start();
                const int n = doc.blockCount() - 1;
                {
                    QTextCursor batch(&doc);
                    batch.beginEditBlock();
                    layoutdoc::sanitize(&doc, n);
                    const QTextBlock b = doc.findBlockByNumber(n);
                    QVERIFY(layoutdoc::apply(&doc, n, layoutdoc::plan(env, b, layoutdoc::read(b), layoutdoc::previousOf(b), QDate(2026, 10, 10), 0, 99999), env));
                    batch.endEditBlock();
                }
                layoutdoc::settle(&doc, {n}, env);
                layoutdoc::applyEndGap(&doc, env);
                doc.documentLayout()->documentSize();
                const qint64 us = one.nsecsElapsed() / 1000;
                qInfo("a new line of yours: %lld us", us);
                QVERIFY2(us < 5000, qPrintable(QString::number(us)));
                QVERIFY(layoutbubbles::itemOf(doc.findBlockByNumber(n), env).right);
            }
            QElapsedTimer clock2;
            clock2.start();
            layoutdoc::removeEndGap(&doc);
            layoutdoc::restoreAll(&doc);
            doc.documentLayout()->documentSize();
            qInfo("4000 blocks, own on the right %d: styled in %lld ms, restored in %lld ms", ownRight ? 1 : 0, ms[ownRight ? 1 : 0], clock2.elapsed());
            if (!ownRight)
                QCOMPARE(compare(before, snapshot(doc)), QString());
        }
        QVERIFY2(ms[1] < 3000, qPrintable(QString::number(ms[1])));
        // Comparable to 2.3.0's look (the same layout's work, the fit's second pass on half of the blocks).
        QVERIFY2(ms[1] <= 2 * ms[0] + 300, qPrintable(QStringLiteral("%1 vs %2 ms").arg(ms[1]).arg(ms[0])));
    }

    // ---- 8. the chat without its scroll bar ---------------------------------------------------------------------------
    // The look's width is the chat's with its scroll bar (ChatIntegration::previewAreaWidth); while the bar is hidden
    // the document is wider. Every bubble of yours still ends at the same x (right-to-left ones too), no line of yours
    // reaches past it once fitted, and the bar coming or going moves none of them and breaks no line again
    // (alignOwnRight).
    void sameEdgeWithoutTheScrollBar()
    {
        constexpr int bar = 14;
        for (const int width : {300, 520, 900}) {
            QTextDocument plain, doc;
            const Ids     ids = fillChat(plain, width);
            fillChat(doc, width);
            // A reaction row under the long one (its lines wrap in the narrow chat).
            const QMap<int, QString> keys = keysOf(plain);
            for (QTextDocument* d : {&plain, &doc})
                textreactdoc::insertRow(d, ids.longOwn, textreactdoc::objectPrefix() + QStringLiteral("o1"), keys.value(ids.longOwn), QSizeF(126, 26));
            const Snapshot       before = snapshot(doc);
            const layoutdoc::Env env    = envFor(Mode::Cozy, true, true, width - 8 - bar); // the bar hidden: 14 px more
            styleOwnRight(doc, env);
            const qreal R     = layoutdoc::ownRightEdge(env) + doc.documentMargin();
            const auto  edges = [&](const QString& when) {
                for (const int n : ids.own()) {
                    const QTextBlock          b    = doc.findBlockByNumber(n);
                    const layoutbubbles::Item item = layoutbubbles::itemOf(b, env);
                    const QString what = QStringLiteral("width %1, %2, block %3: %4 vs %5").arg(width).arg(when).arg(n).arg(item.rect.right()).arg(R + env.tokens.bubblePadX);
                    QVERIFY2(item.right && item.rect.right() == R + env.tokens.bubblePadX, qPrintable(what));
                    const bool rtl = b.blockFormat().alignment() & Qt::AlignRight;
                    for (const QRectF& r : lineRects(b)) {
                        QVERIFY2(r.right() <= R + env.tokens.ownSlack + 0.5, qPrintable(what + QStringLiteral(": a line ends at %1").arg(r.right())));
                        QVERIFY2(!rtl || qAbs(r.right() - R) <= 1.5, qPrintable(what + QStringLiteral(": a right-to-left line ends at %1").arg(r.right())));
                    }
                }
            };
            const auto breaks = [&] {
                QVector<int> out;
                for (const int n : ids.own()) {
                    const QTextLayout* tl = doc.findBlockByNumber(n).layout();
                    out << -1;
                    for (int i = 0; i < tl->lineCount(); ++i)
                        out << tl->lineAt(i).textStart();
                }
                return out;
            };
            edges(QStringLiteral("styled without the bar"));
            const QVector<int> lines = breaks();
            QCOMPARE(layoutdoc::alignOwnRight(&doc, env), 0); // nothing to do
            // The bar comes (the chat gets as narrow as the look's width), then goes again.
            for (const int w : {width - bar, width}) {
                doc.setTextWidth(w);
                doc.documentLayout()->documentSize();
                QCOMPARE(layoutdoc::alignOwnRight(&doc, env), ids.own().size());
                doc.documentLayout()->documentSize();
                edges(QStringLiteral("the chat %1 wide").arg(w));
                QCOMPARE(breaks(), lines);
                QCOMPARE(layoutdoc::alignOwnRight(&doc, env), 0);
                const QVector<int> own = ids.own();
                QCOMPARE(layoutdoc::settle(&doc, {}, env, QSet<int>(own.cbegin(), own.cend())), 0); // fitted still
            }
            // With the bar shown the margins are the ones of every look (the bubble's room, and the slack).
            doc.setTextWidth(width - bar);
            QCOMPARE(layoutdoc::ownRightMargin(&doc, env, false), qreal(env.tokens.bubbleRight));
            QCOMPARE(layoutdoc::ownRightMargin(&doc, env, true), qreal(env.tokens.bubbleRight + env.tokens.ownSlack));
            // Given back exactly.
            doc.setTextWidth(width);
            giveBackLayout(doc);
            QCOMPARE(compare(before, snapshot(doc)), QString());
            QCOMPARE(compare(snapshot(plain), snapshot(doc)), QString());
        }
    }

  private:
    static QString item0Time(const QTextBlock& b) { return layoutart::ownTimeText(layoutdoc::read(b).time, false); }
};

TSMEDIA_REGISTER_TEST(TestOwnRight)

#include "tst_ownright.moc"
