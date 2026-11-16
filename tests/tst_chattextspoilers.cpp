// 2.7 F1: text spoilers in TeamSpeak-like chats (tests/tschat.h; made-up people and links only).
//  * textspoilerdoc: hiding leaves every reader of the message's text (replydoc::parseBlock, the text-reaction key, the
//    sender's link-preview identity) as it was; links inside are no links until revealed; lines keep their height;
//    copies say "(spoiler)"; quote lines and TS Media file links are left alone; every object is its own fragment (equal
//    words never merge, critique H3); restoreAll gives TeamSpeak's text back exactly; the bars follow the chat font;
//    their colour is >= 3:1 on every skin; long spoilers wrap.
//  * ChatTextSpoilers on a QTextBrowser chat: hidden by the 0 ms pass before any paint (critique H2; a caption's
//    spoiler too, the media link after it untouched), clicks reveal one spoiler, the menu's Reveal, the setting on and
//    off, a document swap, the reader's place, unload.

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QtTest>

#include "chatemoji.h"
#include "chatscroll.h"
#include "chattextspoilers.h"
#include "emojiformat.h"
#include "linkpreview/linkdoc.h"
#include "linkpreview/lpmessage.h"
#include "replies.h"
#include "replydoc.h"
#include "settings.h"
#include "testmain.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "textspoiler.h"
#include "textspoilerdoc.h"
#include "tschat.h"
#include "uiutil.h"

namespace {

const QString kAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");

QFont chatFont(qreal points = 9.0)
{
    QFont f(QStringLiteral("Segoe UI"));
    f.setPointSizeF(points);
    return f;
}

void setup(QTextDocument& doc, int width = 520)
{
    doc.setDefaultFont(chatFont());
    doc.setTextWidth(width);
}

void message(QTextDocument* doc, const tschat::Skin& s, const QString& bodyHtml, const QString& uid = kAlice, const QString& nick = QStringLiteral("Alice"))
{
    tschat::append(doc, tschat::messageHtml(s, QStringLiteral("20:01:00"), 17, uid, nick, bodyHtml));
}

// Every block's raw pairs hidden (revealed: straight into the revealed state), as ChatTextSpoilers' pass does it.
int hideAll(QTextDocument* doc, bool revealed = false, int* serial = nullptr)
{
    int  own = 0;
    int* s   = serial ? serial : &own;
    int  n   = 0;
    for (int b = doc->blockCount() - 1; b >= 0; --b) {
        const QTextBlock        block = doc->findBlockByNumber(b);
        const replydoc::Message m     = replydoc::parseBlock(block);
        if (m.block < 0)
            continue;
        const QVector<textspoilerdoc::Pair> pairs = textspoilerdoc::rawPairs(block, m.textStart);
        QVector<QString>                    keys;
        QVector<bool>                       open;
        for (int i = 0; i < pairs.size(); ++i) {
            keys << QStringLiteral("k%1:%2").arg(b).arg(i);
            open << revealed;
        }
        n += textspoilerdoc::hide(doc, pairs, keys, open, s);
    }
    return n;
}

// TeamSpeak's document as a reader sees it: the text and the runs of equal formats (fragment boundaries inside a run of
// one format don't matter to anyone).
QStringList snapshot(QTextDocument* doc)
{
    QStringList out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        QString         run;
        QTextCharFormat format;
        bool            any = false;
        auto            flush = [&] {
            if (!any)
                return;
            QStringList props;
            const QMap<int, QVariant> map = format.properties();
            for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
                if (it.key() == QTextFormat::ObjectIndex)
                    continue;
                props << QStringLiteral("%1=%2").arg(it.key()).arg(it.value().toString());
            }
            out << run + QStringLiteral(" {") + props.join(QLatin1Char(',')) + QLatin1Char('}');
            run.clear();
            any = false;
        };
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            if (any && !(f.charFormat() == format))
                flush();
            format = f.charFormat();
            run += f.text();
            any = true;
        }
        flush();
        out << QStringLiteral("--");
    }
    return out;
}

int rawPairCount(QTextDocument* doc)
{
    int n = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const replydoc::Message m = replydoc::parseBlock(b);
        if (m.block >= 0)
            n += textspoilerdoc::rawPairs(b, m.textStart).size();
    }
    return n;
}

QString blockText(QTextDocument* doc, int block)
{
    return replydoc::parseBlock(doc->findBlockByNumber(block)).text;
}

QString keyOf(QTextDocument* doc, int block)
{
    return textreactdoc::identify(doc->findBlockByNumber(block)).hash;
}

// One object per fragment (critique H3).
bool objectsApart(QTextDocument* doc)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && textspoilerdoc::isObject(f.charFormat()) && f.length() != 1)
                return false;
        }
    }
    return true;
}

QString copyOf(QTextDocument* doc, int block)
{
    const QTextBlock b = doc->findBlockByNumber(block);
    return ChatEmoji::originalText(doc, b.position(), b.position() + b.length() - 1);
}

// A chat widget like TeamSpeak's, laid out (the offscreen platform), with the module on it.
struct Chat {
    QTextBrowser      browser;
    ChatTextSpoilers* spoilers = nullptr;
    int               edits    = 0;
    bool              mutating = false;
    QList<int>        hid; // blocks the passes reported (the chat layout restyles them)

    explicit Chat(const tschat::Skin& skin = tschat::darkSkin(), int width = 560, int height = 300)
    {
        browser.resize(width, height);
        setup(*browser.document(), width - 40);
        if (skin.dark) {
            QPalette pal = browser.palette();
            pal.setColor(QPalette::Base, QColor(0x2f, 0x31, 0x36));
            pal.setColor(QPalette::Text, QColor(0xdc, 0xdd, 0xde));
            browser.setPalette(pal);
        }
        browser.show();
    }
    ~Chat() { delete spoilers; }
    void attach()
    {
        ChatTextSpoilers::Host host;
        host.othersMutating = [this] { return mutating; };
        host.setMutating    = [this](bool on) {
            const bool was = mutating;
            mutating       = on;
            return was;
        };
        host.edited = [this](QTextBrowser*) { ++edits; };
        host.hid    = [this](QTextBrowser*, int block) { hid << block; };
        spoilers    = new ChatTextSpoilers(host);
        spoilers->attach(&browser);
        spoilers->runEarlyPass();
        settle();
    }
    void settle()
    {
        browser.document()->documentLayout()->documentSize();
        chatscroll::syncRange(&browser);
    }
    QTextDocument* doc() { return browser.document(); }
    QPoint         centerOf(const QRect& r) const { return r.center(); }
    bool           click(const QPoint& pos)
    {
        QMouseEvent press(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        const bool  a = spoilers->filterEvent(browser.viewport(), &press);
        const bool  b = spoilers->filterEvent(browser.viewport(), &release);
        return a && b;
    }
};

QVector<textspoilerdoc::Spoiler> all(QTextDocument* doc)
{
    return textspoilerdoc::spoilersIn(doc);
}

} // namespace

