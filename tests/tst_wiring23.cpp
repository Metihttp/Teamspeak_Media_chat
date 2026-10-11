// 2.3 integration: the features of 2.3 together. Message bubbles with the cards, poll cards and reaction rows
// inside them (no box in a box, readable on every bubble), text reactions on poll messages and never on
// centred system rows, the bars stacked above the chat input with TeamSpeak's layout spacing, the hold strip
// on that stack, and the send window's "Who?" button and height. TeamSpeak-like documents (tests/tschat.h),
// made-up people only.

#include <QAbstractTextDocumentLayout>
#include <QBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QtTest>

#include "audiocard.h"
#include "composedialog.h"
#include "composehooks.h"
#include "holdstrip.h"
#include "inputbars.h"
#include "layoutart.h"
#include "layoutbubbles.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "music/musicbar.h"
#include "pollart.h"
#include "polldoc.h"
#include "polls.h"
#include "previewpaint.h"
#include "previewrenderer.h"
#include "reactionart.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "testmain.h"
#include "tschat.h"
#include "uiutil.h"
#include "voicecard.h"

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

layoutdoc::Env envFor(bool bubbles, bool dark, int width = 512)
{
    layoutdoc::Env env;
    env.mode         = Mode::Cozy;
    env.tokens       = layoutart::tokensFor(Mode::Cozy, chatFont(), false, bubbles);
    env.colors       = layoutart::colorsFor(dark, dark ? QColor(0x2f, 0x31, 0x36) : QColor(Qt::white), dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black),
                                            dark ? QColor(0x1c, 0xb0, 0xf4) : QColor(0x1c, 0x82, 0xcc));
    env.dpr          = 1.0;
    env.contentWidth = width;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("w");
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

int blockWith(QTextDocument& doc, const QString& text)
{
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (b.text().contains(text))
            return b.blockNumber();
    }
    return -1;
}

MediaEntry entryFor(const QString& fileName, quint64 size, qint64 durationMs, MediaState state, bool voice = false)
{
    MediaEntry e;
    e.link.host       = QStringLiteral("127.0.0.1");
    e.link.port       = 9987;
    e.link.serverUid  = QStringLiteral("wiringServerUid=");
    e.link.channelId  = 1;
    e.link.path       = QStringLiteral("/tsmedia");
    e.link.fileName   = fileName;
    e.link.size       = size;
    e.link.protocol   = MediaLink::kProtocol;
    e.link.durationMs = durationMs;
    e.link.voice      = voice;
    if (voice) {
        for (int i = 0; i < 64; ++i)
            e.link.waveform.append(static_cast<char>(i % 16));
    }
    e.kind     = kindForFileName(fileName);
    e.state    = state;
    e.progress = state == MediaState::Ready ? 1.0 : 0.0;
    return e;
}

// The share of fully transparent pixels in a frame of the picture (its panel's edge and corners).
qreal transparentEdge(const QImage& image)
{
    const QImage img   = image.convertToFormat(QImage::Format_ARGB32);
    int          clear = 0, total = 0;
    for (int x = 0; x < img.width(); ++x) {
        for (const int y : {1, img.height() - 2}) {
            ++total;
            clear += qAlpha(img.pixel(x, y)) == 0 ? 1 : 0;
        }
    }
    return total ? static_cast<qreal>(clear) / total : 0.0;
}

ReactionView someReactions()
{
    ReactionView v;
    ReactionView::Entry a;
    a.reaction = 1;
    a.count    = 3;
    a.mine     = true;
    v.entries.append(a);
    ReactionView::Entry b;
    b.reaction = 2;
    b.count    = 1;
    v.entries.append(b);
    return v;
}

class ChatLineEdit23 : public QTextEdit
{
  public:
    using QTextEdit::QTextEdit;
};

} // namespace

class TestWiring23 : public QObject
{
    Q_OBJECT

