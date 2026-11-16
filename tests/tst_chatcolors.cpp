// 2.7 F6: the chat's effective colours (chatcolors.h) and TS Media's line colours. The palette's answer for TeamSpeak's
// skins and custom style sheets (ts_facts 5.1: light, the community dark skin, custom1 #1e2a38, custom2 #121212, a
// gradient, a rule on the viewport only), the dominant colour of a sampled strip, dark and light, the presets made 4.5:1
// on every background, Automatic giving 2.6's exact colours on TeamSpeak's two chats, the sampler (a picture background
// the palette never knows; no paint event), and ts3::linkBBCode (M7).

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QStyle>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QtTest>

#include "chatcolors.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"
#include "uiutil.h"

namespace {

// A chat like TeamSpeak's: a QTextBrowser named ChatTab with a style sheet (polished, so the palette has the rule).
struct Chat {
    QWidget      window;
    QTextBrowser browser;
    explicit Chat(const QString& sheet)
        : browser(&window)
    {
        browser.setObjectName(QStringLiteral("ChatTab"));
        window.resize(400, 300);
        browser.setGeometry(0, 0, 400, 300);
        browser.setStyleSheet(sheet);
        browser.ensurePolished();
        browser.viewport()->ensurePolished();
    }
};

// Paint events that reach a widget (the sampler must send none).
class PaintCounter : public QObject
{
  public:
    int paints = 0;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Paint)
            ++paints;
        return QObject::eventFilter(watched, event);
    }
};

double contrast(const QString& a, const QColor& b)
{
    return ui::contrastRatio(QColor(a), b);
}

chatcolors::Colors solid(const char* base, const char* text)
{
    chatcolors::Colors c;
    c.base = QColor(QLatin1String(base));
    c.text = QColor(QLatin1String(text));
    c.link = QColor(0x1c, 0x82, 0xcc);
    c.dark = chatcolors::isDark(c.base);
    return c;
}

} // namespace