class TestChatTextSpoilers : public QObject
{
    Q_OBJECT

  private:
    bool m_reveal = false;

  private slots:
    void initTestCase() { m_reveal = Settings::instance().revealSpoilers; }
    void cleanupTestCase() { Settings::instance().revealSpoilers = m_reveal; }
    void init() { Settings::instance().revealSpoilers = false; }

    // ---- textspoilerdoc ----------------------------------------------------------------------------------------

    void hidingKeepsEveryReader_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void hidingKeepsEveryReader()
    {
        QFETCH(bool, dark);
        const tschat::Skin s = dark ? tschat::darkSkin() : tschat::lightSkin();
        QTextDocument      doc;
        setup(doc);
        // As TeamSpeak shows what was sent: B/I/U rendered, the markers and the text as typed, the link an anchor.
        const QString sent = QStringLiteral("x [B]bold[/B] [I]it[/I] [U]u[/U] [spoiler]secret [b]one[/b] [URL]https://example.com/a.png[/URL] \U0001F600 :)[/spoiler] y");
        message(&doc, s,
                QStringLiteral("x <b>bold</b> <i>it</i> <u>u</u> [spoiler]secret <b>one</b> ") + tschat::linkHtml(s, QStringLiteral("https://example.com/a.png"))
                    + QStringLiteral(" \U0001F600 <img src=\"emoticons:smile.svg\" width=\"16\" height=\"16\">[/spoiler] y"));
        message(&doc, s, QStringLiteral("[spoiler]a[/spoiler] b [SPOILER]c d[/Spoiler]"), kBob, QStringLiteral("Bob"));
        ChatEmoji::processDocument(&doc, 1.0); // HD emoji in the spoiler's text too
        const QString text0 = blockText(&doc, 0);
        const QString key0  = keyOf(&doc, 0);
        const QString text1 = blockText(&doc, 1);
        const QString key1  = keyOf(&doc, 1);
        QVERIFY(!key0.isEmpty());
        QVERIFY2(text0.contains(QLatin1String("[spoiler]secret one https://example.com/a.png")), qPrintable(text0));
        // The sender's identity reads the same message.
        QCOMPARE(key0, linkpreview::message::identityHash(kAlice, sent));
        QCOMPARE(hideAll(&doc), 3);
        QCOMPARE(rawPairCount(&doc), 0);
        QCOMPARE(blockText(&doc, 0), text0);
        QCOMPARE(keyOf(&doc, 0), key0);
        QCOMPARE(blockText(&doc, 1), text1);
        QCOMPARE(keyOf(&doc, 1), key1);
        QVERIFY(objectsApart(&doc));
        // The hidden text is not in the document's text at all.
        QVERIFY(!doc.toPlainText().contains(QLatin1String("secret")));
        QVERIFY(!doc.toPlainText().contains(QLatin1String("[spoiler]")));
        // Revealed: the same again.
        for (const textspoilerdoc::Spoiler& sp : all(&doc))
            QVERIFY(textspoilerdoc::reveal(&doc, sp));
        ChatEmoji::processDocument(&doc, 1.0);
        QCOMPARE(blockText(&doc, 0), text0);
        QCOMPARE(keyOf(&doc, 0), key0);
        QCOMPARE(keyOf(&doc, 1), key1);
        QVERIFY(doc.toPlainText().contains(QLatin1String("secret")));
    }

    // Identity across the sides: the sender's lpmessage::identityHash equals the receiver's key hidden, revealed, raw.
    void identityAcrossSides()
    {
        const tschat::Skin s = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        const QString sent = QStringLiteral("look [B]here[/B] [spoiler]the [I]end[/I] [URL]https://example.org/x[/URL] \U0001F389[/spoiler] [U]ok[/U]");
        message(&doc, s, QStringLiteral("look <b>here</b> [spoiler]the <i>end</i> ") + tschat::linkHtml(s, QStringLiteral("https://example.org/x"))
                             + QStringLiteral(" \U0001F389[/spoiler] <u>ok</u>"));
        const QString hash = linkpreview::message::identityHash(kAlice, sent);
        QCOMPARE(keyOf(&doc, 0), hash);
        hideAll(&doc);
        QCOMPARE(keyOf(&doc, 0), hash);
        textspoilerdoc::reveal(&doc, all(&doc).value(0));
        QCOMPARE(keyOf(&doc, 0), hash);
        textspoilerdoc::restoreAll(&doc);
        QCOMPARE(keyOf(&doc, 0), hash);
    }

