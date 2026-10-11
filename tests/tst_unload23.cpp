// 2.3 cross review (lifetime): what plugin unload and disable give back, with every 2.3 feature at once.
//   documents   a TeamSpeak-like chat with a poll card, a reply line, text reaction rows inside message
//               bubbles and TS Media's layout, edited in ChatIntegration's scan order and given back in its
//               destructor's order: byte for byte (every character's and block's format), pictures freed;
//               also a poll card made after the layout styled its message (the usual order);
//   the input   the paperclip with its menu open when the input is given back (EmojiInput::detach): the
//               menu goes, no popup of ours stays open, a pick queued before never runs;
//   the music   the controller deleted while it plays and keeps push-to-talk open, and while a notice
//               shows: the audio callbacks detached, TeamSpeak's input given back, the bar out of the layout;
//               push-to-talk opened and given back whatever TeamSpeak's flush answers.
// TeamSpeak-like documents and widgets (tests/tschat.h, a fake TeamSpeak), made-up people only.

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QElapsedTimer>
#include <QBoxLayout>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QtTest>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

#include "attachbutton.h"
#include "attachmenu.h"
#include "attachpopup.h"
#include "fakets3.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "music/musicbar.h"
#include "music/musiccontroller.h"
#include "music/musicmix.h"
#include "musicfakes.h"
#include "polldoc.h"
#include "polls.h"
#include "replydoc.h"
#include "testmain.h"
#include "textreactdoc.h"
#include "tschat.h"

namespace {

using layoutart::Mode;
using musicfakes::FakeVoice;

const QString kAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");
const QString kMe    = QStringLiteral("q0Xn6Mine000000000000000000=");

QFont chatFont()
{
    QFont f(QStringLiteral("Segoe UI"));
    f.setPointSizeF(9.0);
    return f;
}

void setup(QTextDocument& doc)
{
    doc.setDefaultFont(chatFont());
    doc.setTextWidth(520);
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
}

layoutdoc::Env envFor(Mode mode, bool bubbles)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false, bubbles && mode == Mode::Cozy);
    env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
    env.dpr          = 1.0;
    env.contentWidth = 512;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("u");
    env.ownNick      = QStringLiteral("Sam");
    env.ownUid       = kMe;
    return env;
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

// A reply's quote line as TeamSpeak shows the BBCode the reply module sends ([i], a client link).
QString quoteHtml(const QString& nick, int clid, const QString& uid, const QString& time, const QString& snippet)
{
    return QStringLiteral("<i>↪ <a href=\"client://%1/%2~%3\">%3</a> · %4: “%5”</i>").arg(clid).arg(uid, nick, time, snippet);
}

void fill(QTextDocument& doc)
{
    const tschat::Skin s = tschat::darkSkin();
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"))));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:05"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Who's in for tonight?")));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:01:00"), 19, kBob, QStringLiteral("Bob"), pollHtml(s, samplePoll())));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:02:10"), 17, kAlice, QStringLiteral("Alice"),
                                             quoteHtml(QStringLiteral("Bob"), 19, kBob, QStringLiteral("16∶01"), QStringLiteral("Which map tonight?"))
                                                 + QStringLiteral("<br>Harbor, as always")));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:02:30"), 19, kBob, QStringLiteral("Bob"), QStringLiteral("deal"), false));
    doc.documentLayout()->documentSize();
}

// Every character with its format, every block's format and visibility: what TeamSpeak's document is.
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
        if (it.key() == QTextFormat::ObjectIndex && doc) { // a picture given back gets an equal new object
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
        if (a.chars.at(i) != b.chars.at(i)) {
            QStringList what; // the properties that differ, for the failure message
            QVariantMap all = a.chars.at(i);
            for (auto it = b.chars.at(i).constBegin(); it != b.chars.at(i).constEnd(); ++it)
                all.insert(it.key(), it.value());
            for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
                if (a.chars.at(i).value(it.key()) != b.chars.at(i).value(it.key()))
                    what << QStringLiteral("%1: %2 / %3").arg(it.key(), a.chars.at(i).value(it.key()).toString(), b.chars.at(i).value(it.key()).toString());
            }
            return QStringLiteral("char %1 (%2) format differs: %3").arg(i).arg(a.text.mid(i, 1), what.join(QStringLiteral("; ")));
        }
    }
    return {};
}

