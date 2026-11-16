// 2.7 F5: the saved chat zoom (chatzoom.h). One value for every chat (Settings::chatZoom, tenths of a point on
// TeamSpeak's chat font): attach applies it and a second attach (a plugin reload) changes no font; Ctrl + wheel in one
// chat sets it (saved after the last notch) and the other visible chats follow after 300 ms, hidden ones when shown;
// TeamSpeak's own chat font becomes the new base; bounds, touchpad fractions and pixel fonts; setChatZoom ends the load
// holds first (design M3.2); a new tab is zoomed before its first paint (M4); a scrolled-up chat shown later keeps its
// line through ChatScale's pass (M3.3).

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QWheelEvent>
#include <QtTest>

#include "chatscale.h"
#include "chatzoom.h"
#include "loadhold.h"
#include "previewscale.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"

namespace {

QFont chatFont(qreal points = 8.25)
{
    QFont f(QStringLiteral("MS Shell Dlg 2"));
    f.setPointSizeF(points);
    return f;
}

// A chat browser as TeamSpeak makes it (named ChatTab, TeamSpeak's chat font), with text to scroll.
QTextBrowser* newChat(QWidget* parent, int lines = 20)
{
    auto* b = new QTextBrowser(parent);
    b->setObjectName(QStringLiteral("ChatTab"));
    b->setFont(chatFont());
    QString html;
    for (int i = 0; i < lines; ++i)
        html += QStringLiteral("<p>&lt;12:%1&gt; \"Alice\": line %2 of the chat</p>").arg(i % 60, 2, 10, QLatin1Char('0')).arg(i);
    b->setHtml(html);
    return b;
}

// FontChange events a chat got (a reload must cause none).
class FontCounter : public QObject
{
  public:
    int fonts = 0;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::FontChange)
            ++fonts;
        return QObject::eventFilter(watched, event);
    }
};

// The font size a chat had at its first paint.
class FirstPaint : public QObject
{
  public:
    QTextBrowser* browser = nullptr;
    qreal         points  = -1;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Paint && points < 0 && browser)
            points = browser->font().pointSizeF();
        return QObject::eventFilter(watched, event);
    }
};