  private slots:
    // Bubbles: every flat colour a bubble can have, for the cards on it.
    void bubbleSurfacesCoverEveryState()
    {
        for (const bool dark : {true, false}) {
            const layoutdoc::Env   env      = envFor(true, dark);
            const QVector<QColor>& surfaces = layoutart::bubbleSurfaces(env.colors);
            QCOMPARE(surfaces.size(), 8);
            QVERIFY(surfaces.contains(layoutart::bubbleBack(env.colors, false)));
            QVERIFY(surfaces.contains(layoutart::bubbleBack(env.colors, true, true, true)));
            for (const QColor& c : surfaces)
                QCOMPARE(c.alpha(), 255);
        }
    }

    // A file, audio or voice card on a bubble has no panel or frame of its own (the bubble is its panel), its
    // content starts at the bubble's text column, and its texts read on every bubble colour. Off a bubble the
    // card keeps its box.
    void cardsOnABubbleHaveNoBoxOfTheirOwn()
    {
        for (const bool dark : {true, false}) {
            PreviewStyle plain;
            plain.dark     = dark;
            plain.font     = chatFont();
            plain.maxWidth = 400;
            PreviewStyle onBubble = plain;
            onBubble.bubbles      = layoutart::bubbleSurfaces(envFor(true, dark).colors);
            QVERIFY(previewpaint::onBubble(onBubble));
            QVERIFY(!previewpaint::onBubble(plain));

            const MediaEntry file  = entryFor(QStringLiteral("project-files.zip"), 24700, 0, MediaState::Ready);
            const MediaEntry song  = entryFor(QStringLiteral("summer_mix.mp3"), 4200000, 205000, MediaState::Ready);
            const MediaEntry voice = entryFor(QStringLiteral("voice_message_3f9a1c2e.m4a"), 146320, 12040, MediaState::Ready, true);
            PlaybackOverlay  o;
            o.durationMs      = 12040;
            o.controlsVisible = true;
            QSize size;
            const QImage cards[][2] = {
                {renderPreview(file, MediaStill(), plain, &size), renderPreview(file, MediaStill(), onBubble, &size)},
                {renderAudioCard(song, o, plain, &size), renderAudioCard(song, o, onBubble, &size)},
                {renderVoiceCard(voice, o, plain, &size), renderVoiceCard(voice, o, onBubble, &size)},
            };
            for (const auto& pair : cards) {
                QVERIFY2(transparentEdge(pair[0]) < 0.2, "off a bubble a card keeps its panel");
                QVERIFY2(transparentEdge(pair[1]) > 0.8, "on a bubble a card draws no panel");
            }
            // The play button moves to the text column (the hit test follows the drawing).
            const QSize voiceSize = voiceCardSize(onBubble);
            QCOMPARE(voiceZoneAt(voice, o, onBubble, voiceSize, QPointF(18, 28)), VideoZone::PlayPause);
            QCOMPARE(voiceZoneAt(voice, o, plain, voiceSize, QPointF(4, 28)), VideoZone::Body);
            QCOMPARE(audioZoneAt(song, o, onBubble, audioCardSize(song, onBubble), QPointF(20, 32)), VideoZone::PlayPause);
            // Texts at 4.5:1, rings at 3:1 on every bubble colour.
            const previewpaint::Palette pal = previewpaint::palette(onBubble);
            QCOMPARE(pal.background.alpha(), 0);
            QCOMPARE(pal.border.alpha(), 0);
            for (const QColor& back : onBubble.bubbles) {
                QVERIFY2(ui::contrastRatio(pal.muted, back) >= 4.5, qPrintable(pal.muted.name() + QLatin1Char(' ') + back.name()));
                QVERIFY(ui::contrastRatio(pal.link, back) >= 4.5);
                QVERIFY(ui::contrastRatio(pal.title, back) >= 4.5);
                QVERIFY(ui::contrastRatio(pal.errorText, back) >= 4.5);
                QVERIFY(ui::contrastRatio(pal.progress, ui::flatten(pal.track, back)) >= 3.0);
            }
        }
    }

