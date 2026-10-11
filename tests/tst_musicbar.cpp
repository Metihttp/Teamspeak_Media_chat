// 2.3 live music: what the user sees and does, in TeamSpeak-like windows that are never on the screen, with
// generated sound (musicfakes.h: nothing is played, no microphone).
//   the bar      its parts at every width (what goes first when narrow, nothing overlapping), the states
//                (loading, playing, paused, a problem line, a notice), the controls' signals (play, stop,
//                drag and keys on the seek slider, volume, mute, the wheel), never taking the focus,
//                contrast in light and dark;
//   the stack    the music bar in TeamSpeak's layout above the input and above the reply bar, also through
//                a row, the hold strip floating right on top of it, everything given back when deleted;
//   controller   start (refused without a connection, a channel, talk power), the bar only on its own
//                server tab, the tags made safe, problems (muted microphone, sound, no capture device),
//                push-to-talk kept open and given back (and given up when TeamSpeak keeps closing it), the
//                voice never slipping out while the input opens or closes, muted in TeamSpeak: not sent,
//                pause / seek / volume (remembered), stop, the track's end, a channel switch, a
//                disconnect, a file that can't be played, the paperclip menu's entry, shutdown;
//                2.3 cross review: voice activation never taken for push-to-talk (a tab coming back), the
//                sound muted: not played here either, Stop fades out for the channel;
//                2.3 live test: paused while another server tab is in front and played on back home (not
//                after the user's own pause or Stop), Play in the compact bar goes back to its tab, voice
//                activation and push-to-talk given back across those pauses, TeamSpeak's server tab bar.

#include <QBoxLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWheelEvent>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "attachmenu.h"
#include "holdstrip.h"
#include "inputbars.h"
#include "music/musicbar.h"
#include "music/musiccontroller.h"
#include "music/musicmix.h"
#include "musicfakes.h"
#include "servertabs.h"
#include "testmain.h"
#include "uiutil.h"
#include "video/mfvideo.h"