void ctrlWheel(QTextBrowser* b, int delta = 120)
{
    QWidget*      vp = b->viewport();
    const QPointF pos(vp->width() / 2.0, vp->height() / 2.0);
    QWheelEvent   e(pos, QPointF(vp->mapToGlobal(pos.toPoint())), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(vp, &e);
}

int savedZoom()
{
    const QSettings ini(ts3::dataDir() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    return ini.value(QStringLiteral("chatZoom"), -999).toInt();
}

} // namespace

class TestChatZoom : public QObject
{
    Q_OBJECT

    int m_zoomBefore = 0;

  private slots:
    void init()
    {
        m_zoomBefore                  = Settings::instance().chatZoom;
        Settings::instance().chatZoom = 0;
    }
    void cleanup()
    {
        Settings::instance().chatZoom = m_zoomBefore;
        Settings::instance().save();
    }

    void maths()
    {
        QCOMPARE(chatzoom::percentOf(10, 8.25), 112);
        QCOMPARE(chatzoom::percentOf(20, 8.25), 124);
        QCOMPARE(chatzoom::percentOf(0, 8.25), 100);
        QCOMPARE(chatzoom::percentOf(-40, 8.25), 52);
        QCOMPARE(chatzoom::percentOf(20, 12.0), 117);
        QCOMPARE(chatzoom::percentOf(0, 0), 100); // no base known: TeamSpeak's default
        QCOMPARE(chatzoom::deltaText(20), QStringLiteral("+2.0 pt"));
        QCOMPARE(chatzoom::deltaText(-5), QStringLiteral("-0.5 pt"));
        QCOMPARE(chatzoom::deltaText(0), QStringLiteral("+0.0 pt"));
        QCOMPARE(chatzoom::clampTenths(999), 240);
        QCOMPARE(chatzoom::clampTenths(-999), -40);
        QCOMPARE(chatzoom::targetPoints(5.0, -40), chatzoom::kMinPoints); // never below 4 pt
        QCOMPARE(QString::fromLatin1(chatzoom::baseProperty()), QStringLiteral("tsmediaZoomBase"));
    }

    // Attach: base + zoom; a second ChatZoom on the same chats (Options > Addons > Reload) sets no font at all.
    void attachIsIdempotent()
    {
        Settings::instance().chatZoom = 20;
        QWidget window;
        window.resize(500, 400);
        QTextBrowser* a = newChat(&window);
        QTextBrowser* b = newChat(&window);
        window.show();
        {
            ChatZoom zoom(ChatZoom::Host{});
            zoom.attach(a);
            zoom.attach(b);
            QCOMPARE(a->font().pointSizeF(), 10.25);
            QCOMPARE(b->font().pointSizeF(), 10.25);
            QCOMPARE(a->document()->defaultFont().pointSizeF(), 10.25); // QTextEdit gives it to its document
            QCOMPARE(zoom.fontsSet(), 2);
            zoom.shutdown();
        }
        FontCounter counter;
        a->installEventFilter(&counter);
        b->installEventFilter(&counter);
        {
            ChatZoom again(ChatZoom::Host{});
            again.attach(a);
            again.attach(b);
            QCoreApplication::processEvents();
            QCOMPARE(again.fontsSet(), 0);
            QCOMPARE(again.basePoints(a), 8.25); // the base came along on the chat
            again.shutdown();
        }
        QCOMPARE(counter.fonts, 0);
        QCOMPARE(a->font().pointSizeF(), 10.25); // the zoom stays at unload
    }

    // Ctrl + wheel in one chat: the value, saved after the debounce, the other visible chat after 300 ms, a hidden one
    // when it is shown (before it paints), and the signal.
    void wheelSetsEveryChat()
    {
        QWidget window;
        window.resize(600, 400);
        QTextBrowser* a = newChat(&window);
        QTextBrowser* b = newChat(&window);
        a->setGeometry(0, 0, 300, 400);
        b->setGeometry(300, 0, 300, 400);
        QTextBrowser* c = newChat(nullptr); // another tab: hidden
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ChatZoom zoom(ChatZoom::Host{});
        QSignalSpy changed(&zoom, &ChatZoom::chatZoomChanged);
        zoom.attach(a);
        zoom.attach(b);
        zoom.attach(c);
        ctrlWheel(a);
        ctrlWheel(a);
        QCOMPARE(a->font().pointSizeF(), 10.25); // Qt's own zoom, +1 pt per notch
        QCOMPARE(Settings::instance().chatZoom, 20);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(changed.last().at(0).toInt(), 20);
        QCOMPARE(b->font().pointSizeF(), 8.25); // not yet: after the last notch
        QTRY_COMPARE_WITH_TIMEOUT(b->font().pointSizeF(), 10.25, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(savedZoom(), 20, 3000);
        QCOMPARE(c->font().pointSizeF(), 8.25); // hidden: when shown
        FirstPaint first;
        first.browser = c;
        c->viewport()->installEventFilter(&first);
        c->resize(300, 300);
        c->show();
        QCOMPARE(c->font().pointSizeF(), 10.25); // at its Show
        QVERIFY(QTest::qWaitForWindowExposed(c));
        QTRY_VERIFY(first.points > 0);
        QCOMPARE(first.points, 10.25);
        c->viewport()->removeEventFilter(&first);
        zoom.shutdown();
        delete c;
    }

    // TeamSpeak's chat font (Options > Chat OK): the chat's new base, the zoom on top a turn later; our own size is no news.
    void teamSpeakFontIsTheBase()
    {
        Settings::instance().chatZoom = 20;
        QWidget window;
        QTextBrowser* a = newChat(&window);
        window.show();
        ChatZoom zoom(ChatZoom::Host{});
        zoom.attach(a);
        QCOMPARE(a->font().pointSizeF(), 10.25);
        const int set = zoom.fontsSet();
        a->setFont(chatFont(12.0)); // TeamSpeak applies its chat font
        QCOMPARE(zoom.basePoints(a), 12.0);
        QTRY_COMPARE_WITH_TIMEOUT(a->font().pointSizeF(), 14.0, 1000);
        QCOMPARE(zoom.fontsSet(), set + 1);
        a->setFont(chatFont(14.0)); // the target itself: nothing to do
        QCoreApplication::processEvents();
        QCOMPARE(zoom.basePoints(a), 12.0);
        QCOMPARE(zoom.fontsSet(), set + 1);
        QCOMPARE(zoom.percent(), 124); // no visible chat in the host: on TeamSpeak's default base (8.25 pt + 2)
        zoom.shutdown();
    }

    void boundsFractionsAndPixelFonts()
    {
        Settings::instance().chatZoom = 240;
        QWidget window;
        window.resize(400, 300);
        QTextBrowser* a = newChat(&window);
        a->setGeometry(0, 0, 400, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ChatZoom zoom(ChatZoom::Host{});
        zoom.attach(a);
        QCOMPARE(a->font().pointSizeF(), 32.25);
        ctrlWheel(a); // one past the bound: brought back
        QCOMPARE(Settings::instance().chatZoom, 240);
        QTRY_COMPARE_WITH_TIMEOUT(a->font().pointSizeF(), 32.25, 1000);
        // A touchpad's fraction: a quarter notch is 0.25 pt, saved as whole tenths and the chat put on them.
        zoom.setChatZoom(0);
        QCOMPARE(a->font().pointSizeF(), 8.25);
        ctrlWheel(a, 30);
        QCOMPARE(Settings::instance().chatZoom, 3);
        QTRY_COMPARE_WITH_TIMEOUT(a->font().pointSizeF(), 8.55, 1000);
        // A chat with a pixel-sized font is left alone.
        QTextBrowser* px = newChat(&window);
        QFont         pixel = px->font();
        pixel.setPixelSize(14);
        px->setFont(pixel);
        zoom.attach(px);
        QVERIFY(!zoom.ensure(px));
        QCOMPARE(px->font().pixelSize(), 14);
        zoom.setChatZoom(-999);
        QCOMPARE(Settings::instance().chatZoom, -40);
        QCOMPARE(a->font().pointSizeF(), 4.25);
        zoom.shutdown();
    }

    // M3.2: a zoom from outside the chat (the Settings row) ends the load holds before any font changes; saved at once.
    void setChatZoomEndsTheLoadHolds()
    {
        QWidget window;
        window.resize(400, 300);
        QTextBrowser* a = newChat(&window, 200);
        a->setGeometry(0, 0, 400, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        LoadHold* hold = new LoadHold(a, chatscroll::captureReader(a), [] { return true; }, &window);
        QVERIFY(hold->active());
        qreal          fontAtEnd = -1;
        ChatZoom::Host host;
        host.endLoadHolds = [&] {
            fontAtEnd = a->font().pointSizeF();
            hold->finish(LoadHold::End::User);
        };
        host.visible = [a] { return a; };
        ChatZoom   zoom(host);
        QSignalSpy changed(&zoom, &ChatZoom::chatZoomChanged);
        zoom.attach(a);
        zoom.setChatZoom(30);
        QCOMPARE(fontAtEnd, 8.25); // the holds went first
        QVERIFY(!hold->active());
        QCOMPARE(hold->ended(), LoadHold::End::User);
        QCOMPARE(a->font().pointSizeF(), 11.25);
        QCOMPARE(savedZoom(), 30); // at once
        QCOMPARE(changed.count(), 1);
        QCOMPARE(zoom.percent(), 136);
        zoom.setChatZoom(30); // the same value: no signal
        QCOMPARE(changed.count(), 1);
        zoom.shutdown();
    }

    // M4: a ChatTabWidget gaining a tab: the new chat is found a turn later and zoomed before its first paint.
    void newTabZoomedBeforeFirstPaint()
    {
        Settings::instance().chatZoom = 20;
        QTabWidget tabs;
        tabs.resize(500, 400);
        tabs.addTab(newChat(nullptr), QStringLiteral("Channel"));
        tabs.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tabs));
        ChatZoom::Host host;
        ChatZoom*      zoomPtr = nullptr;
        int            discovers = 0;
        host.discover = [&] {
            ++discovers;
            for (QTextBrowser* b : tabs.findChildren<QTextBrowser*>())
                zoomPtr->attach(b);
        };
        ChatZoom zoom(host);
        zoomPtr = &zoom;
        zoom.watchTabs(&tabs);
        host.discover();
        QCOMPARE(qobject_cast<QTextBrowser*>(tabs.widget(0))->font().pointSizeF(), 10.25);
        QTextBrowser* fresh = newChat(nullptr);
        FirstPaint    first;
        first.browser = fresh;
        fresh->viewport()->installEventFilter(&first);
        tabs.setCurrentIndex(tabs.addTab(fresh, QStringLiteral("Bob"))); // a private chat opens
        QTRY_VERIFY_WITH_TIMEOUT(first.points > 0, 2000);
        QCOMPARE(first.points, 10.25);
        QVERIFY(discovers >= 2);
        fresh->viewport()->removeEventFilter(&first);
        zoom.shutdown();
    }

    // M3.3: a chat scrolled up while hidden gets the new zoom at its first Show; ChatScale's FontChange pass keeps the
    // line the reader was at.
    void firstShowOfAScrolledUpChatKeepsItsPlace()
    {
        QWidget window;
        window.resize(500, 400);
        QTextBrowser* a = newChat(&window, 400);
        a->setGeometry(0, 0, 500, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);
        a->document()->documentLayout()->documentSize(); // laid out to its end
        QScrollBar* bar = a->verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 2000);
        bar->setValue(bar->maximum() / 2);
        QTest::qWait(50);
        const int topBefore = a->cursorForPosition(QPoint(10, 10)).block().blockNumber();
        QVERIFY2(topBefore > 50 && topBefore < 350, qPrintable(QString::number(topBefore)));
        ChatScale* scaler   = nullptr;
        ChatScale::Host scaleHost;
        scaleHost.compute   = [a] { return previewscale::computeScale(previewscale::chatPxOf(a->document()), 100, a->viewport()->width(), a->document()->documentMargin(), 0, 1.0); };
        scaleHost.fontEvent = [&scaler] {
            if (scaler)
                scaler->request();
        };
        ChatZoom zoom(ChatZoom::Host{});
        zoom.attach(a); // zoom 0: nothing changes
        scaler = new ChatScale(a, scaleHost, &window);
        scaler->setScale(scaleHost.compute());
        a->hide(); // another tab in front
        zoom.setChatZoom(30); // hidden: when shown
        QCOMPARE(a->font().pointSizeF(), 8.25);
        a->show();
        QCOMPARE(a->font().pointSizeF(), 11.25);
        QTRY_VERIFY_WITH_TIMEOUT(!scaler->pending(), 2000);
        QTest::qWait(50);
        const int topAfter = a->cursorForPosition(QPoint(10, 10)).block().blockNumber();
        QVERIFY2(qAbs(topAfter - topBefore) <= 1, qPrintable(QStringLiteral("top block %1 -> %2").arg(topBefore).arg(topAfter)));
        scaler->prepareShutdown();
        zoom.shutdown();
    }

    // Review fix: a chat at its end, hidden through a zoom change, paints at its end the first time it is shown (Qt keeps
    // the scroll value while the bigger font grows the chat; ChatScale puts the end back only at its 150 ms settle).
    void firstShowOfAChatAtItsEndPaintsAtItsEnd()
    {
        QWidget window;
        window.resize(500, 400);
        QTextBrowser* a = newChat(&window, 600);
        a->setGeometry(0, 0, 500, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);
        a->document()->documentLayout()->documentSize(); // laid out to its end
        QScrollBar* bar = a->verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 2000);
        bar->setValue(bar->maximum());
        QTest::qWait(50);
        ChatScale*      scaler = nullptr;
        ChatScale::Host scaleHost;
        scaleHost.compute   = [a] { return previewscale::computeScale(previewscale::chatPxOf(a->document()), 100, a->viewport()->width(), a->document()->documentMargin(), 0, 1.0); };
        scaleHost.fontEvent = [&scaler] {
            if (scaler)
                scaler->request();
        };
        ChatZoom zoom(ChatZoom::Host{});
        zoom.attach(a);
        scaler = new ChatScale(a, scaleHost, &window);
        scaler->setScale(scaleHost.compute());
        a->hide(); // another tab in front
        zoom.setChatZoom(20);
        QCOMPARE(a->font().pointSizeF(), 8.25);
        // At the first paint: the scroll value against the end of the chat laid out in full with the new font.
        struct EndAtPaint : QObject {
            QTextBrowser* browser = nullptr;
            ChatScale*    scale   = nullptr;
            int           paints  = 0;
            int           gap     = -1;
            bool          early   = false; // the paint came before ChatScale's settle (the case that jumped)
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (event->type() == QEvent::Paint && ++paints == 1) {
                    early          = scale && scale->pending();
                    const int full = qRound(browser->document()->documentLayout()->documentSize().height()) - browser->viewport()->height();
                    gap            = full - browser->verticalScrollBar()->value();
                }
                return QObject::eventFilter(watched, event);
            }
        } paint;
        paint.browser = a;
        paint.scale   = scaler;
        a->viewport()->installEventFilter(&paint);
        a->show();
        QCOMPARE(a->font().pointSizeF(), 10.25);
        QTRY_VERIFY_WITH_TIMEOUT(paint.paints > 0, 2000);
        a->viewport()->removeEventFilter(&paint);
        QVERIFY(paint.early);
        QVERIFY2(paint.gap >= 0 && paint.gap <= 4, qPrintable(QStringLiteral("first paint %1 px above the end").arg(paint.gap)));
        QTRY_VERIFY_WITH_TIMEOUT(!scaler->pending(), 2000);
        QVERIFY(bar->value() >= bar->maximum() - 4); // and still at its end after the settle
        scaler->prepareShutdown();
        zoom.shutdown();
    }

    // A chat that stayed hidden through a zoom change (the plugin reloaded before it was shown) still knows its base: the
    // size the zoom set last is ours, not TeamSpeak's new chat font.
    void hiddenChatKeepsItsBaseAcrossReload()
    {
        Settings::instance().chatZoom = 20;
        QWidget window;
        window.resize(400, 300);
        QTextBrowser* a = newChat(&window);
        a->setGeometry(0, 0, 400, 300);
        QTextBrowser* hidden = newChat(nullptr);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        {
            ChatZoom zoom(ChatZoom::Host{});
            zoom.attach(a);
            zoom.attach(hidden);
            QCOMPARE(hidden->font().pointSizeF(), 10.25); // attach zooms hidden chats too (before their scale)
            zoom.setChatZoom(0);
            QCOMPARE(a->font().pointSizeF(), 8.25);
            QCOMPARE(hidden->font().pointSizeF(), 10.25); // when shown
            zoom.shutdown();
        }
        ChatZoom again(ChatZoom::Host{});
        again.attach(hidden);
        QCOMPARE(again.basePoints(hidden), 8.25);
        QCOMPARE(hidden->font().pointSizeF(), 8.25);
        again.shutdown();
        delete hidden;
    }

    // Shutdown: no filter left, a pending save made now.
    void shutdownSavesAndLetsGo()
    {
        QWidget window;
        window.resize(400, 300);
        QTextBrowser* a = newChat(&window);
        a->setGeometry(0, 0, 400, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        Settings::instance().chatZoom = 0;
        Settings::instance().save();
        auto* zoom = new ChatZoom(ChatZoom::Host{});
        zoom->attach(a);
        ctrlWheel(a);
        QCOMPARE(Settings::instance().chatZoom, 10);
        QCOMPARE(savedZoom(), 0); // not yet (500 ms)
        zoom->shutdown();
        QCOMPARE(savedZoom(), 10);
        delete zoom;
        ctrlWheel(a); // nobody listens any more: TeamSpeak's own zoom only
        QCOMPARE(Settings::instance().chatZoom, 10);
    }
};

TSMEDIA_REGISTER_TEST(TestChatZoom)

#include "tst_chatzoom.moc"