class TestChatColors : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup()
    {
        ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); // the default for later tests
    }

    // ts_facts 5.1: what the style sheet copies into the ChatTab's palette, and what we make of it.
    void fromPaletteOfSkins()
    {
        {
            Chat light{QString()}; // TeamSpeak's light skin: the palette's white
            const chatcolors::Colors c = chatcolors::fromPalette(light.browser.palette());
            QCOMPARE(c.base.name(), QStringLiteral("#ffffff"));
            QVERIFY(!c.dark);
        }
        {
            Chat dark(QStringLiteral("QTextEdit#ChatTab { background: #2F3136; color: #dcddde; }")); // the community dark skin
            const chatcolors::Colors c = chatcolors::fromPalette(dark.browser.palette());
            QCOMPARE(c.base.name(), QStringLiteral("#2f3136"));
            QCOMPARE(c.text.name(), QStringLiteral("#dcddde"));
            QVERIFY(c.dark);
        }
        {
            Chat custom1(QStringLiteral("QTextEdit#ChatTab { background-color: #1e2a38; color: #d8dee9; }"));
            const chatcolors::Colors c = chatcolors::fromPalette(custom1.browser.palette());
            QCOMPARE(c.base.name(), QStringLiteral("#1e2a38"));
            QVERIFY(c.dark);
        }
        {
            Chat custom2(QStringLiteral("QWidget { background: #121212; color: #e0e0e0; }"));
            const chatcolors::Colors c = chatcolors::fromPalette(custom2.browser.palette());
            QCOMPARE(c.base.name(), QStringLiteral("#121212"));
            QVERIFY(c.dark);
        }
        {
            // A gradient: the palette's Base has no colour (2.6 called it dark by accident); the average of its stops.
            Chat custom3(QStringLiteral("QTextEdit#ChatTab { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f0f0f0, stop:1 #d0d0d0); color: #ffffff; }"));
            const QBrush b = custom3.browser.palette().brush(QPalette::Active, QPalette::Base);
            QCOMPARE(b.style(), Qt::LinearGradientPattern);
            const chatcolors::Colors c = chatcolors::fromPalette(custom3.browser.palette());
            QCOMPARE(c.base.name(), QStringLiteral("#e0e0e0"));
            QVERIFY(!c.dark); // a light gradient is light, whatever its text colour
            QVERIFY(!chatcolors::isTexture(custom3.browser.palette()));
            Chat custom3dark(QStringLiteral("QTextEdit#ChatTab { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #102030, stop:1 #304050); }"));
            const chatcolors::Colors d = chatcolors::fromPalette(custom3dark.browser.palette());
            QCOMPARE(d.base.name(), QStringLiteral("#203040"));
            QVERIFY(d.dark);
        }
        {
            // A rule on the viewport only paints nothing (ts_facts 5.2): the chat stays white.
            Chat custom4(QStringLiteral("QTextEdit#ChatTab > QWidget#qt_scrollarea_viewport { background: #2a1a1a; }"));
            const chatcolors::Colors c = chatcolors::fromPalette(custom4.browser.palette());
            QCOMPARE(c.base.name(), QStringLiteral("#ffffff"));
            QVERIFY(!c.dark);
        }
    }

    void averageAndDominant()
    {
        QLinearGradient g(0, 0, 0, 1);
        g.setColorAt(0, QColor(0, 0, 0));
        g.setColorAt(0.5, QColor(30, 60, 90));
        g.setColorAt(1, QColor(60, 120, 180));
        QCOMPARE(chatcolors::averageOfStops(g).name(), QStringLiteral("#1e3c5a"));

        // A strip of the chat's background with some text on it: the background wins.
        QImage strip(24, 24, QImage::Format_ARGB32);
        strip.fill(QColor(0x2a, 0x1a, 0x1a));
        {
            QPainter p(&strip);
            p.setPen(Qt::white);
            p.drawText(QRect(0, 0, 24, 24), Qt::AlignCenter, QStringLiteral("Hi"));
            p.fillRect(QRect(0, 0, 6, 6), QColor(0xff, 0xff, 0xff));
        }
        QCOMPARE(chatcolors::dominant(strip).name(), QStringLiteral("#2a1a1a"));
        QImage empty(24, 24, QImage::Format_ARGB32);
        empty.fill(Qt::transparent);
        QVERIFY(!chatcolors::dominant(empty).isValid());
        QVERIFY(!chatcolors::dominant(QImage()).isValid());
    }

    void darkAndLight()
    {
        QVERIFY(chatcolors::isDark(QColor(0x2f, 0x31, 0x36)));
        QVERIFY(chatcolors::isDark(QColor(0x30, 0x40, 0x50)));
        QVERIFY(!chatcolors::isDark(QColor(Qt::white)));
        QVERIFY(!chatcolors::isDark(QColor(0x80, 0x80, 0x80))); // black text reads better on a mid grey
        QVERIFY(chatcolors::isDark(QColor(0x60, 0x60, 0x60)));
        QVERIFY(!chatcolors::isDark(QColor()));
    }

    // Automatic on TeamSpeak's two chats gives 2.6's exact strings (tst_coresend keeps its expectations).
    void automaticKeepsTheOldColors()
    {
        const chatcolors::LineColors light = chatcolors::lineColors(chatcolors::teamSpeakLight(), QString());
        QCOMPARE(light.accent, QStringLiteral("#4752c4"));
        QCOMPARE(light.warning, QStringLiteral("#b54708"));
        QVERIFY(!light.wrapBody);
        const chatcolors::LineColors dark = chatcolors::lineColors(solid("#2f3136", "#dcddde"), QString());
        QCOMPARE(dark.accent, QStringLiteral("#949cf7"));
        QCOMPARE(dark.warning, QStringLiteral("#f0b232"));
        QVERIFY(dark.wrapBody);
        QCOMPARE(dark.body, QStringLiteral("#dcddde"));
        // An unknown choice (a hand edit) is Automatic.
        QCOMPARE(chatcolors::lineColors(chatcolors::teamSpeakLight(), QStringLiteral("#ff0000")).accent, QStringLiteral("#4752c4"));
        QCOMPARE(chatcolors::lineColors(solid("#2f3136", "#dcddde"), QStringLiteral("red")).accent, QStringLiteral("#949cf7"));
    }

    // Every choice on every background reads at 4.5:1 (prefix, warning, the body when it is wrapped).
    void presetsReadEverywhere()
    {
        const char* bases[][2] = {{"#ffffff", "#000000"}, {"#2f3136", "#dcddde"}, {"#1e2a38", "#d8dee9"},
                                  {"#121212", "#e0e0e0"}, {"#304050", "#ffffff"}, {"#808080", "#000000"}};
        QStringList choices{QString()};
        for (const char* name : Settings::lineColorChoices)
            choices << QString::fromLatin1(name);
        for (const auto& b : bases) {
            const chatcolors::Colors chat = solid(b[0], b[1]);
            for (const QString& choice : qAsConst(choices)) {
                const chatcolors::LineColors l = chatcolors::lineColors(chat, choice);
                const QString what = QStringLiteral("%1 on %2").arg(choice.isEmpty() ? QStringLiteral("Automatic") : choice, QLatin1String(b[0]));
                QVERIFY2(contrast(l.accent, chat.base) >= 4.5, qPrintable(what + QStringLiteral(" accent ") + l.accent));
                QVERIFY2(contrast(l.warning, chat.base) >= 4.5, qPrintable(what + QStringLiteral(" warning ") + l.warning));
                if (l.wrapBody)
                    QVERIFY2(contrast(l.body, chat.base) >= 4.5, qPrintable(what + QStringLiteral(" body ") + l.body));
                else
                    QVERIFY2(ui::contrastRatio(QColor(0x00, 0x00, 0x8b), chat.base) >= 4.5, qPrintable(what));
            }
        }
        // "Same as the chat text": the chat's own text colour when it already reads.
        QCOMPARE(chatcolors::lineColors(solid("#1e2a38", "#d8dee9"), QStringLiteral("text")).accent, QStringLiteral("#d8dee9"));
        QCOMPARE(chatcolors::lineColors(chatcolors::teamSpeakLight(), QStringLiteral("text")).accent, QStringLiteral("#000000"));
        // A preset keeps its own tone where it reads already, and its hue where it had to change.
        QCOMPARE(chatcolors::lineColors(chatcolors::teamSpeakLight(), QStringLiteral("teal")).accent, QStringLiteral("#0f766e"));
        const QColor onBlack = QColor(chatcolors::lineColors(solid("#121212", "#e0e0e0"), QStringLiteral("blue")).accent);
        QVERIFY(onBlack.blue() > onBlack.red());
    }

    // M8: a picture the style sheet paints never reaches the palette; the sampler draws only the ChatTab's style-sheet
    // background (no paint event) and its colour wins where the palette disagrees about dark and light.
    void samplerSeesPictureBackgrounds()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = dir.filePath(QStringLiteral("chatbg.png"));
        QImage        picture(64, 64, QImage::Format_RGB32);
        picture.fill(QColor(0x2a, 0x1a, 0x1a));
        QVERIFY(picture.save(png));
        Chat chat(QStringLiteral("QTextEdit#ChatTab { background-image: url(%1); color: #e0e0e0; }").arg(png));
        chat.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&chat.window));
        PaintCounter counter;
        chat.browser.installEventFilter(&counter);
        chat.browser.viewport()->installEventFilter(&counter);
        const chatcolors::Colors fromPal = chatcolors::fromPalette(chat.browser.palette());
        QVERIFY(!fromPal.dark); // the palette doesn't know the picture
        const QColor sampled = chatcolors::sampleBackground(&chat.browser);
        QCOMPARE(counter.paints, 0);
        QVERIFY2(sampled.isValid(), "nothing sampled");
        QCOMPARE(sampled.name(), QStringLiteral("#2a1a1a"));
        const chatcolors::Colors effective = chatcolors::withSample(fromPal, sampled, chatcolors::isTexture(chat.browser.palette()));
        QVERIFY(effective.dark);
        QVERIFY(effective.sampled);
        QCOMPARE(effective.base.name(), QStringLiteral("#2a1a1a"));
        chat.browser.removeEventFilter(&counter);
        chat.browser.viewport()->removeEventFilter(&counter);

        // TeamSpeak's dark skin: the sample agrees with the palette, whose exact colour stays.
        Chat dark(QStringLiteral("QTextEdit#ChatTab { background: #2F3136; color: #dcddde; }"));
        const chatcolors::Colors darkPal = chatcolors::fromPalette(dark.browser.palette());
        const QColor             darkSample = chatcolors::sampleBackground(&dark.browser);
        QCOMPARE(darkSample.name(), QStringLiteral("#2f3136"));
        const chatcolors::Colors darkEff = chatcolors::withSample(darkPal, QColor(0x2e, 0x30, 0x35), false);
        QCOMPARE(darkEff.base.name(), QStringLiteral("#2f3136"));
        QVERIFY(!darkEff.sampled);
        // A gradient: the sampler sees its first pixels (the top of a vertical one), still without a paint event.
        Chat gradient(QStringLiteral("QTextEdit#ChatTab { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #102030, stop:1 #304050); }"));
        gradient.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&gradient.window));
        PaintCounter gradientPaints;
        gradient.browser.installEventFilter(&gradientPaints);
        gradient.browser.viewport()->installEventFilter(&gradientPaints);
        const QColor top = chatcolors::sampleBackground(&gradient.browser);
        QCOMPARE(gradientPaints.paints, 0);
        QVERIFY2(top.isValid() && qAbs(top.red() - 0x10) <= 6 && qAbs(top.green() - 0x20) <= 6 && qAbs(top.blue() - 0x30) <= 6, qPrintable(top.name()));
        gradient.browser.removeEventFilter(&gradientPaints);
        gradient.browser.viewport()->removeEventFilter(&gradientPaints);
        // A gradient keeps the average of its stops; a texture takes the sample.
        chatcolors::Colors grad = solid("#203040", "#ffffff");
        QCOMPARE(chatcolors::withSample(grad, QColor(0x10, 0x20, 0x30), false).base.name(), QStringLiteral("#203040"));
        QCOMPARE(chatcolors::withSample(grad, QColor(0x10, 0x20, 0x30), true).base.name(), QStringLiteral("#102030"));
        QVERIFY(!chatcolors::sampleBackground(nullptr).isValid());
    }

    // Review fix: a chat's colours between samples (chatcolors::Cache, what ChatIntegration keeps per chat). A picture
    // background keeps its sampled colours over a new palette until the new sample arrives (no light/dark flicker), and
    // an unchanged background never counts as a change (no restyle of every chat per event).
    void cacheKeepsTheSample()
    {
        QPalette light;
        light.setBrush(QPalette::Active, QPalette::Base, QColor(Qt::white)); // the palette doesn't know the picture
        light.setColor(QPalette::Active, QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
        light.setColor(QPalette::Active, QPalette::Text, QColor(0xe0, 0xe0, 0xe0));
        const QColor picture(0x2a, 0x1a, 0x1a);

        chatcolors::Cache cache;
        QVERIFY(!cache.valid);
        cache.read(light);
        QVERIFY(cache.valid && !cache.sampleDone && !cache.haveSample);
        QVERIFY(!cache.colors.dark); // the palette's answer until the first sample
        QVERIFY(cache.takeSample(picture)); // the first sample changes it: one restyle
        QVERIFY(cache.colors.dark && cache.colors.sampled);
        QCOMPARE(cache.colors.base, picture);

        // A new style or palette (the same picture): read at once, the earlier sample over it; the new sample equals it,
        // so nothing changed.
        cache.invalidate();
        QVERIFY(!cache.valid && !cache.sampleDone);
        cache.read(light);
        QVERIFY(cache.colors.dark); // not the palette's light guess in between
        QCOMPARE(cache.colors.base, picture);
        QVERIFY(!cache.takeSample(picture));
        QVERIFY(cache.colors.dark);

        // A new picture: one change.
        cache.invalidate();
        cache.read(light);
        QVERIFY(cache.takeSample(QColor(0x10, 0x30, 0x10)));
        QCOMPARE(cache.colors.base.name(), QStringLiteral("#103010"));

        // The picture went (a plain light skin): the earlier sample is in use until the new one, which changes it back.
        QPalette white;
        white.setBrush(QPalette::Active, QPalette::Base, QColor(Qt::white));
        white.setColor(QPalette::Active, QPalette::Text, QColor(Qt::black));
        cache.invalidate();
        cache.read(white);
        QVERIFY(cache.colors.dark);
        QVERIFY(cache.takeSample(QColor(Qt::white)));
        QVERIFY(!cache.colors.dark && !cache.colors.sampled);
        QCOMPARE(cache.colors.base, QColor(Qt::white));

        // A plain dark skin, sampled: the palette's exact colour (2.6's #2f3136), and a second sample changes nothing.
        QPalette dark;
        dark.setBrush(QPalette::Active, QPalette::Base, QColor(0x2f, 0x31, 0x36));
        dark.setColor(QPalette::Active, QPalette::Text, QColor(0xdc, 0xdd, 0xde));
        chatcolors::Cache skin;
        skin.read(dark);
        QVERIFY(!skin.takeSample(QColor(0x2e, 0x30, 0x35)));
        QCOMPARE(skin.colors.base.name(), QStringLiteral("#2f3136"));
        skin.invalidate();
        skin.read(dark);
        QVERIFY(!skin.takeSample(QColor(0x2f, 0x31, 0x36)));
    }

    // Review fix (source rule): ChatIntegration's font hook never throws the colours away; only a style or palette
    // change does (ChatScale's styleEvent), and the sampler compares through chatcolors::Cache.
    void colorsNotOnFontChange()
    {
        QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/src/chatintegration.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString ci    = QString::fromUtf8(f.readAll());
        const int     font  = ci.indexOf(QLatin1String("host.fontEvent = [this, b] {"));
        const int     style = ci.indexOf(QLatin1String("host.styleEvent = [this, b] {"));
        QVERIFY(font > 0 && style > font);
        QVERIFY(!ci.mid(font, style - font).contains(QLatin1String("invalidateColors(")));
        QVERIFY(ci.mid(style, 300).contains(QLatin1String("invalidateColors(b)")));
        QVERIFY(ci.contains(QLatin1String("cached->takeSample(chatcolors::sampleBackground(b))")));
    }

    // M7: the one place for our links' tags, in the accent of the chat it goes into; a URL that could leave the tag: none.
    void linkInTheAccent()
    {
        const QString url = QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest#tsmupd-0123456789abcdef-2.7.0");
        ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString()));
        QCOMPARE(ts3::linkBBCode(url, QStringLiteral("Check [for] updates")),
                 QStringLiteral("[url=") + url + QStringLiteral("][color=#4752c4]Check \\[for\\] updates[/color][/url]"));
        ts3::setLineColors(chatcolors::lineColors(solid("#2f3136", "#dcddde"), QString()));
        QVERIFY(ts3::linkBBCode(url, QStringLiteral("x")).contains(QLatin1String("[color=#949cf7]x[/color]")));
        const chatcolors::Colors custom1 = solid("#1e2a38", "#d8dee9");
        ts3::setLineColors(chatcolors::lineColors(custom1, QString()));
        const QString link   = ts3::linkBBCode(url, QStringLiteral("x"));
        const int     at     = link.indexOf(QLatin1String("[color=")) + 7;
        const QString accent = link.mid(at, 7);
        QVERIFY2(contrast(accent, custom1.base) >= 4.5, qPrintable(link));
        QCOMPARE(ts3::lineColors().accent, accent);
        for (const QString& bad : {url + QLatin1Char(']'), QStringLiteral("[b]") + url, url + QStringLiteral(" x"), url + QLatin1Char('"'), QString()})
            QVERIFY(ts3::linkBBCode(bad, QStringLiteral("x")).isEmpty());
    }

    // M8 in the plugin-only ChatIntegration (source checks): the background is sampled in one place, the owned single-shot
    // timer's slot, never in styleFor (it runs in paints); styleFor and the chat modules take the effective colours.
    void samplerOnlyFromItsTimer()
    {
        QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/src/chatintegration.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString ci = QString::fromUtf8(f.readAll());
        QCOMPARE(ci.count(QLatin1String("chatcolors::sampleBackground(")), 1);
        const int sampler = ci.indexOf(QLatin1String("void ChatIntegration::sampleColors()"));
        QVERIFY(sampler > 0);
        QVERIFY(ci.indexOf(QLatin1String("chatcolors::sampleBackground(")) > sampler);
        QVERIFY(ci.contains(QLatin1String("connect(m_sampleTimer, &QTimer::timeout, this, &ChatIntegration::sampleColors);")));
        const int     style = ci.indexOf(QLatin1String("PreviewStyle ChatIntegration::styleFor(QTextBrowser* browser) const"));
        const QString body  = ci.mid(style, ci.indexOf(QLatin1String("\n}\n"), style) - style);
        QVERIFY(body.contains(QLatin1String("chatColorsOf(browser)")));
        QVERIFY(!body.contains(QLatin1String("lightness()"))); // 2.6's guess is gone
        QVERIFY(!ci.contains(QLatin1String("setChatDark(")));
    }
};

TSMEDIA_REGISTER_TEST(TestChatColors)

#include "tst_chatcolors.moc"