int blockWith(QTextDocument& doc, const QString& text)
{
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (b.text().contains(text))
            return b.blockNumber();
    }
    return -1;
}

class ChatLineEdit : public QTextEdit // TeamSpeak's chat input class name
{
  public:
    using QTextEdit::QTextEdit;
};

} // namespace

class TestUnload23 : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase() { m_saved = ts3::funcs; }
    void cleanupTestCase() { ts3::funcs = m_saved; }
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        fakets3::install(m_dir->path());
        FakeVoice::install();
        for (const attachmenu::Entry& e : attachmenu::entries())
            attachmenu::removeEntry(e.id);
    }
    void cleanup()
    {
        QVERIFY(!music::active());
        QVERIFY(!QApplication::activePopupWidget());
        for (const attachmenu::Entry& e : attachmenu::entries())
            attachmenu::removeEntry(e.id);
    }

    // ---- documents ------------------------------------------------------------------------------------

    // Every 2.3 layer in one chat, as ChatIntegration makes them (polls, replies, text reactions, the layout
    // last) and gives them back (~ChatIntegration: text reactions, the layout, whose edit makes the polls
    // place their line break again, the polls, the replies): TeamSpeak's document byte for byte.
    void everyLayerGivenBackByteExact_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("bubbles");
        QTest::newRow("cozy bubbles") << static_cast<int>(Mode::Cozy) << true;
        QTest::newRow("cozy plain") << static_cast<int>(Mode::Cozy) << false;
        QTest::newRow("compact") << static_cast<int>(Mode::Compact) << false;
    }
    void everyLayerGivenBackByteExact()
    {
        QFETCH(int, mode);
        QFETCH(bool, bubbles);
        QTextDocument doc;
        setup(doc);
        fill(doc);
        const Snapshot plain = snapshot(doc);
        QStringList    pictures;
        {
            layoutformat::MutatingScope mutating;
            // ChatPolls::afterScan: the poll message becomes its card.
            const QVector<polldoc::Found> polls = polldoc::scan(&doc);
            QCOMPARE(polls.size(), 1);
            const QString card = polldoc::objectPrefix() + QStringLiteral("u1");
            doc.addResource(QTextDocument::ImageResource, QUrl(card), QPixmap(260, 200));
            QVERIFY(polldoc::collapse(&doc, polls.first(), card, QSizeF(260, 200)));
            polldoc::normalizeAll(&doc);
            pictures << card;
            // ChatReplies::afterScan: the quote line becomes a reply line.
            const QVector<replydoc::Message> messages = replydoc::scan(&doc);
            int                              quoted   = 0;
            for (const replydoc::Message& m : messages) {
                if (!m.hasQuote)
                    continue;
                const QString line = replydoc::objectPrefix() + QStringLiteral("u.%1").arg(++quoted);
                doc.addResource(QTextDocument::ImageResource, QUrl(line), QPixmap(220, 14));
                QVERIFY(replydoc::collapse(&doc, m, line, QSizeF(220, 14)));
                pictures << line;
            }
            QCOMPARE(quoted, 1);
            // ChatTextReactions: rows at the end of three messages (the poll's too).
            int row = 0;
            for (const QString& text : {QStringLiteral("in for tonight"), QStringLiteral("as always"), QStringLiteral("deal")}) {
                const int     n    = blockWith(doc, text);
                const QString name = textreactdoc::objectPrefix() + QStringLiteral("u%1").arg(++row);
                QVERIFY2(n >= 0, qPrintable(text));
                doc.addResource(QTextDocument::ImageResource, QUrl(name), QPixmap(120, 28));
                QVERIFY(textreactdoc::insertRow(&doc, n, name, QStringLiteral("k%1").arg(row), QSizeF(120, 28)));
                pictures << name;
            }
            const int pollBlock = polldoc::scan(&doc).first().block;
            const QString name  = textreactdoc::objectPrefix() + QStringLiteral("u%1").arg(++row);
            QVERIFY(textreactdoc::insertRow(&doc, pollBlock, name, QStringLiteral("k%1").arg(row), QSizeF(120, 28)));
            pictures << name;
        }
        // ChatLayout last; the polls follow its look (ChatLayout::documentEdited).
        const layoutdoc::Env env = envFor(static_cast<Mode>(mode), bubbles);
        QVERIFY(layoutdoc::styleAll(&doc, env) > 0);
        layoutdoc::renderAll(&doc, env, env.today);
        {
            layoutformat::MutatingScope mutating;
            polldoc::normalizeAll(&doc);
        }
        doc.documentLayout()->documentSize();
        QVERIFY(compare(plain, snapshot(doc)) != QString());

        // ~ChatIntegration's order.
        {
            layoutformat::MutatingScope mutating;
            QStringList                 rows;
            QCOMPARE(textreactdoc::removeAll(&doc, &rows), 4); // ~ChatTextReactions
            for (const QString& r : qAsConst(rows))
                doc.addResource(QTextDocument::ImageResource, QUrl(r), QVariant());
        }
        QVERIFY(layoutdoc::giveBack(&doc) > 0); // ~ChatLayout
        {
            layoutformat::MutatingScope mutating;
            polldoc::normalizeAll(&doc);         // ChatPolls hears ChatLayout's documentEdited
            QCOMPARE(polldoc::giveBack(&doc), 1); // ~ChatPolls
            QCOMPARE(replydoc::giveBack(&doc), 1); // ~ChatReplies
        }
        doc.documentLayout()->documentSize();
        QCOMPARE(compare(plain, snapshot(doc)), QString());
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            QVERIFY(!layoutdoc::hasOurs(b));
            QVERIFY(!textreactdoc::rowOf(b).exists());
            QVERIFY(textreactdoc::rowOf(b).separator < 0);
        }
        QVERIFY(polldoc::objectPositions(&doc).isEmpty());
        QVERIFY(replydoc::objectPositions(&doc).isEmpty());
        for (const QString& name : qAsConst(pictures)) {
            const QVariant left = doc.resource(QTextDocument::ImageResource, QUrl(name));
            QVERIFY2(!left.isValid() || left.value<QPixmap>().isNull(), qPrintable(name)); // the memory went
        }
    }

    // A new poll message is styled by the layout first (ChatLayout's quick step comes right after TeamSpeak
    // appended it, ChatIntegration's scan later), then ChatPolls makes its card. What the card keeps must be
    // TeamSpeak's text, not the layout's look of it: given back in ~ChatIntegration's order (the layout
    // before the polls), the document is TeamSpeak's again byte for byte.
    void pollCardMadeAfterTheLayoutGivenBackByteExact_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("bubbles");
        QTest::addColumn<bool>("light");
        QTest::newRow("cozy bubbles") << static_cast<int>(Mode::Cozy) << true << false;
        QTest::newRow("cozy bubbles light") << static_cast<int>(Mode::Cozy) << true << true;
        QTest::newRow("cozy plain") << static_cast<int>(Mode::Cozy) << false << false;
        QTest::newRow("compact") << static_cast<int>(Mode::Compact) << false << false;
    }
    void pollCardMadeAfterTheLayoutGivenBackByteExact()
    {
        QFETCH(int, mode);
        QFETCH(bool, bubbles);
        QFETCH(bool, light);
        QTextDocument      doc;
        const tschat::Skin s = light ? tschat::lightSkin() : tschat::darkSkin();
        setup(doc);
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:05"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Who's in for tonight?")));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:01:00"), 19, kBob, QStringLiteral("Bob"), pollHtml(s, samplePoll())));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:02:30"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("deal")));
        doc.documentLayout()->documentSize();
        const Snapshot plain = snapshot(doc);
        layoutdoc::Env env   = envFor(static_cast<Mode>(mode), bubbles);
        if (light)
            env.colors = layoutart::colorsFor(false, QColor(Qt::white), QColor(Qt::black), QColor(0x1c, 0x82, 0xcc));
        QVERIFY(layoutdoc::styleAll(&doc, env) > 0); // the quick step
        layoutdoc::renderAll(&doc, env, env.today);
        const QString card = polldoc::objectPrefix() + QStringLiteral("u1");
        {
            layoutformat::MutatingScope   mutating; // ChatPolls::afterScan
            const QVector<polldoc::Found> polls = polldoc::scan(&doc);
            QCOMPARE(polls.size(), 1);
            doc.addResource(QTextDocument::ImageResource, QUrl(card), QPixmap(260, 200));
            QVERIFY(polldoc::collapse(&doc, polls.first(), card, QSizeF(260, 200)));
            polldoc::normalizeAll(&doc);
        }
        QVERIFY(layoutdoc::giveBack(&doc) > 0); // ~ChatLayout
        {
            layoutformat::MutatingScope mutating;
            polldoc::normalizeAll(&doc);          // ChatPolls hears ChatLayout's documentEdited
            QCOMPARE(polldoc::giveBack(&doc), 1); // ~ChatPolls
        }
        doc.documentLayout()->documentSize();
        QCOMPARE(compare(plain, snapshot(doc)), QString());
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            QVERIFY(!layoutdoc::hasOurs(b));
    }

    // ---- the input ------------------------------------------------------------------------------------

    // The paperclip's menu open when EmojiInput gives the input back (unload, the emoji button turned off
    // with it): the button and its menu go, no popup of ours stays open; an entry picked just before runs
    // never (its call was queued on the button).
    void paperclipMenuOpenAtUnload()
    {
        QWidget window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* column = new QVBoxLayout(&window);
        auto* input  = new ChatLineEdit(&window);
        input->setFixedHeight(30);
        column->addWidget(new QTextBrowser(&window), 1);
        column->addWidget(input);
        window.resize(480, 300);
        window.show();
        int ran = 0;
        attachmenu::Entry entry;
        entry.id      = QStringLiteral("unload-test");
        entry.order   = 10;
        entry.text    = [] { return QStringLiteral("Photo or video"); };
        entry.trigger = [&ran](QWidget*) { ++ran; };
        attachmenu::addEntry(entry);

        QPointer<AttachButton> button = AttachButton::placeIn(input, QRect(0, 0, 30, 30));
        QVERIFY(button);
        button->openMenu();
        QPointer<AttachPopup> popup = button->popup();
        QVERIFY(popup && popup->isVisible());
        QCOMPARE(AttachPopup::current(), popup.data());
        AttachButton::removeFrom(input); // EmojiInput::detach
        QVERIFY(!button);
        QVERIFY(!popup);
        QVERIFY(!AttachPopup::current());
        QVERIFY(!QApplication::activePopupWidget());

        // Picked, then the input given back before the queued trigger ran.
        button = AttachButton::placeIn(input, QRect(0, 0, 30, 30));
        button->openMenu();
        popup = button->popup();
        QVERIFY(popup);
        QTest::keyClick(popup.data(), Qt::Key_Return);
        QVERIFY(!popup || popup->isHidden());
        AttachButton::removeFrom(input);
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(ran, 0);
        QVERIFY(!AttachPopup::current());
        // And with the button there, it runs (the queue works).
        button = AttachButton::placeIn(input, QRect(0, 0, 30, 30));
        button->openMenu();
        QTest::keyClick(button->popup(), Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(ran, 1);
        AttachButton::removeFrom(input);
    }

    // ---- the music ------------------------------------------------------------------------------------

    // Unload while the music plays on push-to-talk (the input kept open), and while a notice shows: the
    // callbacks detached before the engine goes, TeamSpeak's input given back, the bar out of TeamSpeak's
    // layout, the menu entry gone, the volume chosen just now written.
    void musicDeletedAtUnload()
    {
        QWidget window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* column = new QVBoxLayout(&window);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        auto* input = new ChatLineEdit(&window);
        input->setFixedHeight(30);
        column->addWidget(new QTextBrowser(&window), 1);
        column->addWidget(input);
        window.resize(600, 360);
        window.show();
        const QString track = m_dir->filePath(QStringLiteral("Morning Walk.mp3"));
        {
            QFile f(track);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("generated in the test");
        }
        MusicController::Host h;
        h.input             = [input] { return input; };
        h.visibleConnection = [] { return static_cast<quint64>(FakeVoice::visible); };
        h.source            = [](const QString&) { return musicfakes::gen(); };
        h.settingsFile      = m_dir->filePath(QStringLiteral("music.ini"));
        const int items     = column->count();

        auto* c = new MusicController(h);
        QCOMPARE(attachmenu::entries().size(), 1);
        FakeVoice::deactivated = INPUT_DEACTIVATED; // push-to-talk, key up
        QVERIFY(c->start(track));
        playFor(*c, 200);
        QVERIFY(c->inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE));
        QPointer<MusicBar> bar = c->bar();
        QVERIFY(bar && bar->isVisible());
        QCOMPARE(column->count(), items + 1); // in TeamSpeak's layout
        c->setVolume(30);                      // saved 800 ms later, or at once on unload
        delete c;
        QVERIFY(!music::active());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        QVERIFY(!bar);
        QCOMPARE(column->count(), items);
        QVERIFY(attachmenu::entries().isEmpty());
        QFile saved(h.settingsFile);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains("volume=30"));

        // A notice on screen (the music stopped on a disconnect): its timer and the bar go too.
        c = new MusicController(h);
        QVERIFY(c->start(track));
        c->connectionLost(fakets3::kConnection);
        bar = c->bar();
        QVERIFY(bar && bar->isVisible());
        QCOMPARE(bar->view().mode, MusicBar::Mode::Notice);
        delete c;
        QVERIFY(!bar);
        QCOMPARE(column->count(), items);
        QVERIFY(!music::active());
    }

    // CLIENT_INPUT_DEACTIVATED is TeamSpeak's own, never the server's: the set takes effect at once, and the
    // flush may have nothing to send (ERROR_ok_no_update) or fail. Whatever the flush says, an input the
    // plugin opened is known as opened: no microphone goes out while push-to-talk is up, and pause, stop and
    // unload give TeamSpeak's input back (before: the plugin thought it had failed, let the voice through
    // and left TeamSpeak's input open after it was gone).
    void pushToTalkGivenBackWhateverTheFlushSays_data()
    {
        QTest::addColumn<unsigned>("flushResult");
        QTest::newRow("no update") << static_cast<unsigned>(ERROR_ok_no_update);
        QTest::newRow("failed") << static_cast<unsigned>(ERROR_undefined);
    }
    void pushToTalkGivenBackWhateverTheFlushSays()
    {
        QFETCH(unsigned, flushResult);
        static unsigned s_flush = ERROR_ok;
        s_flush                 = flushResult;
        ts3::funcs.flushClientSelfUpdates = [](uint64, const char*) -> unsigned int { return s_flush; };
        QWidget window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* column = new QVBoxLayout(&window);
        auto* input  = new ChatLineEdit(&window);
        input->setFixedHeight(30);
        column->addWidget(new QTextBrowser(&window), 1);
        column->addWidget(input);
        window.resize(600, 360);
        window.show();
        const QString track = m_dir->filePath(QStringLiteral("Morning Walk.mp3"));
        {
            QFile f(track);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("generated in the test");
        }
        MusicController::Host h;
        h.input             = [input] { return input; };
        h.visibleConnection = [] { return static_cast<quint64>(FakeVoice::visible); };
        h.source            = [](const QString&) { return musicfakes::gen(); };
        int leaked = 0;
        // A capture block TeamSpeak would send (no voice activation): the microphone (loud) never in it.
        const auto capture = [&leaked] {
            std::vector<qint16> block(2 * 960, 12000);
            int                 edited = music::kEditedSend;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
            if ((edited & music::kEditedSend) && std::any_of(block.begin(), block.end(), [](qint16 s) { return std::abs(int(s)) > 9000; }))
                ++leaked;
        };
        std::unique_ptr<MusicController> c(new MusicController(h));
        FakeVoice::deactivated = INPUT_DEACTIVATED; // push-to-talk, key up
        QVERIFY(c->start(track));
        playFor(*c, 200);
        for (int i = 0; i < 5; ++i)
            capture();
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE)); // opened for the music
        QCOMPARE(leaked, 0);
        QVERIFY(c->inputKeptOpen());
        c->togglePause(); // given back
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        c->togglePause();
        playFor(*c, 100);
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE));
        for (int i = 0; i < 5; ++i)
            capture();
        QCOMPARE(leaked, 0);
        c.reset(); // unload
        QVERIFY(!music::active());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
    }

  private:
    // TeamSpeak's audio threads, played by the test: both sides every 20 ms for ms, with the bar ticking.
    static void playFor(MusicController& c, int ms)
    {
        std::vector<qint16> capture(960, 0), out(2 * 960, 0);
        unsigned            speakers[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
        QElapsedTimer       t;
        t.start();
        while (t.elapsed() < ms) {
            int      edited = 0;
            unsigned mask   = 0;
            music::onCaptured(fakets3::kConnection, capture.data(), 960, 1, &edited);
            music::onPlayback(fakets3::kConnection, out.data(), 960, 2, speakers, &mask);
            c.refreshNow();
            QTest::qWait(20);
        }
    }

    TS3Functions                   m_saved;
    std::unique_ptr<QTemporaryDir> m_dir;
};

TSMEDIA_REGISTER_TEST(TestUnload23)

#include "tst_unload23.moc"