    // A poll card in a bubble: no panel, the bubble's text column, colours checked on every bubble colour;
    // off a bubble (Compact, Classic, bubbles off) it keeps its rounded panel.
    void pollCardInABubble()
    {
        for (const bool dark : {true, false}) {
            pollart::Style plain;
            plain.dark     = dark;
            plain.font     = chatFont();
            plain.maxWidth = 360;
            pollart::Style inside = plain;
            inside.bubbles        = layoutart::bubbleSurfaces(envFor(true, dark).colors);
            pollart::Card card;
            card.question = samplePoll().question;
            card.options  = samplePoll().options;
            card.results  = true;
            card.counts   = {5, 3, 4};
            card.percents = {42, 25, 33};
            card.voters   = 12;
            card.own      = 1;
            const pollart::Layout a = pollart::layout(card, plain);
            const pollart::Layout b = pollart::layout(card, inside);
            QVERIFY(b.question.left() < a.question.left()); // the bubble's text column
            QVERIFY(b.question.left() <= 2.0);
            for (const QRectF& row : b.rows)
                QVERIFY(row.left() >= 0 && row.right() <= b.size.width());
            QVERIFY(b.action.right() <= b.size.width());
            QVERIFY(b.size.width() <= inside.maxWidth);
            QVERIFY(transparentEdge(pollart::render(card, plain, pollart::Pointer())) < 0.2);
            QVERIFY(transparentEdge(pollart::render(card, inside, pollart::Pointer())) > 0.8);
        }
    }

    // Reaction rows inside a bubble (media and text alike): translucent pills that take the bubble's colour,
    // their counts readable on every bubble colour; the chat's own opaque pills elsewhere.
    void reactionRowsTakeTheBubblesColour()
    {
        for (const bool dark : {true, false}) {
            const layoutdoc::Env env     = envFor(true, dark);
            const rx::RowColors  plain   = rx::rowColors(dark, env.colors.base);
            const rx::RowColors  onBubble = rx::rowColors(dark, env.colors.base, true);
            QVERIFY(onBubble.pill.alpha() < 255);
            QVERIFY(onBubble.mine.alpha() < 255);
            QVERIFY(!dark || plain.pill.alpha() == 255);
            for (const QColor& back : layoutart::bubbleSurfaces(env.colors)) {
                QVERIFY(ui::contrastRatio(onBubble.count, ui::flatten(onBubble.pill, back)) >= 4.5);
                QVERIFY(ui::contrastRatio(onBubble.count, ui::flatten(onBubble.pillHover, back)) >= 4.5);
                QVERIFY(ui::contrastRatio(onBubble.mineCount, ui::flatten(onBubble.mine, back)) >= 4.5);
                // 2.3 ux review: hovered and pressed too, and your reaction's border (3:1 on the bubble).
                QVERIFY(ui::contrastRatio(onBubble.count, ui::flatten(onBubble.pillPressed, back)) >= 4.5);
                QVERIFY2(ui::contrastRatio(onBubble.mineCount, ui::flatten(onBubble.mineHover, back)) >= 4.5,
                         qPrintable(QString::number(ui::contrastRatio(onBubble.mineCount, ui::flatten(onBubble.mineHover, back)))));
                QVERIFY(ui::contrastRatio(onBubble.mineCount, ui::flatten(onBubble.minePressed, back)) >= 4.5);
                QVERIFY2(ui::contrastRatio(onBubble.mineBorder, back) >= 3.0, qPrintable(QString::number(ui::contrastRatio(onBubble.mineBorder, back))));
            }
        }
        // A media object on a bubble: the row's pills let the bubble through.
        QImage picture(QSize(200, 120), QImage::Format_ARGB32_Premultiplied);
        picture.fill(Qt::darkCyan);
        rx::ObjectState state;
        state.dark     = true;
        state.base     = QColor(0x2f, 0x31, 0x36);
        state.font     = chatFont();
        state.maxWidth = 400;
        state.bubbles  = layoutart::bubbleSurfaces(envFor(true, true).colors);
        QSize        objectSize;
        rx::ObjectLayout layout;
        const QImage object = rx::composeObject(picture, picture.size(), someReactions(), state, &layout, &objectSize).convertToFormat(QImage::Format_ARGB32);
        QVERIFY(objectSize.height() > picture.height());
        QVERIFY(!layout.row.pills.isEmpty());
        const QPointF inPill = layout.row.pills.first().center() + QPointF(0, picture.height()) + QPointF(-layout.row.pills.first().width() / 2 + 3, 0);
        QVERIFY(qAlpha(object.pixel(inPill.toPoint())) < 255);
    }