namespace {

using musicfakes::FakeVoice;
using musicfakes::GenSource;
using Mode = MusicBar::Mode;

class ChatLineEdit : public QTextEdit // TeamSpeak's chat input class name
{
  public:
    using QTextEdit::QTextEdit;
};

// A TeamSpeak-like chat window: the chat, then the input, in one vertical layout.
struct Window {
    QWidget                window;
    QTextBrowser*          browser = nullptr;
    QPointer<ChatLineEdit> input;
    explicit Window(bool dark = false, int width = 600)
    {
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        browser = new QTextBrowser(&window);
        input   = new ChatLineEdit(&window);
        input->setFixedHeight(30);
        QPalette pal = input->palette();
        pal.setColor(QPalette::Base, dark ? QColor(0x38, 0x3a, 0x40) : Qt::white);
        pal.setColor(QPalette::Text, dark ? QColor(0xdb, 0xde, 0xe1) : QColor(0x31, 0x33, 0x38));
        input->setPalette(pal);
        layout->addWidget(browser, 1);
        layout->addWidget(input);
        window.resize(width, 360);
        window.show();
    }
    QVBoxLayout* box() const { return static_cast<QVBoxLayout*>(window.layout()); }
};

MusicBar::View playing(const QString& title = QStringLiteral("Morning Walk"))
{
    MusicBar::View v;
    v.mode       = Mode::Playing;
    v.title      = title;
    v.artist     = QStringLiteral("Alice");
    v.positionMs = 83000;
    v.durationMs = 225000;
    v.volume     = 50;
    v.animate    = false;
    return v;
}

void mouse(QWidget* w, QEvent::Type type, const QPoint& at, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, at, w->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

void click(QWidget* w)
{
    const QPoint c = w->rect().center();
    mouse(w, QEvent::MouseButtonPress, c, Qt::LeftButton);
    mouse(w, QEvent::MouseButtonRelease, c, Qt::NoButton);
}

// 2.3 live test: TeamSpeak's server tabs for servertabs: the connections in TeamSpeak's order, their servers'
// names, and which connection each tab of the server tab widget is (TeamSpeak's current one follows its tab).
struct FakeTabs {
    static inline QList<quint64>             list;
    static inline QHash<quint64, QByteArray> names{{11, "Weekend Squad"}, {12, "Night & Day"}, {13, "Weekend Squad"}};
    static inline QList<quint64>             tabConnection;
    static inline QPointer<QTabWidget>       tabs;

    static unsigned int handlers(uint64** result)
    {
        auto* out = static_cast<uint64*>(std::malloc((list.size() + 1) * sizeof(uint64)));
        for (int i = 0; i < list.size(); ++i)
            out[i] = list.at(i);
        out[list.size()] = 0;
        *result          = out;
        return ERROR_ok;
    }
    static unsigned int serverString(uint64 sch, size_t flag, char** result)
    {
        if (flag != VIRTUALSERVER_NAME || !names.contains(sch))
            return ERROR_parameter_invalid;
        const QByteArray name = names.value(sch);
        *result               = static_cast<char*>(std::malloc(static_cast<size_t>(name.size()) + 1)); // freeMemory: std::free
        std::memcpy(*result, name.constData(), static_cast<size_t>(name.size()) + 1);
        return ERROR_ok;
    }
    static uint64 current() { return tabs ? tabConnection.value(tabs->currentIndex()) : 0; }
    static void   install(QTabWidget* widget, const QList<quint64>& connections, const QList<quint64>& order)
    {
        tabs          = widget;
        list          = connections;
        tabConnection = order;
        ts3::funcs.getServerConnectionHandlerList      = &handlers;
        ts3::funcs.getServerVariableAsString           = &serverString;
        ts3::funcs.getCurrentServerConnectionHandlerID = &current;
    }
};

bool overlaps(const MusicBar::Geometry& g)
{
    QVector<QRect> shown{g.badge, g.title, g.play, g.stop};
    if (g.showElapsed)
        shown << g.elapsed;
    if (g.showSeek)
        shown << g.seek.adjusted(4, 0, 0, 0); // the slider's thumb room reaches into the gaps
    if (g.showTotal)
        shown << g.total;
    if (g.showVolumeButton)
        shown << g.volumeButton;
    if (g.showVolumeSlider)
        shown << g.volumeSlider;
    for (int i = 0; i < shown.size(); ++i)
        for (int j = i + 1; j < shown.size(); ++j)
            if (shown[i].intersects(shown[j]))
                return true;
    return false;
}

} // namespace

class TestMusicBar : public QObject
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
        for (const attachmenu::Entry& e : attachmenu::entries()) // (the shared contract has no clear())
            attachmenu::removeEntry(e.id);
        m_track = m_dir->filePath(QStringLiteral("Morning Walk.mp3"));
        QFile f(m_track);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("generated in the test"); // the controller only checks it is there; the sound is generated
    }
    void cleanup() { QVERIFY(!music::active()); }

    // ---- the bar --------------------------------------------------------------------------------------------

    void partsAtEveryWidth()
    {
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        const MusicBar::View v = playing();
        MusicBar::Geometry   wide = MusicBar::geometryFor(QSize(900, 40), font, v);
        QVERIFY(wide.showElapsed && wide.showSeek && wide.showTotal && wide.showVolumeButton && wide.showVolumeSlider);
        QVERIFY(wide.seek.width() > 200);
        int lastWidth = 10000;
        // Narrower: the title keeps its room first (up to 120 px); then the volume slider goes, the total,
        // the volume button, the seek slider, and last the elapsed time.
        for (int width = 900; width >= 220; width -= 10) {
            const MusicBar::Geometry g = MusicBar::geometryFor(QSize(width, 40), font, v);
            QVERIFY2(!overlaps(g), qPrintable(QString::number(width)));
            QVERIFY2(g.stop.right() < width && g.badge.left() >= 0, qPrintable(QString::number(width)));
            QVERIFY(g.title.width() >= 64);
            QVERIFY(g.play.right() < g.stop.left());
            if (!g.showElapsed)
                QVERIFY(!g.showSeek && !g.showVolumeButton && !g.showTotal && !g.showVolumeSlider);
            if (!g.showSeek)
                QVERIFY(!g.showVolumeButton && !g.showTotal && !g.showVolumeSlider);
            if (!g.showVolumeButton)
                QVERIFY(!g.showTotal && !g.showVolumeSlider);
            if (!g.showTotal)
                QVERIFY(!g.showVolumeSlider);
            if (g.showSeek && g.showTotal) // the track sits as far from both times
                QCOMPARE(g.seek.left() + 7 - (g.elapsed.right() + 1), g.total.left() - (g.seek.right() + 1 - 7));
            lastWidth = width;
        }
        QCOMPARE(lastWidth, 220);
        const MusicBar::Geometry narrow = MusicBar::geometryFor(QSize(260, 40), font, v);
        QVERIFY(!narrow.showVolumeSlider && !narrow.showVolumeButton);
        const MusicBar::Geometry mid = MusicBar::geometryFor(QSize(380, 40), font, v);
        QVERIFY(mid.showSeek && mid.showElapsed);
        // The title isn't cut first (up to 120 px of it).
        QVERIFY(mid.title.width() >= qMin(120, static_cast<int>(std::ceil(QFontMetricsF(font).horizontalAdvance(v.title)))));
        // A notice too long for one line wraps (three lines at most) instead of being cut.
        MusicBar wrap;
        MusicBar::View longNote;
        longNote.mode   = Mode::Notice;
        longNote.notice = QStringLiteral("Couldn't play “Holiday Mix”: Ogg and Opus audio need an extension on this computer. Install “Web Media Extensions” from the Microsoft Store.");
        wrap.setView(longNote);
        QVERIFY(wrap.hasHeightForWidth());
        QVERIFY(wrap.heightForWidth(300) > wrap.heightForWidth(20000));
        QVERIFY(wrap.heightForWidth(120) <= wrap.heightForWidth(300) + 40); // never more than three lines
        QCOMPARE(wrap.heightForWidth(20000), MusicBar::heightFor(wrap.font(), false)); // one line: a plain row
        // Unknown length: no seeking, no total.
        MusicBar::View live = v;
        live.durationMs     = 0;
        const MusicBar::Geometry g = MusicBar::geometryFor(QSize(900, 40), font, live);
        QVERIFY(!g.showSeek && !g.showTotal && g.showElapsed);
        // A notice: one line and the close button.
        MusicBar::View note;
        note.mode   = Mode::Notice;
        note.notice = QStringLiteral("The music stopped because you left the channel.");
        const MusicBar::Geometry n = MusicBar::geometryFor(QSize(500, 40), font, note);
        QVERIFY(n.close.right() < 500 && n.title.right() < n.close.left());
    }

    void statesAndControls()
    {
        Window   w;
        MusicBar bar;
        QVERIFY(inputbars::insertAbove(w.input, &bar));
        bar.setTheme(false, Qt::white, w.input->font());
        bar.setView(playing());
        bar.show();
        QCoreApplication::processEvents();
        QCOMPARE(bar.objectName(), QStringLiteral("tsmediaMusicBar"));
        QCOMPARE(bar.playButton()->accessibleName(), QStringLiteral("Pause"));
        QVERIFY(bar.playButton()->isEnabled());
        QVERIFY(bar.seekSlider()->isVisible() && bar.seekSlider()->isEnabled());
        QVERIFY(!bar.closeButton()->isVisible());
        // No control takes the focus on a click (the chat input keeps it); Tab reaches them all.
        for (QWidget* c : {bar.playButton(), bar.stopButton(), bar.seekSlider(), bar.volumeSlider(), bar.volumeButton(), bar.closeButton()})
            QCOMPARE(c->focusPolicy(), Qt::TabFocus);
        QVERIFY(!bar.isWindow());
        QVERIFY(!bar.testAttribute(Qt::WA_QuitOnClose));

        QSignalSpy play(&bar, &MusicBar::playPauseClicked), stop(&bar, &MusicBar::stopClicked), seek(&bar, &MusicBar::seekRequested),
            volume(&bar, &MusicBar::volumeChanged), mute(&bar, &MusicBar::muteToggled), closed(&bar, &MusicBar::noticeClosed);
        click(bar.playButton());
        QCOMPARE(play.count(), 1);
        QTest::keyClick(bar.playButton(), Qt::Key_Return);
        QTRY_COMPARE(play.count(), 2);
        click(bar.stopButton());
        QCOMPARE(stop.count(), 1);

        // Dragging the seek slider: the time follows, the seek comes on release, where it was let go.
        QWidget*  slider = bar.seekSlider();
        const int y      = slider->height() / 2;
        mouse(slider, QEvent::MouseButtonPress, QPoint(7, y), Qt::LeftButton);
        QVERIFY(bar.seeking());
        QCOMPARE(bar.shownPositionMs(), qint64(0));
        mouse(slider, QEvent::MouseMove, QPoint(slider->width() / 2, y), Qt::LeftButton);
        QVERIFY(qAbs(bar.shownPositionMs() - 112500) < 3000);
        bar.setView(playing()); // the music moves on meanwhile: the drag keeps its place
        QVERIFY(qAbs(bar.shownPositionMs() - 112500) < 3000);
        QCOMPARE(seek.count(), 0);
        mouse(slider, QEvent::MouseButtonRelease, QPoint(slider->width() - 7, y), Qt::NoButton);
        QCOMPARE(seek.count(), 1);
        QCOMPARE(seek.last().at(0).toLongLong(), qint64(225000));
        QVERIFY(!bar.seeking());
        // Keys: 5 s steps, Page 30 s, Home / End.
        bar.setView(playing());
        QTest::keyClick(slider, Qt::Key_Right);
        QCOMPARE(seek.last().at(0).toLongLong(), qint64(88000));
        bar.setView(playing());
        QTest::keyClick(slider, Qt::Key_Left);
        QCOMPARE(seek.last().at(0).toLongLong(), qint64(78000));
        bar.setView(playing());
        QTest::keyClick(slider, Qt::Key_PageUp);
        QCOMPARE(seek.last().at(0).toLongLong(), qint64(113000));
        QTest::keyClick(slider, Qt::Key_Home);
        QCOMPARE(seek.last().at(0).toLongLong(), qint64(0));

        // Volume: the slider, the button (mute), its wheel and keys.
        QWidget* vol = bar.volumeSlider();
        mouse(vol, QEvent::MouseButtonPress, QPoint(vol->width() - 7, vol->height() / 2), Qt::LeftButton);
        mouse(vol, QEvent::MouseButtonRelease, QPoint(vol->width() - 7, vol->height() / 2), Qt::NoButton);
        QVERIFY(volume.count() >= 1);
        QCOMPARE(volume.last().at(0).toInt(), 100);
        click(bar.volumeButton());
        QCOMPARE(mute.count(), 1);
        QWheelEvent wheel(QPointF(5, 5), bar.volumeButton()->mapToGlobal(QPoint(5, 5)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(bar.volumeButton(), &wheel);
        QCOMPARE(volume.last().at(0).toInt(), 45);
        QTest::keyClick(bar.volumeButton(), Qt::Key_Up);
        QCOMPARE(volume.last().at(0).toInt(), 55);

        // Paused, loading, a problem line, a notice.
        MusicBar::View v = playing();
        v.mode           = Mode::Paused;
        bar.setView(v);
        QCOMPARE(bar.playButton()->accessibleName(), QStringLiteral("Play"));
        const int oneLine = bar.sizeHint().height();
        QCOMPARE(bar.height(), oneLine); // what TeamSpeak's layout gave it
        v.problem         = QStringLiteral("Your microphone is muted in TeamSpeak, so the channel can't hear the music. Unmute it to share.");
        v.problemKind     = MusicBar::Problem::Warning;
        bar.setView(v);
        QVERIFY(bar.sizeHint().height() > oneLine);
        w.window.layout()->activate();
        QCOMPARE(bar.height(), bar.sizeHint().height()); // the chat above gave it a line
        QVERIFY(bar.accessibleDescription().contains(QStringLiteral("muted")));
        v.problem.clear();
        v.mode = Mode::Loading;
        bar.setView(v);
        QCOMPARE(bar.sizeHint().height(), oneLine);
        QVERIFY(!bar.playButton()->isEnabled());
        QVERIFY(!bar.seekSlider()->isEnabled());
        MusicBar::View note;
        note.mode       = Mode::Notice;
        note.notice     = QStringLiteral("Couldn't play “x”: This file has no sound to play.");
        note.noticeIcon = MusicBar::NoticeIcon::Error;
        bar.setView(note);
        QVERIFY(bar.closeButton()->isVisible());
        QVERIFY(!bar.playButton()->isVisible() && !bar.stopButton()->isVisible() && !bar.seekSlider()->isVisible());
        click(bar.closeButton());
        QCOMPARE(closed.count(), 1);
        QCOMPARE(bar.accessibleDescription(), note.notice);
        // Every state draws (light and dark).
        for (const bool dark : {false, true}) {
            bar.setTheme(dark, dark ? QColor(0x38, 0x3a, 0x40) : QColor(Qt::white), w.input->font());
            for (Mode m : {Mode::Loading, Mode::Playing, Mode::Paused}) {
                MusicBar::View s = playing();
                s.mode           = m;
                bar.setView(s);
                QVERIFY(!bar.grab().isNull());
            }
        }
        // The window gives the bar back when it is deleted (it is a child of TeamSpeak's window here).
        bar.setParent(nullptr);
    }

    void contrast()
    {
        for (const bool dark : {false, true}) {
            const MusicBar::Colors c = MusicBar::colorsFor(dark, dark ? QColor(0x38, 0x3a, 0x40) : QColor(Qt::white));
            QVERIFY(ui::contrastRatio(c.text, c.surface) >= 7.0);
            QVERIFY(ui::contrastRatio(c.muted, c.surface) >= 4.5);
            QVERIFY(ui::contrastRatio(c.error, c.surface) >= 4.5);
            QVERIFY(ui::contrastRatio(c.accent, c.surface) >= 3.0);
            QVERIFY(ui::contrastRatio(c.onAccent, c.accent) >= 3.0);
        }
        // The attach menu's icon: its own hue, readable on light and dark menus, at 24 logical px and up.
        QVERIFY(ui::contrastRatio(MusicBar::menuHue(), Qt::white) >= 3.0);
        QVERIFY(ui::contrastRatio(MusicBar::menuHue(), QColor(0x2b, 0x2d, 0x31)) >= 3.0);
        const QIcon icon = MusicBar::menuIcon();
        QVERIFY(!icon.isNull());
        QVERIFY(icon.availableSizes().contains(QSize(24, 24)) && icon.availableSizes().contains(QSize(48, 48)));
        const QImage note = icon.pixmap(QSize(48, 48)).toImage();
        int          inked = 0;
        for (int y = 0; y < note.height(); ++y)
            for (int x = 0; x < note.width(); ++x)
                if (qAlpha(note.pixel(x, y)) == 255) { // solid ink: the hue, nothing else (edges are antialiased)
                    ++inked;
                    const QRgb px = note.pixel(x, y), hue = MusicBar::menuHue().rgb();
                    QVERIFY(qAbs(qRed(px) - qRed(hue)) <= 2 && qAbs(qGreen(px) - qGreen(hue)) <= 2 && qAbs(qBlue(px) - qBlue(hue)) <= 2);
                }
        QVERIFY(inked > 48 * 48 / 20); // a note, not a speck
    }

    // 2.3 ux review: the badge's note reads at 3:1 on its own tinted tile (not only on the bar), as drawn.
    void noteReadableOnItsTile()
    {
        QStringList low;
        for (const bool dark : {false, true}) {
            Window   w(dark);
            MusicBar bar;
            QVERIFY(inputbars::insertAbove(w.input, &bar));
            bar.setTheme(dark, w.input->palette().color(QPalette::Base), w.input->font());
            MusicBar::View v = playing();
            v.live           = true;
            bar.setView(v);
            bar.show();
            QCoreApplication::processEvents();
            QImage img(bar.size() * 2, QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(2.0);
            img.fill(Qt::transparent);
            bar.render(&img);
            const QRect  badge = bar.parts().badge;
            const QColor tile  = img.pixelColor(2 * badge.left() + 4, 2 * badge.center().y()); // inside the tile, left of the note
            double       best  = 1.0;
            for (int y = 2 * badge.top(); y < 2 * (badge.bottom() + 1); ++y)
                for (int x = 2 * badge.left(); x < 2 * (badge.right() + 1); ++x)
                    best = qMax(best, ui::contrastRatio(img.pixelColor(x, y), tile));
            if (best < 3.0)
                low << QStringLiteral("%1: %2").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")).arg(best);
        }
        QVERIFY2(low.isEmpty(), qPrintable(low.join(QStringLiteral("; "))));
    }

    void wheelOnThePositionDoesntMoveIt()
    {
        Window   w;
        MusicBar bar;
        QVERIFY(inputbars::insertAbove(w.input, &bar));
        bar.setTheme(false, Qt::white, w.input->font());
        bar.setView(playing());
        bar.show();
        QCoreApplication::processEvents();
        QSignalSpy seek(&bar, &MusicBar::seekRequested), volume(&bar, &MusicBar::volumeChanged);
        auto* slider = qobject_cast<QAbstractSlider*>(bar.seekSlider());
        QVERIFY(slider);
        const int before = slider->value();
        QWheelEvent wheel(QPointF(10, 5), slider->mapToGlobal(QPoint(10, 5)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(slider, &wheel);
        QCOMPARE(slider->value(), before); // no thumb jumping and snapping back two ticks later
        QCOMPARE(seek.count(), 0);
        // The volume slider still takes the wheel.
        QWidget*    vol = bar.volumeSlider();
        QWheelEvent up(QPointF(10, 5), vol->mapToGlobal(QPoint(10, 5)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(vol, &up);
        QVERIFY(volume.count() >= 1);
        QVERIFY(volume.last().at(0).toInt() > 50);
        bar.setParent(nullptr);
    }

    // ---- the stack above the input ---------------------------------------------------------------------------

    void barsStackAboveTheInput()
    {
        Window w;
        auto*  music = new MusicBar;
        QVERIFY(inputbars::insertAbove(w.input, music));
        QCOMPARE(w.box()->indexOf(music), 1);
        QCOMPARE(w.box()->indexOf(w.input), 2);
        // The reply bar goes right above the input (chatreplies.cpp inserts at the input's index): the
        // music stays on top of it.
        auto* reply = new QWidget;
        reply->setObjectName(QStringLiteral("tsmediaReplyBar"));
        reply->setFixedHeight(28);
        w.box()->insertWidget(w.box()->indexOf(w.input), reply);
        QCOMPARE(w.box()->indexOf(music), 1);
        QCOMPARE(w.box()->indexOf(reply), 2);
        // Again: stays where it is. A bar of ours added later goes on top.
        QVERIFY(inputbars::insertAbove(w.input, music));
        QCOMPARE(w.box()->indexOf(music), 1);
        auto* later = new QWidget;
        later->setObjectName(QStringLiteral("tsmediaLaterBar"));
        later->setFixedHeight(20);
        QVERIFY(inputbars::insertAbove(w.input, later));
        QCOMPARE(w.box()->indexOf(later), 1);
        QCOMPARE(w.box()->indexOf(music), 2);
        delete later;
        music->setView(playing());
        music->show();
        reply->show();
        w.window.layout()->activate();
        QCoreApplication::processEvents();
        // The hold strip floats right on top of everything stacked on the input.
        const int inputTop = w.input->mapTo(&w.window, QPoint(0, 0)).y();
        QCOMPARE(inputbars::stackTop(w.input, &w.window), music->mapTo(&w.window, QPoint(0, 0)).y());
        QVERIFY(inputbars::stackTop(w.input, &w.window) < inputTop - 28);
        HoldStrip strip(&w.window);
        HoldStrip::View hold;
        hold.mode    = HoldStrip::Mode::Notice;
        hold.text    = QStringLiteral("Hold to record, release to send");
        hold.animate = false;
        strip.setView(hold);
        strip.placeAbove(w.input);
        strip.show();
        QCOMPARE(strip.geometry().bottom() + 1, music->mapTo(&w.window, QPoint(0, 0)).y());
        QCOMPARE(inputbars::stackTop(w.input, &w.window, &strip), music->mapTo(&w.window, QPoint(0, 0)).y()); // the strip itself doesn't count
        // The music bar hidden: the strip sits on the reply bar; both hidden: on the input.
        music->hide();
        w.window.layout()->activate();
        strip.placeAbove(w.input);
        QCOMPARE(strip.geometry().bottom() + 1, reply->mapTo(&w.window, QPoint(0, 0)).y());
        reply->hide();
        w.window.layout()->activate();
        strip.placeAbove(w.input);
        QCOMPARE(strip.geometry().bottom() + 1, w.input->mapTo(&w.window, QPoint(0, 0)).y());
        // Deleted: TeamSpeak's layout is as before.
        delete music;
        delete reply;
        QCOMPARE(w.box()->count(), 2);
        QCOMPARE(w.box()->indexOf(w.input), 1);
    }

    void barThroughARowAndWithoutALayout()
    {
        // The input in a row (TeamSpeak's input with its buttons): the bar goes into the column above the row.
        QWidget window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* column = new QVBoxLayout(&window);
        auto* chat   = new QTextBrowser(&window);
        auto* row    = new QHBoxLayout;
        auto* input  = new ChatLineEdit(&window);
        auto* button = new QWidget(&window);
        row->addWidget(input, 1);
        row->addWidget(button);
        column->addWidget(chat, 1);
        column->addLayout(row);
        auto* bar = new MusicBar;
        QVERIFY(inputbars::insertAbove(input, bar));
        QCOMPARE(column->indexOf(bar), 1);
        QCOMPARE(row->indexOf(bar), -1);
        // A bottom-to-top column: the bar still ends up above.
        QWidget flipped;
        auto*   up    = new QBoxLayout(QBoxLayout::BottomToTop, &flipped);
        auto*   input2 = new ChatLineEdit(&flipped);
        up->addWidget(input2);
        up->addWidget(new QTextBrowser(&flipped));
        auto* bar2 = new MusicBar;
        QVERIFY(inputbars::insertAbove(input2, bar2));
        QCOMPARE(up->indexOf(bar2), 1);
        // No layout at all: false (the caller floats it).
        QWidget bare;
        auto*   loose = new ChatLineEdit(&bare);
        MusicBar floating;
        QVERIFY(!inputbars::insertAbove(loose, &floating));
        QVERIFY(!inputbars::insertAbove(nullptr, &floating));
        loose->setGeometry(0, 200, 300, 30);
        const int h = floating.sizeHint().height();
        inputbars::placeOverlay(loose, &floating, h); // (never shown: it would be a window of its own before)
        QCOMPARE(floating.parentWidget(), &bare);
        QCOMPARE(floating.geometry(), QRect(0, 200 - h, 300, h));
        floating.setParent(nullptr);
    }

    // ---- the controller ------------------------------------------------------------------------------------

    void startsShowsAndStops()
    {
        Window w;
        QString lines;
        for (int i = 0; i < 80; ++i)
            lines += QStringLiteral("<p>Bob: line %1</p>").arg(i);
        w.browser->setHtml(lines);
        QCoreApplication::processEvents();
        QScrollBar* scroll = w.browser->verticalScrollBar();
        scroll->setValue(scroll->maximum()); // the chat shows its end
        MusicController c(host(&w));
        QVERIFY(c.start(m_track));
        QVERIFY(c.playing());
        QCOMPARE(c.connection(), fakets3::kConnection);
        QTRY_COMPARE(scroll->value(), scroll->maximum()); // shorter now, still at its end
        QVERIFY(scroll->maximum() > 0);
        QVERIFY(music::active() == c.engine());
        QPointer<MusicBar> bar = c.bar();
        QVERIFY(bar && bar->isVisible());
        QCOMPARE(w.box()->indexOf(bar), 1); // in TeamSpeak's layout, above the input
        QCOMPARE(bar->view().title, QStringLiteral("Morning Walk")); // the file's name until its tags are read
        playFor(c, 400);
        QCOMPARE(bar->view().mode, Mode::Playing);
        QCOMPARE(bar->view().title, QStringLiteral("Morning Walk")); // no tags
        QVERIFY(bar->view().live);
        QVERIFY(bar->view().problem.isEmpty());
        // Pause and play.
        click(bar->playButton());
        QCOMPARE(bar->view().mode, Mode::Paused);
        click(bar->playButton());
        QCOMPARE(bar->view().mode, Mode::Playing);
        // Stop: the music and the bar go, the callbacks are detached.
        click(bar->stopButton());
        QVERIFY(!c.playing());
        QVERIFY(!music::active());
        QVERIFY(bar && bar->isHidden());
    }

    void tagsAreMadeSafe()
    {
        Window              w;
        MusicController::Host h = host(&w);
        h.source = [](const QString&) {
            GenSource::Options o;
            // A right-to-left override, a line break, a right-to-left mark, an isolate: none of them may reach the bar.
            o.title  = QStringLiteral("Ev") + QChar(0x202E) + QStringLiteral("il\nSong") + QChar(0x200F) + QStringLiteral("  ") + QString(300, QLatin1Char('x'));
            o.artist = QStringLiteral("Bob") + QChar(0x2066);
            return musicfakes::gen(o);
        };
        MusicController c(h);
        QVERIFY(c.start(m_track));
        playFor(c, 300);
        const QString title = c.bar()->view().title;
        QVERIFY2(title.startsWith(QStringLiteral("Evil Song")), qPrintable(title));
        QVERIFY(title.size() <= 120);
        QVERIFY(!title.contains(QChar(0x202E)) && !title.contains(QLatin1Char('\n')));
        QCOMPARE(c.bar()->view().artist, QStringLiteral("Bob"));
        c.stop();
    }

    void refusedWhereItCantPlay()
    {
        Window          w;
        MusicController c(host(&w));
        // Not connected.
        fakets3::setConnected(false);
        QVERIFY(!c.start(m_track));
        QVERIFY(!c.playing());
        QCOMPARE(c.bar()->view().mode, Mode::Notice);
        QVERIFY(c.bar()->view().notice.contains(QStringLiteral("Connect to a server")));
        QVERIFY(c.bar()->isVisible());
        click(c.bar()->closeButton());
        QVERIFY(c.bar()->isHidden());
        fakets3::setConnected(true);
        // No talk power in the channel.
        FakeVoice::neededPower = 50;
        FakeVoice::talkPower   = 10;
        QVERIFY(!c.start(m_track));
        QVERIFY(c.bar()->view().notice.contains(QStringLiteral("talk power")));
        FakeVoice::talker = 1; // a talker may anyway
        QVERIFY(c.start(m_track));
        c.stop();
        FakeVoice::talker = 0;
        FakeVoice::talkPower = 60;
        QVERIFY(c.start(m_track));
        c.stop();
        // The file is gone.
        QVERIFY(!c.start(m_dir->filePath(QStringLiteral("gone.mp3"))));
        QVERIFY(!c.playing());
        QVERIFY(c.bar()->view().notice.contains(QStringLiteral("isn't there")));
    }

    // 2.3: on another server tab (TeamSpeak shows its chat above the same input) the bar stays, compact, says
    // where the music plays (the names sanitized: no controls, bidi marks or line breaks, short) and (live
    // test) that it waits there, on a line of its own; Play's tool tip says it goes back. Back on its own tab
    // it is the full bar again. A notice belongs to its own tab.
    void barCompactOnAnotherServerTab()
    {
        Window                w;
        MusicController::Host h = host(&w);
        h.placeName             = [](quint64 sch, quint64) {
            return sch == fakets3::kConnection ? QStringLiteral("Weekend\nSquad \u202Eevil\u202C \u203A Lobby") + QString(200, QLatin1Char('x')) : QString();
        };
        MusicController c(h);
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.bar()->isVisible());
        QVERIFY(c.bar()->view().away.isEmpty());
        QVERIFY(c.bar()->parts().showSeek);
        const int fullHeight = c.bar()->height();
        FakeVoice::visible = 2; // another server tab
        c.serverTabChanged(2);
        QVERIFY(c.bar()->isVisible()); // at once, compact
        QVERIFY(c.playing());          // still its music, on its own connection
        QVERIFY(!c.inputKeptOpen());   // TeamSpeak's input is the other tab's now
        const QString away = c.bar()->view().away;
        QVERIFY2(away.startsWith(QStringLiteral("Weekend Squad")), qPrintable(away));
        QVERIFY(!away.contains(QChar(0x202E)) && !away.contains(QLatin1Char('\n')));
        QVERIFY(away.size() <= 64);
        // Live test: it waits there (TeamSpeak sends only from the tab in front), and says so.
        QCOMPARE(c.bar()->view().mode, Mode::Paused);
        QVERIFY2(c.bar()->view().problem.startsWith(QStringLiteral("Paused while you're on another server tab")), qPrintable(c.bar()->view().problem));
        QCOMPARE(c.bar()->view().problemKind, MusicBar::Problem::Info);
        QVERIFY(c.bar()->accessibleDescription().startsWith(QStringLiteral("Paused: Morning Walk on Weekend Squad")));
        QVERIFY2(c.bar()->playButton()->toolTip().startsWith(QStringLiteral("Back to Weekend Squad")), qPrintable(c.bar()->playButton()->toolTip()));
        w.window.layout()->activate();
        QCOMPARE(c.bar()->height(), c.bar()->sizeHint().height()); // the line has its room
        QVERIFY(c.bar()->height() > fullHeight);
        const MusicBar::Geometry& g = c.bar()->parts();
        QVERIFY(!g.showSeek && !g.showVolumeSlider && !g.showVolumeButton && !g.showElapsed);
        QVERIFY(c.bar()->playButton()->isVisible() && c.bar()->stopButton()->isVisible());
        QVERIFY(g.title.right() < g.play.left() && g.play.right() < g.stop.left());
        QVERIFY(!g.problem.isEmpty() && g.problem.top() > g.play.center().y() && g.problem.bottom() < c.bar()->height());
        QVERIFY(!c.bar()->grab().isNull());
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QVERIFY(c.bar()->isVisible());
        QVERIFY(c.bar()->view().away.isEmpty()); // the full bar again
        QVERIFY(c.bar()->parts().showSeek);
        QCOMPARE(c.bar()->view().mode, Mode::Playing);
        QVERIFY(c.bar()->view().problem.isEmpty());
        QCOMPARE(c.bar()->playButton()->toolTip(), QStringLiteral("Pause"));
        // A notice belongs to its tab too.
        c.connectionLost(fakets3::kConnection);
        QVERIFY(!c.playing());
        QCOMPARE(c.bar()->view().mode, Mode::Notice);
        QVERIFY(c.bar()->isVisible());
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QVERIFY(c.bar()->isHidden());
    }

    // 2.3 live test: TeamSpeak sends the voice of the server tab in front only, so the track went on unheard
    // (the channel missed that part) while the user looked at another server tab. Now it pauses there by
    // itself and plays on from the same place once its tab is in front again; not when the user paused or
    // stopped it meanwhile.
    void pausesForAnotherServerTabAndPlaysOnBack()
    {
        Window          w;
        MusicController c(host(&w));
        QVERIFY(c.start(m_track));
        playFor(c, 300);
        const auto state = [&c] { return c.engine()->snapshot().state; };
        QCOMPARE(state(), music::Engine::State::Playing);
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(c.waitingForItsTab());
        const qint64 at = c.engine()->snapshot().positionMs;
        QVERIFY(at > 0);
        playFor(c, 300); // TeamSpeak's audio threads go on meanwhile: the place stays, nothing is mixed in
        QCOMPARE(c.engine()->snapshot().positionMs, at);
        std::vector<qint16> block(960, 500);
        int                 edited = 0;
        music::onCaptured(fakets3::kConnection, block.data(), 960, 1, &edited);
        QCOMPARE(edited, 0);
        QCOMPARE(int(block[100]), 500);
        FakeVoice::visible = 3; // a third tab: still waiting
        c.serverTabChanged(3);
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(c.waitingForItsTab());
        // Back: it plays on from where it was.
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Playing);
        QVERIFY(!c.waitingForItsTab());
        QVERIFY(c.bar()->view().away.isEmpty() && c.bar()->view().problem.isEmpty());
        playFor(c, 200);
        const qint64 later = c.engine()->snapshot().positionMs;
        QVERIFY2(later > at && later < at + 1000, qPrintable(QStringLiteral("%1 -> %2").arg(at).arg(later)));
        // Paused by the user, then another tab and back: it stays paused (and nothing says it waits).
        click(c.bar()->playButton());
        QCOMPARE(state(), music::Engine::State::Paused);
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QVERIFY(!c.waitingForItsTab());
        QVERIFY(c.bar()->view().problem.isEmpty());
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Paused);
        click(c.bar()->playButton());
        QCOMPARE(state(), music::Engine::State::Playing);
        // The tick alone sees it too (an event that never came).
        FakeVoice::visible = 2;
        playFor(c, 100);
        QCOMPARE(state(), music::Engine::State::Paused);
        FakeVoice::visible = fakets3::kConnection;
        c.refreshNow();
        QCOMPARE(state(), music::Engine::State::Playing);
        // No tab in front for a moment (TeamSpeak between tabs): nothing changes.
        FakeVoice::visible = 0;
        c.serverTabChanged(0);
        QCOMPARE(state(), music::Engine::State::Playing);
        FakeVoice::visible = fakets3::kConnection;
        // Stopped while away: nothing plays back home.
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        click(c.bar()->stopButton());
        QVERIFY(!c.playing());
        QVERIFY(!music::active());
        QVERIFY(c.bar()->isHidden());
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QVERIFY(!c.playing());
        QVERIFY(c.bar()->isHidden());
        // A new track starts on the tab in front and isn't waiting for anything.
        QVERIFY(c.start(m_track));
        playFor(c, 100);
        QCOMPARE(state(), music::Engine::State::Playing);
        QVERIFY(!c.waitingForItsTab());
        c.stop();
    }

    // 2.3 live test: Play in the compact bar on another server tab brings the music's tab to the front
    // (TeamSpeak's server tab bar, servertabs::show) and plays on there: at once when TeamSpeak says so at once,
    // else as soon as it does. If the tab can't be brought to the front, the bar says to go there, and it plays
    // on once the user does. Also after a pause of the user's own. Stop works from there.
    void playOnTheCompactBarGoesBackToItsTab()
    {
        Window                w;
        MusicController::Host h = host(&w);
        QList<quint64>        asked;
        bool                  can = true, atOnce = true;
        h.showConnection = [&](quint64 sch) {
            asked << sch;
            if (can && atOnce)
                FakeVoice::visible = sch;
            return can;
        };
        MusicController c(h);
        const auto      state = [&c] { return c.engine()->snapshot().state; };
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(asked.isEmpty()); // (going away never switches tabs)
        click(c.bar()->playButton());
        QCOMPARE(asked, QList<quint64>{fakets3::kConnection});
        QCOMPARE(state(), music::Engine::State::Playing);
        QVERIFY(c.bar()->view().away.isEmpty());
        // TeamSpeak says so a moment later: it plays on then, not before.
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        atOnce = false;
        click(c.bar()->playButton());
        QCOMPARE(asked.size(), 2);
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(c.waitingForItsTab());
        playFor(c, 100);
        QCOMPARE(state(), music::Engine::State::Paused);
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Playing);
        // It can't be brought to the front: the bar says where to go, and it plays on once the user goes there.
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        can = false;
        click(c.bar()->playButton());
        QCOMPARE(asked.size(), 3);
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY2(c.bar()->view().problem.startsWith(QStringLiteral("Go back to its server tab")), qPrintable(c.bar()->view().problem));
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Playing);
        QVERIFY(c.bar()->view().problem.isEmpty());
        // Paused by the user at home, then Play on another tab: back there, playing.
        click(c.bar()->playButton());
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QVERIFY(!c.waitingForItsTab());
        can    = true;
        atOnce = true;
        click(c.bar()->playButton());
        QCOMPARE(asked.last(), fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Playing);
        // Stop from another tab.
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        click(c.bar()->stopButton());
        QVERIFY(!c.playing());
        QCOMPARE(asked.size(), 4);
    }

    // 2.3 polish review: /tsmedia music pause and play are what they say. "pause" was a toggle: typed on another
    // server tab while the music waited there for its tab, it played the music on and took the user back to the
    // music's tab. Now the music stays paused, also back on its tab; "play" works as Play in the bar.
    void pauseAndPlayCommandsAcrossServerTabs()
    {
        Window                w;
        MusicController::Host h = host(&w);
        QList<quint64>        asked;
        h.showConnection = [&](quint64 sch) {
            asked << sch;
            FakeVoice::visible = sch; // TeamSpeak switches at once
            return true;
        };
        MusicController c(h);
        const auto      state = [&c] { return c.engine()->snapshot().state; };
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QVERIFY(c.waitingForItsTab());
        c.pause(); // "/tsmedia music pause" there
        QVERIFY(asked.isEmpty());
        QCOMPARE(FakeVoice::visible, quint64(2));
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(!c.waitingForItsTab());
        QVERIFY(c.bar()->view().problem.isEmpty()); // nothing says it plays on back there
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        playFor(c, 100);
        QCOMPARE(state(), music::Engine::State::Paused); // back home: it stays paused
        c.pause(); // never a toggle
        QCOMPARE(state(), music::Engine::State::Paused);
        c.play();
        QCOMPARE(state(), music::Engine::State::Playing);
        c.play();
        QCOMPARE(state(), music::Engine::State::Playing);
        c.pause();
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(asked.isEmpty());
        // "/tsmedia music play" on another tab: back there, playing.
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        c.play();
        QCOMPARE(asked, QList<quint64>{fakets3::kConnection});
        QCOMPARE(state(), music::Engine::State::Playing);
        // "pause" while it plays at home, then "play": as the bar's button.
        c.pause();
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(!c.waitingForItsTab());
        c.play();
        QCOMPARE(state(), music::Engine::State::Playing);
        c.stop();
        c.pause(); // nothing playing: nothing happens
        c.play();
        QVERIFY(!c.playing());
    }

    // 2.3 polish review: Play while TeamSpeak has no tab in front for a moment never plays the music on a tab
    // in the background (there it went on unheard, and coming back didn't pause it either): it waits for its tab.
    void playBetweenTabsWaitsForItsTab()
    {
        Window                w;
        MusicController::Host h = host(&w);
        int                   asked = 0;
        h.showConnection        = [&](quint64) {
            ++asked;
            return false;
        };
        MusicController c(h);
        const auto      state = [&c] { return c.engine()->snapshot().state; };
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QCOMPARE(state(), music::Engine::State::Paused);
        FakeVoice::visible = 0; // between tabs
        click(c.bar()->playButton());
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(c.waitingForItsTab());
        FakeVoice::visible = 2; // it was the other tab after all
        c.serverTabChanged(2);
        playFor(c, 100);
        QCOMPARE(state(), music::Engine::State::Paused); // never playing unheard there
        QVERIFY(c.waitingForItsTab());
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Playing);
        QVERIFY(!c.waitingForItsTab());
        // The user's own pause at home, then Play between tabs on the way back home: it plays once home.
        click(c.bar()->playButton());
        QCOMPARE(state(), music::Engine::State::Paused);
        FakeVoice::visible = 0;
        click(c.bar()->playButton());
        QCOMPARE(state(), music::Engine::State::Paused);
        FakeVoice::visible = fakets3::kConnection;
        c.refreshNow();
        QCOMPARE(state(), music::Engine::State::Playing);
        QCOMPARE(asked, 0); // (no tab in front: nothing to switch from)
        c.stop();
    }

    // 2.3 live test: voice activation the plugin turned off for a moment (TeamSpeak doesn't start sending on
    // the music's word alone) is turned on again when the music pauses for another server tab, left alone while
    // it waits there, and the music starts sending the same way back home; trips back and forth never make it
    // give up. A push-to-talk input it keeps open is given back there and opened again back home.
    void voiceActivationAcrossTabPauses()
    {
        Window          w;
        MusicController c(host(&w));
        const auto      state = [&c] { return c.engine()->snapshot().state; };
        FakeVoice::vad     = "true";
        FakeVoice::talking = 0;
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.voiceActivationPaused());
        QCOMPARE(FakeVoice::vad, QByteArray("false"));
        QCOMPARE(FakeVoice::vadOffSets, 1);
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(!c.voiceActivationPaused());
        QCOMPARE(FakeVoice::vad, QByteArray("true")); // given back at once
        playFor(c, MusicController::kVadKickGapMs + 200);
        QCOMPARE(FakeVoice::vadOffSets, 1); // left alone while it waits
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
        // Meanwhile the user's voice is TeamSpeak's own: talking goes out as it is.
        std::vector<qint16> block(2 * 960, 12000);
        int                 edited = music::kEditedSend;
        music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
        QCOMPARE(edited, int(music::kEditedSend));
        QCOMPARE(int(block[2 * 500]), 12000);
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Playing);
        playFor(c, 200);
        QCOMPARE(FakeVoice::vadOffSets, 2); // started the same way back home
        QVERIFY(c.voiceActivationPaused());
        FakeVoice::talking = 1;
        c.refreshNow();
        QVERIFY(!c.voiceActivationPaused());
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
        // Back and forth often within a few seconds: each time back home is a fresh start, never given up.
        for (int i = 0; i < MusicController::kVadKickLimit + 2; ++i) {
            FakeVoice::talking = 0;
            FakeVoice::visible = 2;
            c.serverTabChanged(2);
            QCOMPARE(FakeVoice::vad, QByteArray("true"));
            FakeVoice::visible = fakets3::kConnection;
            c.serverTabChanged(fakets3::kConnection);
            playFor(c, 60);
            QVERIFY2(c.voiceActivationPaused(), qPrintable(QString::number(i)));
        }
        for (int i = 0; i < MusicController::kCheckTicks; ++i)
            c.refreshNow();
        QVERIFY2(!c.bar()->view().problem.contains(QStringLiteral("only while you talk")), qPrintable(c.bar()->view().problem));
        c.stop();
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
        // Push-to-talk: the input it keeps open is given back while it waits, opened again back home.
        FakeVoice::vad         = "false";
        FakeVoice::talking     = 1;
        FakeVoice::deactivated = INPUT_DEACTIVATED;
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE));
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QCOMPARE(state(), music::Engine::State::Paused);
        QVERIFY(!c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QCOMPARE(state(), music::Engine::State::Playing);
        QVERIFY(c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE));
        c.shutdown();
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        QCOMPARE(FakeVoice::vad, QByteArray("false"));
    }

    // 2.3 live test: TeamSpeak's server tab bar (no chat tab bar, a tab per connection), which of its tabs is a
    // connection's (its server's name, the n-th of one name), brought to the front; with several of one name,
    // the one TeamSpeak then says is current, or back where it was.
    void serverTabsFindTheMusicsTab()
    {
        const QStringList texts{QStringLiteral("Weekend Squad"), QStringLiteral("Night && Day"), QStringLiteral("Weekend Squad"), QStringLiteral("Lan Party Ser...")};
        QCOMPARE(servertabs::indexFor(texts, QStringLiteral("Weekend Squad"), 0), 0);
        QCOMPARE(servertabs::indexFor(texts, QStringLiteral("Weekend Squad"), 1), 2);
        QCOMPARE(servertabs::indexFor(texts, QStringLiteral("Weekend Squad"), 2), -1);
        QCOMPARE(servertabs::indexFor(texts, QStringLiteral("Night & Day"), 0), 1);
        QCOMPARE(servertabs::indexFor(texts, QStringLiteral("Lan Party Server 24/7"), 0), 3); // cut short by TeamSpeak
        QCOMPARE(servertabs::indexFor(texts, QStringLiteral("Lobby"), 0), -1);
        QCOMPARE(servertabs::indexFor(texts, QString(), 0), -1);

        QMainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new QTabWidget(central);
        for (const char* name : {"Weekend Squad", "Night && Day", "Weekend Squad"})
            tabs->addTab(new QWidget(tabs), QString::fromLatin1(name));
        auto* chatTabs = new QTabBar(central); // a chat tab bar with as many tabs: never the server tabs
        chatTabs->setObjectName(QStringLiteral("ChatTabBar"));
        for (const char* name : {"Weekend Squad", "Lobby", "Bob"})
            chatTabs->addTab(QString::fromLatin1(name));
        auto* other = new QTabBar(central);
        other->addTab(QStringLiteral("Weekend Squad"));
        column->addWidget(tabs);
        column->addWidget(chatTabs);
        column->addWidget(other);
        window.setCentralWidget(central);
        window.show();
        FakeTabs::install(tabs, {11, 12, 13}, {11, 12, 13});
        QTabBar* serverBar = servertabs::bar();
        QVERIFY(serverBar && serverBar->parentWidget() == tabs);
        tabs->setCurrentIndex(0);
        QVERIFY(servertabs::show(12));
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(servertabs::show(13));
        QCOMPARE(tabs->currentIndex(), 2);
        QVERIFY(servertabs::show(11));
        QCOMPARE(ts3::currentConnection(), quint64(11));
        // Two of one name, not in the connections' order: the one TeamSpeak then says is current.
        FakeTabs::tabConnection = {13, 12, 11};
        tabs->setCurrentIndex(1);
        QVERIFY(servertabs::show(11));
        QCOMPARE(tabs->currentIndex(), 2);
        // None of them is it: back where it was.
        FakeTabs::tabConnection = {13, 12, 13};
        tabs->setCurrentIndex(1);
        QVERIFY(!servertabs::show(11));
        QCOMPARE(tabs->currentIndex(), 1);
        // A connection without a tab of its own (the counts differ), no server name, no main window: false.
        FakeTabs::tabConnection = {11, 12, 13};
        FakeTabs::list          = {11, 12, 13, 14};
        QVERIFY(!servertabs::bar());
        QVERIFY(!servertabs::show(12));
        FakeTabs::list = {11, 12, 13};
        QVERIFY(!servertabs::show(14));
        window.hide();
        QVERIFY(!servertabs::bar());
        QVERIFY(!servertabs::show(12));
        QCOMPARE(tabs->currentIndex(), 1);
    }

    // 2.3 polish review: a server tab's text is its label, not always the server's name. In the live test a tab
    // opened with a label read "TesterB" (two tabs on one server); a bookmark's name may be anything, or read like
    // another server's. Its tab is still found: by its place among the connections, checked with TeamSpeak.
    void serverTabsWhoseTextIsALabel()
    {
        QMainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* tabs = new QTabWidget(&window);
        window.setCentralWidget(tabs);
        window.show();
        const auto label = [tabs](const QStringList& texts) {
            while (tabs->count())
                delete tabs->widget(0);
            for (const QString& text : texts)
                tabs->addTab(new QWidget(tabs), text);
        };
        // The live test: two tabs on one server (11 and 13 are both "Weekend Squad"), the second one labelled.
        label({QStringLiteral("Weekend Squad"), QStringLiteral("TesterB")});
        FakeTabs::install(tabs, {11, 13}, {11, 13});
        tabs->setCurrentIndex(0);
        QVERIFY(servertabs::show(13));
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(servertabs::show(11));
        QCOMPARE(tabs->currentIndex(), 0);
        // A bookmark's name instead of the server's.
        label({QStringLiteral("Squad (bookmark)"), QStringLiteral("Night && Day")});
        FakeTabs::install(tabs, {11, 12}, {11, 12});
        tabs->setCurrentIndex(1);
        QVERIFY(servertabs::show(11));
        QCOMPARE(tabs->currentIndex(), 0);
        // A label that reads like another server's name: that tab is the wrong one, the right one is found.
        label({QStringLiteral("Night && Day"), QStringLiteral("My bookmark"), QStringLiteral("Third")});
        FakeTabs::install(tabs, {11, 13, 12}, {11, 13, 12});
        tabs->setCurrentIndex(1);
        QVERIFY(servertabs::show(12));
        QCOMPARE(tabs->currentIndex(), 2);
        QCOMPARE(ts3::currentConnection(), quint64(12));
        // Nowhere to be found (the place isn't it either): back where it was.
        FakeTabs::tabConnection = {11, 13, 13};
        tabs->setCurrentIndex(1);
        QVERIFY(!servertabs::show(12));
        QCOMPARE(tabs->currentIndex(), 1);
    }

    // The compact bar with its line (2.3 live test) at every width: nothing overlaps, the line under the row.
    void compactBarWithItsLine()
    {
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        MusicBar::View v = playing();
        v.mode           = Mode::Paused;
        v.away           = QStringLiteral("Weekend Squad › Lobby");
        v.problem        = QStringLiteral("Paused while you're on another server tab. Play takes you back there.");
        v.problemKind    = MusicBar::Problem::Info;
        const int height = MusicBar::heightFor(font, true);
        QVERIFY(height > MusicBar::heightFor(font, false));
        for (int width = 900; width >= 220; width -= 10) {
            const MusicBar::Geometry g = MusicBar::geometryFor(QSize(width, height), font, v);
            QVERIFY2(!overlaps(g), qPrintable(QString::number(width)));
            QVERIFY(!g.problem.isEmpty());
            QVERIFY(g.problem.top() > g.play.center().y() && g.problem.bottom() < height);
            QVERIFY(g.problem.right() < width);
            QVERIFY(g.stop.right() < width && g.play.right() < g.stop.left());
        }
    }

    void problemsShowInTheBar()
    {
        Window          w;
        MusicController c(host(&w));
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.bar()->view().problem.isEmpty());
        const auto problemAfter = [&] {
            for (int i = 0; i < MusicController::kCheckTicks; ++i)
                c.refreshNow();
            return c.bar()->view().problem;
        };
        FakeVoice::inputMuted = MUTEINPUT_MUTED;
        QVERIFY(problemAfter().contains(QStringLiteral("microphone is muted")));
        QCOMPARE(c.bar()->view().problemKind, MusicBar::Problem::Warning);
        FakeVoice::outputMuted = MUTEOUTPUT_MUTED;
        QVERIFY(problemAfter().contains(QStringLiteral("sound is muted")));
        FakeVoice::outputMuted = MUTEOUTPUT_NONE;
        FakeVoice::inputMuted  = MUTEINPUT_NONE;
        FakeVoice::hardware    = 0;
        QVERIFY(problemAfter().contains(QStringLiteral("no microphone open")));
        FakeVoice::hardware    = 1;
        FakeVoice::neededPower = 99; // talk power taken away while it plays
        QVERIFY(problemAfter().contains(QStringLiteral("talk power")));
        FakeVoice::neededPower = 0;
        QVERIFY(problemAfter().isEmpty());
        // 2.3 live test: muted music says so (the push-to-talk line said "the channel hears the music").
        FakeVoice::deactivated = INPUT_DEACTIVATED;
        QVERIFY(problemAfter().contains(QStringLiteral("Push-to-talk")));
        c.toggleMute();
        QVERIFY(problemAfter().contains(QStringLiteral("music is muted")));
        QCOMPARE(c.bar()->view().problemKind, MusicBar::Problem::Info);
        c.toggleMute();
        QVERIFY(problemAfter().contains(QStringLiteral("Push-to-talk")));
        c.stop(); // (push-to-talk given back: deactivated again)
        FakeVoice::deactivated = INPUT_ACTIVE;
        // A voice codec: a hint at the start.
        FakeVoice::codec = CODEC_OPUS_VOICE;
        QVERIFY(c.start(m_track));
        playFor(c, 100);
        QVERIFY(c.bar()->view().problem.contains(QStringLiteral("Opus Music")));
        QCOMPARE(c.bar()->view().problemKind, MusicBar::Problem::Info);
        c.stop();
    }

    void pushToTalkIsKeptOpenAndGivenBack()
    {
        Window          w;
        MusicController c(host(&w));
        FakeVoice::deactivated = INPUT_DEACTIVATED; // push-to-talk, key up
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE));
        QVERIFY(c.bar()->view().problem.contains(QStringLiteral("Push-to-talk")));
        // Only the music goes out meanwhile: the voice never does, even when TeamSpeak says "send".
        std::vector<qint16> block(2 * 960, 12000);
        int                 edited = music::kEditedSend;
        for (int i = 0; i < 3; ++i) {
            std::fill(block.begin(), block.end(), qint16(12000));
            edited = music::kEditedSend;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
        }
        QCOMPARE(edited, 3);
        QVERIFY(std::abs(int(block[2 * 900]) - 12000) > 2000); // not the voice
        // The key pressed and let go: TeamSpeak deactivates again; it is opened again.
        FakeVoice::deactivated = INPUT_DEACTIVATED;
        c.refreshNow();
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE));
        // Paused: given back to push-to-talk.
        click(c.bar()->playButton());
        QVERIFY(!c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        click(c.bar()->playButton());
        playFor(c, 100);
        QVERIFY(c.inputKeptOpen());
        // Another server tab: given back there too.
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        QVERIFY(!c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        QVERIFY(c.inputKeptOpen());
        // TeamSpeak keeps closing it: the plugin stops trying, and says what that means.
        for (int i = 0; i < MusicController::kReopenLimit + 2; ++i) {
            FakeVoice::deactivated = INPUT_DEACTIVATED;
            c.refreshNow();
        }
        QVERIFY(!c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        for (int i = 0; i < MusicController::kCheckTicks; ++i)
            c.refreshNow();
        QVERIFY(c.bar()->view().problem.contains(QStringLiteral("only while you hold")));
        // Stop: TeamSpeak's input as it was.
        c.stop();
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        // Voice activation users: never touched.
        FakeVoice::deactivated    = INPUT_ACTIVE;
        FakeVoice::deactivateSets = 0;
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(!c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivateSets, 0);
        // Shutdown while it keeps a push-to-talk input open gives it back.
        FakeVoice::deactivated = INPUT_DEACTIVATED;
        c.refreshNow();
        QVERIFY(c.inputKeptOpen());
        c.shutdown();
        QCOMPARE(FakeVoice::deactivated, int(INPUT_DEACTIVATED));
        QVERIFY(!c.bar());
    }

    void theVoiceNeverSlipsOutAroundPushToTalk()
    {
        // Right when the plugin opens TeamSpeak's push-to-talk input, and right when it closes it again, a
        // capture block TeamSpeak would send (no voice activation: everything) never carries the microphone.
        Window          w;
        MusicController c(host(&w));
        int             opened = 0, closed = 0, leaked = 0;
        FakeVoice::onSet = [&](size_t flag, int value) {
            if (flag != CLIENT_INPUT_DEACTIVATED)
                return;
            ++(value == INPUT_ACTIVE ? opened : closed);
            std::vector<qint16> block(2 * 960, 12000); // the voice; the music alone stays below 0.25 (8192)
            int                 edited = music::kEditedSend;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
            if ((edited & music::kEditedSend) && std::any_of(block.begin(), block.end(), [](qint16 s) { return std::abs(int(s)) > 9000; }))
                ++leaked;
        };
        FakeVoice::deactivated = INPUT_DEACTIVATED; // push-to-talk, key up
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.inputKeptOpen());
        click(c.bar()->playButton()); // paused: given back
        QVERIFY(!c.inputKeptOpen());
        click(c.bar()->playButton());
        playFor(c, 100);
        QVERIFY(c.inputKeptOpen());
        c.stop(); // given back
        FakeVoice::onSet = nullptr;
        QVERIFY(opened >= 2);
        QVERIFY(closed >= 2);
        QCOMPARE(leaked, 0);
    }

    void mutedInTeamSpeakTheMusicStaysOut()
    {
        // The user mutes the microphone or the sound in TeamSpeak (or a voice message's MicGuard mutes it):
        // within a tick the plugin stops making the client send, and TeamSpeak's own decision stands.
        Window          w;
        MusicController c(host(&w));
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        std::vector<qint16> block(960, 500);
        const auto          capture = [&] {
            std::fill(block.begin(), block.end(), qint16(500));
            int edited = 0;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 1, &edited);
            return edited;
        };
        QCOMPARE(capture(), 3); // the music goes out
        FakeVoice::inputMuted = MUTEINPUT_MUTED;
        c.refreshNow(); // one tick, not only the check every second
        QCOMPARE(capture(), 0);
        QCOMPARE(int(block[100]), 500);
        FakeVoice::inputMuted  = MUTEINPUT_NONE;
        FakeVoice::outputMuted = MUTEOUTPUT_MUTED;
        c.refreshNow();
        QCOMPARE(capture(), 0);
        QVERIFY(c.bar()->view().problem.contains(QStringLiteral("sound is muted")));
        FakeVoice::outputMuted = MUTEOUTPUT_NONE;
        c.refreshNow();
        QCOMPARE(capture(), 3);
        c.stop();
        // Muted before it starts: never sent from the first block on.
        FakeVoice::inputMuted = MUTEINPUT_MUTED;
        QVERIFY(c.start(m_track));
        for (int i = 0; i < 50; ++i) {
            QCOMPARE(capture(), 0);
            QTest::qWait(5);
        }
        c.stop();
    }

    // ---- 2.3 cross review (audio) ---------------------------------------------------------------------------

    void voiceActivationIsNeverKeptOpen()
    {
        // The author's setup: voice activation. TeamSpeak deactivates the input of a server tab in the
        // background and activates it again when the tab comes back; a tick that reads "deactivated" right
        // then must not take the input over as push-to-talk. Before, it did, and the voice was left out of
        // the music for the rest of the track ("your voice isn't sent while it plays").
        Window          w;
        MusicController c(host(&w));
        FakeVoice::vad = "true";
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(!c.inputKeptOpen());
        FakeVoice::visible = 2;
        c.serverTabChanged(2);
        FakeVoice::deactivated = INPUT_DEACTIVATED; // the background tab's input
        FakeVoice::visible     = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection); // back, before TeamSpeak activated it again
        for (int i = 0; i < MusicController::kCheckTicks; ++i)
            c.refreshNow();
        QVERIFY(!c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivateSets, 0); // TeamSpeak's input, never written
        QVERIFY(!c.bar()->view().problem.contains(QStringLiteral("Push-to-talk")));
        FakeVoice::deactivated = INPUT_ACTIVE;
        // Talking over the music: the voice goes out with it (the music alone stays below 0.13, 4100).
        std::vector<qint16> block(2 * 960, 12000);
        for (int i = 0; i < 2; ++i) { // (the first block ramps the voice in after the silence before)
            std::fill(block.begin(), block.end(), qint16(12000));
            int edited = music::kEditedSend;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
            QCOMPARE(edited, 3);
        }
        QVERIFY2(block[2 * 500] >= 12000, qPrintable(QString::number(block[2 * 500])));
        c.stop();
        QCOMPARE(FakeVoice::deactivateSets, 0);
        // Push-to-talk (kept open), then switched to voice activation while it plays: the plugin lets go
        // without closing TeamSpeak's input (that would silence the user until they switch again).
        FakeVoice::vad         = "false";
        FakeVoice::deactivated = INPUT_DEACTIVATED;
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.inputKeptOpen());
        FakeVoice::vad  = "true"; // (TeamSpeak keeps the input active for voice activation)
        const int sets  = FakeVoice::deactivateSets;
        c.refreshNow();
        QVERIFY(!c.inputKeptOpen());
        c.stop();
        QCOMPARE(FakeVoice::deactivateSets, sets);
        QCOMPARE(FakeVoice::deactivated, int(INPUT_ACTIVE));
    }

    void voiceActivationStartsTheMusic()
    {
        // 2.3 live test: under voice activation TeamSpeak goes on sending on the plugin's "send", but never
        // starts while it hears silence: the channel heard nothing until the user spoke, and nothing again
        // after a pause. The plugin turns voice activation off for a moment (the microphone out meanwhile)
        // and on again once TeamSpeak sends; always given back.
        Window          w;
        MusicController c(host(&w));
        FakeVoice::vad     = "true";
        FakeVoice::talking = 0;
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.voiceActivationPaused());
        QCOMPARE(FakeVoice::vad, QByteArray("false"));
        QCOMPARE(FakeVoice::vadOffSets, 1);
        QVERIFY(!c.inputKeptOpen());
        QCOMPARE(FakeVoice::deactivateSets, 0); // TeamSpeak's input itself is never written
        // Meanwhile only the music goes out: with voice activation off TeamSpeak would send the room.
        std::vector<qint16> block(2 * 960, 12000);
        int                 edited = music::kEditedSend;
        music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
        QCOMPARE(edited, 3);
        QVERIFY(std::abs(int(block[2 * 900]) - 12000) > 2000);
        // TeamSpeak sends: voice activation back, and the voice goes along with the music again.
        FakeVoice::talking = 1;
        c.refreshNow();
        QVERIFY(!c.voiceActivationPaused());
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
        for (int i = 0; i < 2; ++i) { // (the first block ramps the voice in)
            std::fill(block.begin(), block.end(), qint16(12000));
            edited = music::kEditedSend;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
            QCOMPARE(edited, 3);
        }
        QVERIFY2(block[2 * 500] >= 12000, qPrintable(QString::number(block[2 * 500])));
        playFor(c, 100);
        QCOMPARE(FakeVoice::vadOffSets, 1); // sending: left alone
        // Paused (TeamSpeak stops sending), then resumed: started again the same way.
        click(c.bar()->playButton());
        FakeVoice::talking = 0;
        playFor(c, 100);
        QCOMPARE(FakeVoice::vadOffSets, 1); // paused: nothing to send
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
        click(c.bar()->playButton());
        playFor(c, MusicController::kVadKickGapMs + 300);
        QVERIFY(FakeVoice::vadOffSets >= 2);
        // Stop and shutdown while voice activation is off: given back.
        QTRY_VERIFY_WITH_TIMEOUT((playFor(c, 40), c.voiceActivationPaused()), MusicController::kVadKickGapMs + 1000);
        c.stop();
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        QVERIFY(c.voiceActivationPaused());
        c.shutdown();
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
    }

    void voiceActivationLeftAloneWhenMutedOrAway()
    {
        // Muted music (nothing for the channel), TeamSpeak's microphone muted, or the music's tab in the
        // background: voice activation is never touched.
        Window          w;
        MusicController c(host(&w));
        FakeVoice::vad     = "true";
        FakeVoice::talking = 0;
        c.setVolume(0);
        QVERIFY(c.start(m_track));
        playFor(c, 300);
        QCOMPARE(FakeVoice::vadOffSets, 0);
        c.setVolume(60);
        FakeVoice::inputMuted = MUTEINPUT_MUTED;
        playFor(c, 300);
        QCOMPARE(FakeVoice::vadOffSets, 0);
        FakeVoice::inputMuted = MUTEINPUT_NONE;
        FakeVoice::visible    = 2;
        c.serverTabChanged(2);
        playFor(c, 300);
        QCOMPARE(FakeVoice::vadOffSets, 0);
        FakeVoice::visible = fakets3::kConnection;
        c.serverTabChanged(fakets3::kConnection);
        playFor(c, 300);
        QCOMPARE(FakeVoice::vadOffSets, 1); // back on its tab: started
        FakeVoice::visible = 2;
        c.serverTabChanged(2); // another tab while voice activation is off: given back at once
        QCOMPARE(FakeVoice::vad, QByteArray("true"));
        c.stop();
    }

    void soundMutedNotEvenHere()
    {
        // TeamSpeak's sound muted: the bar says "nobody hears the music, not even you", so nothing goes into
        // this computer's playback either (one fading block), whatever TeamSpeak's mixer still runs.
        Window          w;
        MusicController c(host(&w));
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        std::vector<qint16> out(2 * 960, 0);
        unsigned            speakers[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
        const auto          play       = [&] {
            std::fill(out.begin(), out.end(), qint16(0));
            unsigned mask = 0;
            music::onPlayback(fakets3::kConnection, out.data(), 960, 2, speakers, &mask);
            return mask;
        };
        QCOMPARE(play(), 3u);
        FakeVoice::outputMuted = MUTEOUTPUT_MUTED;
        c.refreshNow();
        play(); // the fade out
        QCOMPARE(play(), 0u);
        QCOMPARE(play(), 0u);
        QVERIFY(c.bar()->view().problem.contains(QStringLiteral("not even you")));
        FakeVoice::outputMuted = MUTEOUTPUT_NONE;
        c.refreshNow();
        play(); // back up from nothing
        QCOMPARE(play(), 3u);
        c.stop();
    }

    void stopFadesOutForTheChannel()
    {
        // TeamSpeak's capture thread keeps calling while the user presses Stop: the last block that goes out
        // with music in it is the 5 ms fade out, never the music cut off mid-wave (a click for everyone).
        Window          w;
        MusicController c(host(&w));
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        std::atomic<bool>   run{true};
        std::mutex          lock;
        std::vector<qint16> lastMusic;
        std::thread         capture([&] {
            std::vector<qint16> block(2 * 960);
            while (run.load()) {
                std::fill(block.begin(), block.end(), qint16(0));
                int edited = 0;
                music::onCaptured(fakets3::kConnection, block.data(), 960, 2, &edited);
                if (edited & music::kEditedSend) {
                    std::lock_guard<std::mutex> guard(lock);
                    lastMusic = block;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        QTest::qWait(100);
        c.stop();
        QVERIFY(!c.playing());
        QTest::qWait(50);
        run = false;
        capture.join();
        std::lock_guard<std::mutex> guard(lock);
        QVERIFY(!lastMusic.empty());
        QVERIFY2(qAbs(int(lastMusic[2 * (music::Engine::kFadeFrames - 1)])) <= 1, qPrintable(QString::number(lastMusic[2 * (music::Engine::kFadeFrames - 1)])));
        QVERIFY2(qAbs(int(lastMusic[2 * 959])) <= 1, qPrintable(QString::number(lastMusic[2 * 959])));
    }

    void seekVolumeAndMuteRemembered()
    {
        Window          w;
        MusicController::Host h = host(&w);
        h.settingsFile          = m_dir->filePath(QStringLiteral("music.ini"));
        {
            MusicController c(h);
            QCOMPARE(c.volume(), MusicController::kDefaultVolume);
            QVERIFY(c.start(m_track));
            playFor(c, 200);
            c.seek(3000);
            playFor(c, 200);
            QVERIFY(qAbs(c.bar()->view().positionMs - 3000) < 500);
            c.setVolume(30);
            QCOMPARE(c.bar()->view().volume, 30);
            c.toggleMute();
            QVERIFY(c.muted());
            QVERIFY(c.bar()->view().muted);
            // Muted: one block fades the music out (2.3 review: no click), then the voice gets no music, and
            // TeamSpeak's decision stands (nothing sent for silent music).
            std::vector<qint16> block(960, 1000);
            int                 edited = 0;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 1, &edited);
            QCOMPARE(edited, 3); // the fade
            std::fill(block.begin(), block.end(), qint16(1000));
            edited = 0;
            music::onCaptured(fakets3::kConnection, block.data(), 960, 1, &edited);
            QCOMPARE(edited, 0);
            QCOMPARE(int(block[500]), 1000);
            edited = music::kEditedSend; // talking: TeamSpeak sends the voice as it is
            music::onCaptured(fakets3::kConnection, block.data(), 960, 1, &edited);
            QCOMPARE(edited, int(music::kEditedSend));
            QCOMPARE(int(block[500]), 1000);
            c.toggleMute();
            c.setVolume(35);
        } // shutdown writes it
        MusicController again(h);
        QCOMPARE(again.volume(), 35);
        QVERIFY(!again.muted());
    }

    void stopsOnChannelSwitchDisconnectAndEnd()
    {
        Window          w;
        MusicController c(host(&w));
        QVERIFY(c.start(m_track));
        playFor(c, 200);
        // Somebody else moved: nothing.
        c.clientMoved(fakets3::kConnection, fakets3::kOwnClient + 1, 99);
        QVERIFY(c.playing());
        // We moved to another channel.
        FakeVoice::channel = 77;
        c.clientMoved(fakets3::kConnection, fakets3::kOwnClient, 77);
        QVERIFY(!c.playing());
        QVERIFY(!music::active());
        QVERIFY(c.bar()->view().notice.contains(QStringLiteral("left the channel")));
        // Also found by the check every second (an event that never came).
        QVERIFY(c.start(m_track));
        playFor(c, 100);
        FakeVoice::channel = 78;
        for (int i = 0; i < MusicController::kCheckTicks && c.playing(); ++i)
            c.refreshNow();
        QVERIFY(!c.playing());
        FakeVoice::channel = fakets3::kChannel;
        // Disconnected.
        QVERIFY(c.start(m_track));
        playFor(c, 100);
        fakets3::setConnected(false);
        c.connectionLost(fakets3::kConnection);
        QVERIFY(!c.playing());
        QVERIFY(c.bar()->view().notice.contains(QStringLiteral("disconnected")));
        fakets3::setConnected(true);
        // The track's end: the bar goes, no notice.
        MusicController::Host h = host(&w);
        h.source = [](const QString&) {
            GenSource::Options o;
            o.frames = 48000 / 4;
            return musicfakes::gen(o);
        };
        MusicController shortOne(h);
        QVERIFY(shortOne.start(m_track));
        QPointer<MusicBar> bar = shortOne.bar();
        playFor(shortOne, 600);
        QTRY_VERIFY(!shortOne.playing());
        QVERIFY(bar && bar->isHidden());
        QVERIFY(!music::active());
    }

    void fileThatCantBePlayed()
    {
        Window              w;
        MusicController::Host h = host(&w);
        h.source = [](const QString&) {
            GenSource::Options o;
            o.failOpen = true;
            return musicfakes::gen(o);
        };
        MusicController c(h);
        QVERIFY(c.start(m_track));
        QTRY_VERIFY(!c.playing());
        QCOMPARE(c.bar()->view().mode, Mode::Notice);
        QCOMPARE(c.bar()->view().noticeIcon, MusicBar::NoticeIcon::Error);
        QCOMPARE(c.bar()->view().notice, QStringLiteral("Couldn't play “Morning Walk”: This file has no sound to play."));
        QVERIFY(!music::active());
    }

    void thePaperclipEntry()
    {
        Window          w;
        QString         askedDir;
        int             asked = 0;
        auto*           c     = new MusicController([&] {
            MusicController::Host h = host(&w);
            h.pickFile              = [&](QWidget*, const QString& dir) {
                ++asked;
                askedDir = dir;
                return m_track;
            };
            return h;
        }());
        const QList<attachmenu::Entry> entries = attachmenu::entries();
        QCOMPARE(entries.size(), 1);
        const attachmenu::Entry& e = entries.first();
        QCOMPARE(e.id, QStringLiteral("livemusic"));
        QCOMPARE(e.order, 40); // attachmenu.h's slot: after Photo or video (10), Document (20), Poll (30)
        QCOMPARE(e.text(), QStringLiteral("Live music"));
        QVERIFY(!e.icon().isNull());
        QVERIFY(e.enabled());
        fakets3::setConnected(false);
        QVERIFY(!e.enabled());
        e.trigger(w.input); // explains instead of opening the dialog
        QCOMPARE(asked, 0);
        QVERIFY(c->bar()->view().notice.contains(QStringLiteral("Connect")));
        fakets3::setConnected(true);
        // No talk power: chosen, it says why (a grey item wouldn't), and no dialog opens.
        FakeVoice::neededPower = 50;
        QVERIFY(e.enabled());
        e.trigger(w.input);
        QCOMPARE(asked, 0);
        QVERIFY(c->bar()->view().notice.contains(QStringLiteral("talk power")));
        FakeVoice::neededPower = 0;
        e.trigger(w.input);
        QCOMPARE(asked, 1);
        QVERIFY(c->playing());
        delete c; // stops it and takes the entry away
        QVERIFY(attachmenu::entries().isEmpty());
        QVERIFY(!music::active());
        e.trigger(w.input); // a menu still holding the old entry: nothing happens
        e.enabled();
    }

    void shutdownWhilePlaying()
    {
        Window             w;
        auto*              c = new MusicController(host(&w));
        QVERIFY(c->start(m_track));
        playFor(*c, 200);
        QPointer<MusicBar> bar = c->bar();
        QVERIFY(bar);
        c->shutdown();
        QVERIFY(!music::active());
        QVERIFY(!bar); // out of TeamSpeak's layout
        QCOMPARE(w.box()->count(), 2);
        c->shutdown(); // twice is fine
        QVERIFY(!c->start(m_track));
        delete c;
    }

  private:
    MusicController::Host host(Window* w)
    {
        MusicController::Host h;
        QPointer<QWidget>     input = w->input.data();
        h.input             = [input] { return input.data(); };
        h.visibleConnection = [] { return FakeVoice::visible; };
        h.source            = [](const QString&) { return musicfakes::gen(); };
        h.pickFile          = [](QWidget*, const QString&) { return QString(); };
        return h;
    }
    // TeamSpeak's audio threads, played by the test: both sides every 20 ms for ms, with the bar ticking.
    void playFor(MusicController& c, int ms)
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
    QString                        m_track;
};

TSMEDIA_REGISTER_TEST(TestMusicBar)
#include "tst_musicbar.moc"