    void linksInsideAreNoLinks()
    {
        const tschat::Skin s = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        message(&doc, s, QStringLiteral("[spoiler]see ") + tschat::linkHtml(s, QStringLiteral("https://example.com/page"), QStringLiteral("two words"))
                             + QStringLiteral(" and ") + tschat::linkHtml(s, QStringLiteral("https://example.com/a.png")) + QStringLiteral("[/spoiler] ")
                             + tschat::linkHtml(s, QStringLiteral("https://example.org/out")));
        QCOMPARE(linkdoc::findLinks(&doc).size(), 3);
        hideAll(&doc);
        const QVector<linkdoc::Link> hidden = linkdoc::findLinks(&doc);
        QCOMPARE(hidden.size(), 1);
        QCOMPARE(hidden.at(0).href, QStringLiteral("https://example.org/out"));
        // Not even the space of a link inside (it would be an anchor with the address).
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                QVERIFY(!f.charFormat().isAnchor() || f.charFormat().anchorHref() == QLatin1String("https://example.org/out")
                        || f.charFormat().anchorHref().startsWith(QLatin1String("client://")));
            }
        }
        textspoilerdoc::reveal(&doc, all(&doc).value(0));
        QCOMPARE(linkdoc::findLinks(&doc).size(), 3);
    }

    // Review fix: TeamSpeak's own address wrapping can take "[/spoiler]" and the comma after it into one link (a
    // selection followed by punctuation, or tags typed by hand), or an address can run into "[spoiler]". The parts of
    // that link outside the pair lose the link while the spoiler is hidden: no tool tip with the hidden address, no
    // click, no preview; Reveal and unload give the link back whole.
    void linkAcrossAMarkerIsNoLink()
    {
        const tschat::Skin s = tschat::darkSkin();
        for (const QString& href : {QStringLiteral("https://example.com/a.png[/spoiler],"), QStringLiteral("https://example.com/a.png%5B/spoiler%5D,")}) {
            QTextDocument doc;
            setup(doc);
            message(&doc, s, QStringLiteral("[spoiler]look ") + tschat::linkHtml(s, href, QStringLiteral("https://example.com/a.png[/spoiler],")) + QStringLiteral(" ok"));
            const QStringList before = snapshot(&doc);
            QCOMPARE(linkdoc::findLinks(&doc).size(), 1);
            QCOMPARE(hideAll(&doc), 1);
            QVERIFY2(linkdoc::findLinks(&doc).isEmpty(), qPrintable(href));
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat cf = it.fragment().charFormat();
                    QVERIFY2(!cf.isAnchor() || cf.anchorHref().startsWith(QLatin1String("client://")), qPrintable(cf.anchorHref()));
                    QVERIFY(!cf.toolTip().contains(QLatin1String("example.com")));
                }
            }
            QCOMPARE(copyOf(&doc, 0).right(QStringLiteral("(spoiler), ok").size()), QStringLiteral("(spoiler), ok"));
            // Reveal: the link back (the comma untinted and linked again).
            textspoilerdoc::reveal(&doc, all(&doc).value(0));
            const QVector<linkdoc::Link> shown = linkdoc::findLinks(&doc);
            QVERIFY(!shown.isEmpty());
            for (const linkdoc::Link& l : shown)
                QCOMPARE(l.href, href);
            bool comma = false;
            for (auto it = doc.firstBlock().begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                if (f.text() == QLatin1String(",")) {
                    comma = true;
                    QVERIFY(f.charFormat().isAnchor());
                    QCOMPARE(textspoilerdoc::kindOf(f.charFormat()), static_cast<int>(textspoilerdoc::None));
                }
            }
            QVERIFY(comma);
            // Unhide and unload: TeamSpeak's text exactly.
            textspoilerdoc::unhide(&doc, all(&doc).value(0));
            QCOMPARE(snapshot(&doc), before);
            hideAll(&doc);
            QVERIFY(linkdoc::findLinks(&doc).isEmpty());
            textspoilerdoc::restoreAll(&doc);
            QCOMPARE(snapshot(&doc), before);
        }
        // An address that runs into "[spoiler]": its visible start loses the link too.
        QTextDocument doc;
        setup(doc);
        message(&doc, s, tschat::linkHtml(s, QStringLiteral("https://example.com/x[spoiler]secret[/spoiler]")) + QStringLiteral(" ok"));
        const QStringList before = snapshot(&doc);
        QCOMPARE(hideAll(&doc), 1);
        QVERIFY(linkdoc::findLinks(&doc).isEmpty());
        textspoilerdoc::restoreAll(&doc);
        QCOMPARE(snapshot(&doc), before);
        // A link that only follows the spoiler (a space between) keeps its link.
        QTextDocument apart;
        setup(apart);
        message(&apart, s, QStringLiteral("[spoiler]x[/spoiler] ") + tschat::linkHtml(s, QStringLiteral("https://example.org/out")));
        hideAll(&apart);
        QCOMPARE(linkdoc::findLinks(&apart).size(), 1);
    }

    void lineHeightsStay()
    {
        const tschat::Skin s = tschat::lightSkin();
        QTextDocument      doc;
        setup(doc, 4000); // one line, hidden or not
        message(&doc, s, QStringLiteral("one line [spoiler]with a secret[/spoiler] in it"));
        textspoilerdoc::Handler handler; // as in a chat: the bars have their size
        doc.documentLayout()->registerHandler(textspoilerdoc::kObjectType, &handler);
        const qreal before = doc.documentLayout()->blockBoundingRect(doc.firstBlock()).height();
        QCOMPARE(doc.firstBlock().layout()->lineCount(), 1);
        hideAll(&doc);
        QCOMPARE(doc.documentLayout()->blockBoundingRect(doc.firstBlock()).height(), before);
        QCOMPARE(doc.firstBlock().layout()->lineCount(), 1);
        textspoilerdoc::reveal(&doc, all(&doc).value(0));
        QCOMPARE(doc.documentLayout()->blockBoundingRect(doc.firstBlock()).height(), before);
        doc.documentLayout()->unregisterHandler(textspoilerdoc::kObjectType, &handler);
    }

    void copiesSayPlaceholder()
    {
        const tschat::Skin s = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        message(&doc, s, QStringLiteral("x [spoiler]the secret words[/spoiler] y"));
        hideAll(&doc);
        const QString hidden = copyOf(&doc, 0);
        QVERIFY2(hidden.endsWith(QLatin1String("x (spoiler) y")), qPrintable(hidden));
        QCOMPARE(hidden.count(QLatin1String("(spoiler)")), 1);
        textspoilerdoc::reveal(&doc, all(&doc).value(0));
        const QString shown = copyOf(&doc, 0);
        QVERIFY2(shown.endsWith(QLatin1String("x [spoiler]the secret words[/spoiler] y")), qPrintable(shown));
    }

    // Review fix: a spoiler that runs past what the chat's reader keeps of a message (replies::kMaxBodyChars) is still
    // "(spoiler)" in the reply rows, the reply bar and the sent quote, although the text read ends inside it.
    void longSpoilerStaysMasked()
    {
        const tschat::Skin s = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        QString secret;
        for (int i = 0; secret.size() < 4500; ++i)
            secret += QStringLiteral("secret%1 ").arg(i);
        message(&doc, s, QStringLiteral("Ending: [spoiler]") + secret.trimmed() + QStringLiteral("[/spoiler] done"));
        QCOMPARE(hideAll(&doc), 1);
        const replydoc::Message m = replydoc::parseBlock(doc.firstBlock());
        QVERIFY(m.text.size() <= replies::kMaxBodyChars);
        QVERIFY(m.text.contains(QLatin1String("secret"))); // the reader keeps the text (identity), cut inside the spoiler
        const QString snippet = replies::makeSnippet(m.text, 200);
        QVERIFY2(!snippet.contains(QLatin1String("secret")), qPrintable(snippet));
        QCOMPARE(snippet, QStringLiteral("Ending: (spoiler)"));
        QVERIFY(!replies::makeSnippet(m.text, 160).contains(QLatin1String("secret"))); // the reply bar
        QVERIFY(!replies::makeSnippet(m.text, 280).contains(QLatin1String("secret"))); // the tool tip
        replies::Original o;
        o.nick    = QStringLiteral("Alice");
        o.uid     = kAlice;
        o.minutes = 20 * 60;
        o.text    = m.text;
        const QString quote = replies::quoteLine(o);
        QVERIFY2(!quote.contains(QLatin1String("secret")), qPrintable(quote));
        QVERIFY(quote.contains(QLatin1String("(spoiler)")));
        // The quote still finds its original (the other side masks the same way).
        const replies::Quote q = replies::parseQuoteBBCode(quote);
        QVERIFY(q.valid());
        QCOMPARE(q.snippet, snippet);
        // The same cut in plain text, and a closed spoiler stays as before.
        QCOMPARE(replies::makeSnippet(QStringLiteral("a [spoiler]b c d"), 200), QStringLiteral("a (spoiler)"));
        QCOMPARE(replies::makeSnippet(QStringLiteral("a [spoiler]b[/spoiler] c"), 200), QStringLiteral("a (spoiler) c"));
    }

    void quoteLinesAndFileLinksStay()
    {
        const tschat::Skin s = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        // A reply whose quote line (TS Media 2.6 quoted a spoiler) holds a pair: the quote line is left alone.
        replies::Original o;
        o.nick    = QStringLiteral("Bob");
        o.uid     = kBob;
        o.minutes = 20 * 60;
        o.text    = QStringLiteral("hi");
        const QString quote = replies::quoteLine(o).replace(QStringLiteral("“hi”"), QStringLiteral("“[spoiler]old[/spoiler]”"));
        QString       html  = tschat::escaped(quote);
        html.replace(QStringLiteral("[i]"), QStringLiteral("<i>")).replace(QStringLiteral("[/i]"), QStringLiteral("</i>"));
        html.replace(QRegularExpression(QStringLiteral("\\[URL=([^\\]]*)\\]")), QStringLiteral("<a href=\"\\1\">")).replace(QStringLiteral("[/URL]"), QStringLiteral("</a>"));
        message(&doc, s, html + QStringLiteral("<br>my [spoiler]answer[/spoiler]"));
        QVERIFY(replydoc::parseBlock(doc.firstBlock()).hasQuote);
        // A pair around a TS Media file link is no spoiler.
        message(&doc, s, QStringLiteral("[spoiler]") + tschat::linkHtml(s, QStringLiteral("ts3file://127.0.0.1?port=9987&channel=1&path=%2F&filename=a.png&size=10&isDir=0&serverUID=x&message=TS%20Media%20chat"), QStringLiteral("a.png"))
                             + QStringLiteral("[/spoiler]"));
        QCOMPARE(hideAll(&doc), 1);
        const QString text = doc.firstBlock().text();
        QVERIFY(text.contains(QLatin1String("[spoiler]old[/spoiler]")));
        QVERIFY(!text.contains(QLatin1String("answer")));
        QVERIFY(doc.lastBlock().text().contains(QLatin1String("[spoiler]")));
    }

    void restoreAllGivesTeamSpeaksTextBack()
    {
        const tschat::Skin s = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        message(&doc, s, QStringLiteral("x [spoiler]a <b>bold</b> ") + tschat::linkHtml(s, QStringLiteral("https://example.com/l i")) + QStringLiteral(" :)[/spoiler] y"));
        message(&doc, s, QStringLiteral("[spoiler]one[/spoiler] [spoiler]two three[/spoiler]"), kBob, QStringLiteral("Bob"));
        message(&doc, s, QStringLiteral("[spoiler]line one<br>line two[/spoiler]"));
        const QStringList before = snapshot(&doc);
        const QString     plain  = doc.toPlainText();
        QCOMPARE(hideAll(&doc), 4);
        QVERIFY(snapshot(&doc) != before);
        // One revealed, the rest hidden: all given back.
        textspoilerdoc::reveal(&doc, all(&doc).value(1));
        QCOMPARE(textspoilerdoc::restoreAll(&doc), 4);
        QCOMPARE(doc.toPlainText(), plain);
        QCOMPARE(snapshot(&doc), before);
        QCOMPARE(textspoilerdoc::restoreAll(&doc), 0);
    }

    // Critique H3: equal words (a long one-letter word in pieces of 12, "ha ha") are separate objects.
    void equalWordsStayApart()
    {
        const tschat::Skin s = tschat::lightSkin();
        QTextDocument      doc;
        setup(doc);
        const QStringList words = {QString(24, QLatin1Char('a')), QString(36, QLatin1Char('b')), QStringLiteral("ha ha"), QString(12, QLatin1Char('c')),
                                   QString(13, QLatin1Char('d'))};
        for (const QString& w : words)
            message(&doc, s, QStringLiteral("[spoiler]%1[/spoiler] and [spoiler]%1[/spoiler]").arg(w));
        QStringList texts, keys;
        for (int b = 0; b < doc.blockCount(); ++b) {
            texts << blockText(&doc, b);
            keys << keyOf(&doc, b);
        }
        const QStringList before = snapshot(&doc);
        hideAll(&doc);
        QVERIFY(objectsApart(&doc));
        for (int b = 0; b < doc.blockCount(); ++b) {
            QCOMPARE(blockText(&doc, b), texts.at(b));
            QCOMPARE(keyOf(&doc, b), keys.at(b));
            QCOMPARE(copyOf(&doc, b).count(QLatin1String("(spoiler)")), 2);
        }
        // 36 letters: three bars of 12.
        int bars = 0;
        for (auto it = doc.findBlockByNumber(1).begin(); !it.atEnd(); ++it)
            bars += textspoilerdoc::kindOf(it.fragment().charFormat()) == textspoilerdoc::Word ? 1 : 0;
        QCOMPARE(bars, 6);
        textspoilerdoc::restoreAll(&doc);
        QCOMPARE(snapshot(&doc), before);
    }

    void barsFollowTheChatFont()
    {
        const tschat::Skin s = tschat::lightSkin();
        QTextDocument      doc;
        setup(doc);
        message(&doc, s, QStringLiteral("[spoiler]wide words here[/spoiler]"));
        hideAll(&doc);
        QVector<QTextCharFormat> bars;
        for (auto it = doc.firstBlock().begin(); !it.atEnd(); ++it) {
            if (textspoilerdoc::kindOf(it.fragment().charFormat()) == textspoilerdoc::Word)
                bars << it.fragment().charFormat();
        }
        QCOMPARE(bars.size(), 3);
        const int revision = doc.revision();
        doc.setDefaultFont(chatFont(8.25));
        QVector<qreal> a;
        for (const QTextCharFormat& f : bars)
            a << textspoilerdoc::objectSize(&doc, f).width();
        doc.setDefaultFont(chatFont(12.25));
        for (int i = 0; i < bars.size(); ++i) {
            const qreal b = textspoilerdoc::objectSize(&doc, bars.at(i)).width();
            QVERIFY2(qAbs(b / a.at(i) - 12.25 / 8.25) < 0.12, qPrintable(QStringLiteral("%1 -> %2").arg(a.at(i)).arg(b)));
            QVERIFY(textspoilerdoc::objectSize(&doc, bars.at(i)).height() > 0);
        }
        QCOMPARE(doc.revision(), revision); // no edit: the layout asks the handler again
        // Markers take no room.
        QTextCharFormat open;
        open.setObjectType(textspoilerdoc::kObjectType);
        open.setProperty(textspoilerdoc::kKind, static_cast<int>(textspoilerdoc::Open));
        QCOMPARE(textspoilerdoc::objectSize(&doc, open), QSizeF(0, 0));
    }

    void barColourContrast()
    {
        const QVector<QPair<QColor, QColor>> skins = {{QColor(0xff, 0xff, 0xff), QColor(0, 0, 0)},
                                                      {QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde)},
                                                      {QColor(0x1e, 0x2a, 0x38), QColor(0xd8, 0xde, 0xe9)},
                                                      {QColor(0x12, 0x12, 0x12), QColor(0xe0, 0xe0, 0xe0)}};
        for (const auto& skin : skins) {
            textspoilerdoc::Look look;
            look.base = skin.first;
            look.text = skin.second;
            for (const int state : {0, 1, 2}) {
                look.hovered      = state == 1 ? 3 : -1;
                look.pressed      = state == 2 ? 3 : -1;
                const QColor bar  = textspoilerdoc::barColor(look, 3);
                const double cr   = ui::contrastRatio(bar, skin.first);
                QVERIFY2(cr >= 3.0, qPrintable(QStringLiteral("%1 on %2: %3").arg(bar.name(), skin.first.name()).arg(cr)));
            }
        }
        textspoilerdoc::Look look;
        look.base = Qt::white;
        look.text = Qt::black;
        QCOMPARE(textspoilerdoc::barColor(look, 1), QColor(0x80, 0x80, 0x80));
    }

    void longSpoilersWrap()
    {
        Chat chat(tschat::lightSkin(), 340, 400);
        QStringList many;
        for (int i = 0; i < 200; ++i)
            many << QStringLiteral("w%1").arg(i);
        message(chat.doc(), tschat::lightSkin(), QStringLiteral("[spoiler]%1[/spoiler]").arg(many.join(QLatin1Char(' '))));
        message(chat.doc(), tschat::lightSkin(), QStringLiteral("[spoiler]%1[/spoiler]").arg(QString(60, QLatin1Char('x'))));
        chat.attach();
        const QVector<QRect> bars = chat.spoilers->barRects(&chat.browser);
        QCOMPARE(bars.size(), 205);
        const int column = chat.browser.viewport()->width();
        QSet<int> lines;
        for (const QRect& r : bars) {
            QVERIFY2(r.right() <= column, qPrintable(QStringLiteral("%1 > %2").arg(r.right()).arg(column)));
            lines.insert(r.top());
        }
        QVERIFY(lines.size() > 10);
    }

    void layoutOfManyBarsIsQuick()
    {
        QTextDocument doc;
        setup(doc, 900);
        QStringList many;
        for (int i = 0; i < 2000; ++i)
            many << QStringLiteral("ab");
        message(&doc, tschat::darkSkin(), QStringLiteral("[spoiler]%1[/spoiler]").arg(many.join(QLatin1Char(' '))));
        hideAll(&doc);
        textspoilerdoc::Handler handler;
        doc.documentLayout()->registerHandler(textspoilerdoc::kObjectType, &handler);
        // Review fix: the clock runs over the whole relayout. setTextWidth itself lays the document out (QTextDocumentLayout::
        // documentChanged runs its first steps at once), so timing documentSize() alone measured nothing.
        QElapsedTimer clock;
        clock.start();
        doc.setTextWidth(901); // laid out again
        const qreal height = doc.documentLayout()->documentSize().height();
        const qint64 ms = clock.elapsed();
        QVERIFY2(ms < 50, qPrintable(QStringLiteral("%1 ms").arg(ms)));
        QVERIFY(height > 100); // 2000 bars on many lines, not nothing
        // A Ctrl + wheel notch: the chat's font one point larger, every bar measured again.
        clock.restart();
        doc.setDefaultFont(chatFont(10.0));
        const qreal zoomed = doc.documentLayout()->documentSize().height();
        const qint64 zoomMs = clock.elapsed();
        QVERIFY2(zoomMs < 50, qPrintable(QStringLiteral("%1 ms after a font change").arg(zoomMs)));
        QVERIFY(zoomed > height);
        doc.documentLayout()->unregisterHandler(textspoilerdoc::kObjectType, &handler);
    }

    // ---- ChatTextSpoilers on a chat -----------------------------------------------------------------------------

    // Critique H2: hidden by the 0 ms pass, before the 60 ms scan and before any paint.
    void earlyPassBeforePaint()
    {
        Chat chat;
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("hello"));
        chat.attach();
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("x [spoiler]short secret[/spoiler] y"));
        QVERIFY(chat.spoilers->earlyPending());
        QCOMPARE(rawPairCount(chat.doc()), 1);
        chat.spoilers->runEarlyPass(); // what the 0 ms timer runs (no paint has been processed)
        QCOMPARE(rawPairCount(chat.doc()), 0);
        QVERIFY(!chat.doc()->toPlainText().contains(QLatin1String("secret")));
        QVERIFY(chat.edits > 0);
        QVERIFY(!chat.mutating); // ChatIntegration's flag was set around it and is back
        // The scan's pass finds nothing more.
        chat.spoilers->afterScan(&chat.browser);
        QCOMPARE(all(chat.doc()).size(), 1);
        // Another module editing right then: left for the scan's pass.
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]b[/spoiler]"));
        chat.mutating = true;
        chat.spoilers->runEarlyPass();
        chat.mutating = false;
        QCOMPARE(rawPairCount(chat.doc()), 1);
        chat.spoilers->afterScan(&chat.browser);
        QCOMPARE(rawPairCount(chat.doc()), 0);
    }

    // Live test 2.7: with the same message in several server tabs TeamSpeak painted a chat before the 0 ms pass (the raw
    // pair showed for a frame). The viewport paints nothing from the insert until the pass has hidden it, and its updates
    // come back on after the pass, after the scan's pass when another module was editing, and at unload.
    void noPaintBeforeTheEarlyPass()
    {
        Chat chat;
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("hello"));
        chat.attach();
        QWidget* viewport = chat.browser.viewport();
        QVERIFY(viewport->updatesEnabled());
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("x [spoiler]short secret[/spoiler] y"));
        QVERIFY(chat.spoilers->earlyPending());
        QVERIFY(!viewport->updatesEnabled()); // a repaint() now does nothing
        chat.spoilers->runEarlyPass();
        QCOMPARE(rawPairCount(chat.doc()), 0);
        QVERIFY(viewport->updatesEnabled());
        // A message without a marker: nothing changes.
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("plain"));
        QVERIFY(viewport->updatesEnabled());
        // Another module editing at the 0 ms pass: still off until the scan's pass hid it.
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]b[/spoiler]"));
        QVERIFY(!viewport->updatesEnabled());
        chat.mutating = true;
        chat.spoilers->runEarlyPass();
        chat.mutating = false;
        QVERIFY(!viewport->updatesEnabled());
        chat.spoilers->afterScan(&chat.browser);
        QCOMPARE(rawPairCount(chat.doc()), 0);
        QVERIFY(viewport->updatesEnabled());
        // Never left off: the safety timer, and the unload.
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]c[/spoiler]"));
        QVERIFY(!viewport->updatesEnabled());
        chat.mutating = true;
        chat.spoilers->runEarlyPass();
        chat.mutating = false;
        QTRY_VERIFY_WITH_TIMEOUT(viewport->updatesEnabled(), 2000);
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]d[/spoiler]"));
        QVERIFY(!viewport->updatesEnabled());
        delete chat.spoilers;
        chat.spoilers = nullptr;
        QVERIFY(viewport->updatesEnabled());
    }

    // A caption's spoiler is hidden early; the TS Media link after it (the media pass replaces it) stays as it was.
    // Review fix: the pass tells the chat layout which blocks it hid (it ignores edits made under the mutating flag), so
    // a row styled before the first pass (a plugin load) has its mention test run again on the hidden text.
    void passReportsHiddenBlocks()
    {
        Chat chat;
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("hello"));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]Alice dies[/spoiler]"), kBob, QStringLiteral("Bob"));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("bye"));
        chat.attach();
        QCOMPARE(chat.hid, QList<int>({1}));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("x [spoiler]y[/spoiler]"));
        chat.spoilers->runEarlyPass();
        QCOMPARE(chat.hid, QList<int>({1, 3}));
        // The plugin's host asks ChatLayout to restyle those blocks (source rule), and attaches the spoilers first.
        QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/src/chatintegration.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString ci = QString::fromUtf8(f.readAll());
        QVERIFY(ci.contains(QLatin1String("m_layout->restyleSoon(browser, block);")));
        QVERIFY(ci.indexOf(QLatin1String("m_textSpoilers->attach(browser);")) < ci.indexOf(QLatin1String("m_layout->attach(browser);")));
    }

    void captionSpoilerLeavesTheMediaLink()
    {
        Chat          chat;
        const QString href = QStringLiteral("ts3file://127.0.0.1?port=9987&channel=1&path=%2F&filename=holiday.jpg&size=2400&isDir=0&serverUID=x&message=TS%20Media%20chat");
        chat.attach();
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("my [spoiler]caption secret[/spoiler]<br>") + tschat::linkHtml(tschat::darkSkin(), href, QStringLiteral("holiday.jpg"))
                                                    + QStringLiteral(" You need TS Media chat to see this."));
        chat.spoilers->runEarlyPass();
        QCOMPARE(rawPairCount(chat.doc()), 0);
        int anchors = 0;
        for (auto it = chat.doc()->lastBlock().begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.charFormat().anchorHref() == href) {
                QCOMPARE(f.text(), QStringLiteral("holiday.jpg"));
                ++anchors;
            }
        }
        QCOMPARE(anchors, 1);
        QVERIFY(chat.doc()->lastBlock().text().contains(QLatin1String("You need TS Media chat")));
        QCOMPARE(replydoc::parseBlock(chat.doc()->lastBlock()).mediaLabel, QStringLiteral("holiday.jpg"));
    }

    void clickRevealsOnlyThatSpoiler()
    {
        Chat chat;
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]first[/spoiler] and [spoiler]second[/spoiler]"));
        chat.attach();
        QVector<textspoilerdoc::Spoiler> sp = all(chat.doc());
        QCOMPARE(sp.size(), 2);
        const QRect second = chat.spoilers->barRects(&chat.browser, sp.at(1).id).value(0);
        QVERIFY(!second.isEmpty());
        QCOMPARE(chat.spoilers->barAt(&chat.browser, second.center()), sp.at(1).id);
        QVERIFY(chat.click(second.center()));
        sp = all(chat.doc());
        QVERIFY(!sp.at(0).revealed);
        QVERIFY(sp.at(1).revealed);
        QVERIFY(chat.doc()->toPlainText().contains(QLatin1String("second")));
        QVERIFY(!chat.doc()->toPlainText().contains(QLatin1String("first")));
        // A click beside the bars is not ours.
        QMouseEvent press(QEvent::MouseButtonPress, QPoint(2, chat.browser.viewport()->height() - 2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(!chat.spoilers->filterEvent(chat.browser.viewport(), &press));
        QCOMPARE(chat.spoilers->hiddenCount(), 1);
        QCOMPARE(chat.spoilers->revealedCount(), 1);
        QCOMPARE(chat.spoilers->diagnosticLines(), QStringList{QStringLiteral("Text spoilers: hidden 1, revealed 1")});
    }

    void menuRevealsTheMessage()
    {
        Chat chat;
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]a[/spoiler] [spoiler]b[/spoiler]"));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]c[/spoiler]"));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("nothing"));
        chat.attach();
        QMenu menu;
        QAction* two = chat.spoilers->revealAction(&menu, &chat.browser, 0);
        QVERIFY(two);
        QCOMPARE(two->text(), QStringLiteral("Re&veal spoilers"));
        QAction* one = chat.spoilers->revealAction(&menu, &chat.browser, 1);
        QVERIFY(one);
        QCOMPARE(one->text(), QStringLiteral("Re&veal spoiler"));
        QVERIFY(!chat.spoilers->revealAction(&menu, &chat.browser, 2));
        two->trigger();
        QCOMPARE(chat.spoilers->hiddenCount(), 1);
        QVERIFY(!chat.spoilers->revealAction(&menu, &chat.browser, 0));
    }

    void settingRevealsAndHidesAgain()
    {
        Chat chat;
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]clicked[/spoiler]"));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]by the setting[/spoiler]"));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]with a card ") + tschat::linkHtml(tschat::darkSkin(), QStringLiteral("https://example.com/c"))
                                                    + QStringLiteral("[/spoiler]"));
        chat.attach();
        QVERIFY(chat.click(chat.spoilers->barRects(&chat.browser, all(chat.doc()).at(0).id).value(0).center()));
        Settings::instance().revealSpoilers = true;
        chat.spoilers->settingsChanged();
        QCOMPARE(chat.spoilers->hiddenCount(), 0);
        // A link preview's slot came under the revealed link meanwhile.
        {
            const textspoilerdoc::Spoiler third = all(chat.doc()).at(2);
            QTextCursor                   c(chat.doc());
            c.setPosition(third.close);
            QTextImageFormat slot;
            slot.setName(QStringLiteral("tsmedia-link:1"));
            slot.setProperty(linkdoc::kSlotObject, true);
            c.insertImage(slot);
        }
        Settings::instance().revealSpoilers = false;
        chat.spoilers->settingsChanged();
        const QVector<textspoilerdoc::Spoiler> sp = all(chat.doc());
        QCOMPARE(sp.size(), 3);
        QVERIFY(sp.at(0).revealed);  // clicked: stays
        QVERIFY(!sp.at(1).revealed); // the setting's: hidden again
        QVERIFY(sp.at(2).revealed);  // a preview came into it: stays
        QVERIFY(!chat.doc()->toPlainText().contains(QLatin1String("by the setting")));
        // New spoilers arrive revealed while it is on.
        Settings::instance().revealSpoilers = true;
        chat.spoilers->settingsChanged();
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]new[/spoiler]"));
        chat.spoilers->runEarlyPass();
        QVERIFY(all(chat.doc()).last().revealed);
        QCOMPARE(rawPairCount(chat.doc()), 0);
    }

    void documentSwapKeepsReveals()
    {
        Chat chat;
        // TeamSpeak's own documents (a document QTextEdit made itself goes when another is set).
        auto* old = new QTextDocument(&chat.browser);
        setup(*old);
        chat.browser.setDocument(old);
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]old one[/spoiler]"));
        chat.attach();
        QVERIFY(chat.click(chat.spoilers->barRects(&chat.browser).value(0).center()));
        // TeamSpeak reloads the history into a new document: the same message again, and a new one.
        auto* fresh = new QTextDocument(&chat.browser);
        setup(*fresh);
        message(fresh, tschat::darkSkin(), QStringLiteral("[spoiler]old one[/spoiler]"));
        message(fresh, tschat::darkSkin(), QStringLiteral("[spoiler]new one[/spoiler]"));
        chat.browser.setDocument(fresh);
        chat.spoilers->documentSwapped(&chat.browser);
        QCOMPARE(fresh->documentLayout()->handlerForObject(textspoilerdoc::kObjectType) != nullptr, true);
        chat.spoilers->runEarlyPass();
        const QVector<textspoilerdoc::Spoiler> sp = all(fresh);
        QCOMPARE(sp.size(), 2);
        QVERIFY(sp.at(0).revealed);
        QVERIFY(!sp.at(1).revealed);
        // The old document is given back too at unload.
        QVERIFY(!all(old).isEmpty());
        delete chat.spoilers;
        chat.spoilers = nullptr;
        QVERIFY(all(old).isEmpty());
        QVERIFY(all(fresh).isEmpty());
        QVERIFY(old->toPlainText().contains(QLatin1String("[spoiler]old one[/spoiler]")));
    }

    // 2.7.1 (dark mode, review): TeamSpeak's look changed (ChatIntegration::themeChanged: dark mode on or off, a skin taken
    // over): every chat's bars take the colours the chat has now at once, a hidden chat's too, and the shown one paints them.
    void themeChangedRecolorsEveryChat()
    {
        QTextBrowser shown, hidden;
        for (QTextBrowser* b : {&shown, &hidden}) {
            b->resize(560, 300);
            setup(*b->document(), 520);
            message(b->document(), tschat::lightSkin(), QStringLiteral("a [spoiler]bbbbbbbbbb cccccccc[/spoiler] d"));
        }
        shown.show();
        QVERIFY(QTest::qWaitForWindowExposed(&shown));
        ChatTextSpoilers::Colors now{QColor(0xff, 0xff, 0xff), QColor(0, 0, 0)};
        bool                     mutating = false;
        ChatTextSpoilers::Host   host;
        host.othersMutating = [&mutating] { return mutating; };
        host.setMutating    = [&mutating](bool on) {
            const bool was = mutating;
            mutating       = on;
            return was;
        };
        host.colors = [&now](QTextBrowser*) { return now; };
        ChatTextSpoilers spoilers(host);
        spoilers.attach(&shown);
        spoilers.attach(&hidden);
        spoilers.runEarlyPass();
        QCOMPARE(spoilers.hiddenCount(), 2);
        QVERIFY(!hidden.isVisible());
        const auto lookOf = [](QTextDocument* doc) {
            auto* handler = dynamic_cast<textspoilerdoc::Handler*>(doc->documentLayout()->handlerForObject(textspoilerdoc::kObjectType));
            return handler ? handler->look(doc) : textspoilerdoc::Look();
        };
        const auto barPixel = [&] {
            shown.document()->documentLayout()->documentSize();
            chatscroll::syncRange(&shown);
            const QRect  bar   = spoilers.barRects(&shown).value(0);
            const QImage image = shown.viewport()->grab().toImage();
            return bar.isEmpty() ? QColor() : QColor(image.pixel(bar.center()));
        };
        for (QTextBrowser* b : {&shown, &hidden})
            QCOMPARE(lookOf(b->document()).base, now.base);
        const int  id     = textspoilerdoc::spoilersIn(shown.document()).value(0).id;
        const auto before = lookOf(shown.document());
        QCOMPARE(barPixel(), textspoilerdoc::barColor(before, id));
        // Dark mode on: the chat's colours change; nothing follows until TeamSpeak's look is said to have changed.
        now = {QColor(0x26, 0x26, 0x24), QColor(0xf5, 0xf4, 0xee)};
        QCOMPARE(lookOf(shown.document()).base, QColor(0xff, 0xff, 0xff));
        spoilers.themeChanged();
        for (QTextBrowser* b : {&shown, &hidden}) {
            QCOMPARE(lookOf(b->document()).base, now.base);
            QCOMPARE(lookOf(b->document()).text, now.text);
        }
        const auto after = lookOf(shown.document());
        QVERIFY(textspoilerdoc::barColor(after, id) != textspoilerdoc::barColor(before, id));
        QCOMPARE(barPixel(), textspoilerdoc::barColor(after, id));
        // Off again, from the hidden chat's side: both back.
        now = {QColor(0xff, 0xff, 0xff), QColor(0, 0, 0)};
        spoilers.themeChanged();
        for (QTextBrowser* b : {&shown, &hidden})
            QCOMPARE(lookOf(b->document()).base, now.base);
        QCOMPARE(barPixel(), textspoilerdoc::barColor(before, id));
    }

    void unloadGivesTheChatBack()
    {
        Chat chat;
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("a [spoiler]b c[/spoiler] d"));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]e[/spoiler]"));
        const QStringList before = snapshot(chat.doc());
        chat.browser.viewport()->setCursor(Qt::IBeamCursor);
        chat.attach();
        QVERIFY(snapshot(chat.doc()) != before);
        // The pointer over a bar: a pointing hand.
        const QRect bar = chat.spoilers->barRects(&chat.browser).value(0);
        QMouseEvent move(QEvent::MouseMove, bar.center(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        chat.spoilers->filterEvent(chat.browser.viewport(), &move);
        QCOMPARE(chat.browser.viewport()->cursor().shape(), Qt::PointingHandCursor);
        QVERIFY(chat.doc()->documentLayout()->handlerForObject(textspoilerdoc::kObjectType));
        delete chat.spoilers;
        chat.spoilers = nullptr;
        QCOMPARE(snapshot(chat.doc()), before);
        QVERIFY(!chat.doc()->documentLayout()->handlerForObject(textspoilerdoc::kObjectType));
        QCOMPARE(chat.browser.viewport()->cursor().shape(), Qt::IBeamCursor);
    }

    // The chat at its end stays at its end when a long spoiler shrinks; a reader above keeps their line.
    void readersPlaceKept()
    {
        Chat chat(tschat::darkSkin(), 560, 200);
        for (int i = 0; i < 30; ++i)
            message(chat.doc(), tschat::darkSkin(), QStringLiteral("message %1 about the map").arg(i));
        chat.attach();
        chat.browser.verticalScrollBar()->setValue(chat.browser.verticalScrollBar()->maximum());
        QVERIFY(chatscroll::atBottom(&chat.browser));
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]%1[/spoiler]").arg(QString(400, QLatin1Char('z'))));
        chatscroll::toBottom(&chat.browser); // TeamSpeak follows its new line
        chat.spoilers->runEarlyPass();
        chat.settle();
        QVERIFY(chatscroll::atBottom(&chat.browser));
        // Scrolled up: the line at the top stays where it is.
        chat.browser.verticalScrollBar()->setValue(chat.browser.verticalScrollBar()->maximum() / 3);
        const chatscroll::TextPlace place = chatscroll::captureText(&chat.browser);
        const int                   value = chat.browser.verticalScrollBar()->value();
        message(chat.doc(), tschat::darkSkin(), QStringLiteral("[spoiler]%1[/spoiler]").arg(QString(400, QLatin1Char('q'))));
        chat.spoilers->runEarlyPass();
        chat.settle();
        QVERIFY(!place.cursor.isNull());
        QCOMPARE(chat.browser.verticalScrollBar()->value(), value);
    }

    // Review fix: spoilers ABOVE the view, where hiding, revealing and hiding again change the heights above the reader:
    // the full pass at attach on a scrolled-up chat, Reveal from the message menu, and the setting on and off. The line at
    // the view's top stays where it is on screen (within 4 px) each time.
    void readersPlaceKeptAbove()
    {
        Chat chat(tschat::darkSkin(), 420, 200);
        // Long unbroken words: as bars they break between pieces, as text anywhere, so the heights above really change.
        for (int i = 0; i < 40; ++i) {
            if (i % 5 == 1 && i < 25)
                message(chat.doc(), tschat::darkSkin(), QStringLiteral("before %1 [spoiler]%2 %3 end[/spoiler] after").arg(i).arg(QString(260, QLatin1Char('z')), QString(90, QLatin1Char('y'))));
            else
                message(chat.doc(), tschat::darkSkin(), QStringLiteral("message %1 about the map").arg(i));
        }
        chat.settle();
        QScrollBar* bar = chat.browser.verticalScrollBar();
        bar->setValue(bar->maximum() * 9 / 10);
        const int topBlock = chat.browser.cursorForPosition(QPoint(5, 5)).block().blockNumber();
        QVERIFY2(topBlock > 22, qPrintable(QString::number(topBlock))); // every spoiler above the view
        const chatscroll::TextPlace place = chatscroll::captureText(&chat.browser);
        QVERIFY(!place.cursor.isNull());
        const auto onScreen = [&] {
            chat.settle();
            return chatscroll::lineTopOf(chat.doc(), place.cursor.position()) - bar->value();
        };
        const qreal before = onScreen();
        const qreal height = chat.doc()->documentLayout()->documentSize().height();
        QStringList moved;
        const auto  check = [&](const char* step) {
            const qreal now = onScreen();
            if (qAbs(now - before) > 4.0)
                moved << QStringLiteral("%1: %2 -> %3").arg(QLatin1String(step)).arg(before).arg(now);
        };
        // The full pass at attach (TS Media loaded on a scrolled-up chat).
        chat.attach();
        QCOMPARE(all(chat.doc()).size(), 5);
        const qreal hidden = chat.doc()->documentLayout()->documentSize().height();
        check("attach");
        // Reveal from the message menu (the one in block 6).
        QMenu    menu;
        QAction* reveal = chat.spoilers->revealAction(&menu, &chat.browser, 6);
        QVERIFY(reveal);
        reveal->trigger();
        check("reveal");
        // The setting on (every spoiler revealed), then off (hidden again, but not the one revealed by the menu).
        Settings::instance().revealSpoilers = true;
        chat.spoilers->settingsChanged();
        check("setting on");
        Settings::instance().revealSpoilers = false;
        chat.spoilers->settingsChanged();
        check("setting off");
        QVERIFY2(moved.isEmpty(), qPrintable(moved.join(QStringLiteral("; "))));
        qInfo("heights above the view: %.0f raw, %.0f hidden", height, hidden);
    }

    // Strings put into TeamSpeak's documents, Qt state or menus are built at run time (CRASH RULE).
    void noStaticStringsInFormatsOrActions()
    {
        for (const char* file : {"src/textspoilerdoc.cpp", "src/chattextspoilers.cpp", "src/inputformat.cpp"}) {
            QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/") + QString::fromLatin1(file));
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            for (const QString& line : lines) {
                const bool sets = line.contains(QLatin1String("setProperty(")) || line.contains(QLatin1String("setObjectName(")) || line.contains(QLatin1String("new QAction("))
                                  || line.contains(QLatin1String("setText(")) || line.contains(QLatin1String("insertText("));
                QVERIFY2(!(sets && line.contains(QLatin1String("QStringLiteral"))), qPrintable(QString::fromLatin1(file) + QStringLiteral(": ") + line));
            }
        }
    }
};

TSMEDIA_REGISTER_TEST(TestChatTextSpoilers)
#include "tst_chattextspoilers.moc"