    // A text reaction row sits inside the message's bubble, at its bottom (like the media row); the next
    // message's bubble starts below it.
    void textReactionRowInsideTheBubble()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:05"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Who's in for tonight?")));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:20"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("around nine")));
        const int first = blockWith(doc, QStringLiteral("in for tonight"));
        QVERIFY(textreactdoc::insertRow(&doc, first, textreactdoc::objectPrefix() + QStringLiteral("w1"), QStringLiteral("k1"), QSizeF(120, 30)));
        const layoutdoc::Env env = envFor(true, true);
        QVERIFY(layoutdoc::styleAll(&doc, env) > 0);
        doc.documentLayout()->documentSize();
        const QTextBlock          b    = doc.findBlockByNumber(first);
        const layoutbubbles::Item item = layoutbubbles::itemOf(b, env);
        QCOMPARE(item.kind, layoutbubbles::Kind::Bubble);
        const QRectF row = textreactdoc::rowRect(b);
        QVERIFY(!row.isEmpty());
        QVERIFY2(item.rect.contains(row), qPrintable(QStringLiteral("%1,%2 %3x%4 in %5,%6 %7x%8")
                                                         .arg(row.left()).arg(row.top()).arg(row.width()).arg(row.height())
                                                         .arg(item.rect.left()).arg(item.rect.top()).arg(item.rect.width()).arg(item.rect.height())));
        QVERIFY(item.rect.bottom() - row.bottom() >= 2); // the bubble's padding under the row
        const layoutbubbles::Item next = layoutbubbles::itemOf(doc.findBlockByNumber(first + 1), env);
        QVERIFY(next.rect.top() >= item.rect.bottom());
    }

    // Centred system rows (TeamSpeak's and TS Media's own lines) get pills, never bubbles, and never a text
    // reaction row; messages do.
    void systemRowsNeverGetReactionsOrBubbles()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"))));
        tschat::append(&doc, tschat::printHtml(s, QStringLiteral("2 files sent.")));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:05"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("hello")));
        const layoutdoc::Env env = envFor(true, true);
        QVERIFY(layoutdoc::styleAll(&doc, env) > 0);
        doc.documentLayout()->documentSize();
        for (int n = 0; n < 2; ++n) {
            const QTextBlock b = doc.findBlockByNumber(n);
            QVERIFY(!textreactdoc::identify(b).eligible());
            QCOMPARE(layoutbubbles::itemOf(b, env).kind, layoutbubbles::Kind::Pill);
            QCOMPARE(b.blockFormat().alignment() & Qt::AlignHorizontal_Mask, Qt::AlignHCenter);
        }
        const QTextBlock message = doc.findBlockByNumber(2);
        QVERIFY(textreactdoc::identify(message).eligible());
        QCOMPARE(layoutbubbles::itemOf(message, env).kind, layoutbubbles::Kind::Bubble);
    }

    // A poll message gets text reactions with one key, whether its card is there yet or not (every client,
    // every layout): the key reads the poll as TeamSpeak shows it, not the card's summary.
    void pollMessagesKeepTheirReactionKey()
    {
        for (const bool styled : {false, true}) {
            QTextDocument doc;
            setup(doc);
            const tschat::Skin s    = tschat::darkSkin();
            const polls::Poll  poll = samplePoll();
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:05:00"), 19, kBob, QStringLiteral("Bob"), pollHtml(s, poll)));
            const textreactdoc::Identity before = textreactdoc::identify(doc.firstBlock());
            QVERIFY(before.eligible());
            if (styled)
                QVERIFY(layoutdoc::styleAll(&doc, envFor(true, true)) > 0);
            const QVector<polldoc::Found> found = polldoc::scan(&doc);
            QCOMPARE(found.size(), 1);
            QVERIFY(polldoc::collapse(&doc, found.first(), polldoc::objectPrefix() + QStringLiteral("w1"), QSizeF(260, 200)));
            polldoc::normalizeAll(&doc);
            const textreactdoc::Identity after = textreactdoc::identify(doc.firstBlock());
            QVERIFY(after.eligible());
            QCOMPARE(after.hash, before.hash);
            QVERIFY(!after.media);
        }
    }

    // The bars above the chat input with TeamSpeak's layout spacing between them: the music bar on top, the
    // reply bar right above the input, the hold strip floating on the stack, as wide as the bars.
    void barsStackWithLayoutSpacing()
    {
        QWidget window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(560, 360);
        auto* column = new QVBoxLayout(&window);
        column->setSpacing(4);
        auto* chat   = new QTextBrowser(&window);
        auto* row    = new QHBoxLayout;
        auto* input  = new ChatLineEdit23(&window);
        input->setFixedHeight(30);
        auto* button = new QToolButton(&window);
        row->addWidget(input, 1);
        row->addWidget(button);
        column->addWidget(chat, 1);
        column->addLayout(row);
        window.show();
        auto* music = new MusicBar;
        QVERIFY(inputbars::insertAbove(input, music));
        music->show();
        auto* reply = new QWidget(&window); // ChatReplies' bar: right above the input's row
        reply->setObjectName(QStringLiteral("tsmediaReplyBar"));
        reply->setFixedHeight(28);
        column->insertWidget(column->count() - 1, reply);
        reply->show();
        column->activate();
        QCoreApplication::processEvents();
        const auto rectIn = [&](QWidget* w) { return QRect(w->mapTo(&window, QPoint(0, 0)), w->size()); };
        QVERIFY(rectIn(music).bottom() < rectIn(reply).top());
        QVERIFY(rectIn(reply).bottom() < rectIn(input).top());
        QRect     top;
        const int edge = inputbars::stackTop(input, &window, nullptr, &top);
        QCOMPARE(edge, rectIn(music).top()); // through both gaps
        QCOMPARE(top, rectIn(music));
        HoldStrip strip;
        strip.placeAbove(input);
        QCOMPARE(strip.geometry().bottom() + 1, rectIn(music).top());
        QCOMPARE(strip.geometry().left(), rectIn(music).left());
        QCOMPARE(strip.geometry().width(), rectIn(music).width());
        // Without bars: as wide as the input, right on it.
        delete music;
        delete reply;
        column->activate();
        QCoreApplication::processEvents();
        QCOMPARE(inputbars::stackTop(input, &window, &strip, &top), rectIn(input).top());
        QVERIFY(top.isEmpty());
        strip.placeAbove(input);
        QCOMPARE(strip.geometry().width(), input->width());
    }

    // The send window's presence line: "Who?" is one of the window's chips (never the skin's underlined link),
    // a click doesn't take the focus, and opening its list makes the window taller instead of squeezing it.
    void presenceWhoButtonAndHeight()
    {
        compose::setPresenceLineFactory([](QWidget* parent, const ChatTarget&) -> QWidget* {
            auto* line = new QWidget(parent);
            line->setObjectName(QStringLiteral("tsmediaPresenceLine"));
            auto* box  = new QVBoxLayout(line);
            box->setContentsMargins(0, 0, 0, 0);
            auto* text = new QLabel(QStringLiteral("3 of 5 people here will see it in the chat."), line);
            auto* who  = new QToolButton(line);
            who->setText(QStringLiteral("Who?"));
            who->setCheckable(true);
            who->setStyleSheet(QStringLiteral("QToolButton { color: palette(link); text-decoration: underline; }"));
            auto* list = new QLabel(QStringLiteral("Alice\nBob\nCarol\nDave\nErin"), line);
            list->setVisible(false);
            QObject::connect(who, &QToolButton::toggled, list, &QLabel::setVisible);
            box->addWidget(text);
            box->addWidget(who);
            box->addWidget(list);
            return line;
        });
        ComposeHost host;
        host.describeTarget  = [](const ChatTarget&) { return QStringLiteral("the channel \"Lobby\""); };
        host.blocker         = [](const ChatTarget&) { return QString(); };
        host.uploadLimitMB   = [](const ChatTarget&) { return 100; };
        host.linkContext     = [](const ChatTarget&) { return compose::LinkContext(); };
        host.savePastedImage = [](const QImage&) { return QString(); };
        host.send            = [](const SendRequest&) { return 1; };
        host.rememberAlbum   = [](bool) {};
        ChatTarget target;
        target.sch  = 1;
        target.mode = TextMessageTarget_CHANNEL;
        // One picture: its large preview is what gets lower when the window can't grow any more (a small screen).
        QTemporaryDir tmp;
        QImage        picture(1600, 1200, QImage::Format_RGB32);
        picture.fill(QColor(0x3b, 0x82, 0xf6));
        const QString path = tmp.path() + QStringLiteral("/lake.png");
        QVERIFY(picture.save(path));
        auto* d = new ComposeDialog(host, target, nullptr);
        d->setAttribute(Qt::WA_DontShowOnScreen);
        d->addFiles(QStringList{path});
        d->show();
        QTRY_VERIFY(!d->isBusy());
        QTest::qWait(100);
        QToolButton* who = nullptr;
        for (QToolButton* b : d->findChildren<QToolButton*>()) {
            if (b->text() == QLatin1String("Who?"))
                who = b;
        }
        QVERIFY(who);
        QVERIFY(!who->styleSheet().contains(QLatin1String("palette(link)")));
        QVERIFY(!who->styleSheet().contains(QLatin1String("underline;")));
        QVERIFY(who->styleSheet().contains(QLatin1String("border-radius")));
        QCOMPARE(who->focusPolicy(), Qt::TabFocus);
        QWidget* stage = nullptr;
        for (QWidget* w : d->findChildren<QWidget*>(QStringLiteral("tsmComposeTile"))) {
            if (w->isVisible())
                stage = w;
        }
        QVERIFY(stage);
        const int before      = d->height();
        const int stageBefore = stage->height();
        who->setChecked(true);
        // Taller; or, at the screen's limit (the offscreen screen is small), the same height with the preview
        // lower: the window follows the presence line instead of keeping a height that squeezes its rows.
        QTRY_VERIFY2(d->height() > before || stage->height() < stageBefore,
                     qPrintable(QStringLiteral("window %1 (was %2), preview %3 (was %4)").arg(d->height()).arg(before).arg(stage->height()).arg(stageBefore)));
        who->setChecked(false);
        QTRY_VERIFY2(d->height() <= before && stage->height() >= stageBefore, // and back (at least as roomy)
                     qPrintable(QStringLiteral("window %1 (was %2), preview %3 (was %4)").arg(d->height()).arg(before).arg(stage->height()).arg(stageBefore)));
        delete d;
        compose::setPresenceLineFactory({});
    }
};

TSMEDIA_REGISTER_TEST(TestWiring23)
#include "tst_wiring23.moc"
