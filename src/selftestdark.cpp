// 2.7 Dark mode: the live-test driver's commands (design_dark 4.1, 6). TESTHOOKS builds only (CMakeLists.txt); dispatched
// from SelfTest::run (selftest.cpp).
//   dark.on / dark.off   the Plugins menu pair (the plugin's own path: saved, applied, menus updated; asynchronous)
//   dark.toggle          the hotkey (tsmedia_darkmode)
//   dark.state           enabled, active, paused, the sheet and palette against the snapshot, documents, adoptions, title
//                        bars, the last times, the widget count
//   dark.metrics         the size factor, TeamSpeak's font, the chat's frame and scroll bar width kept from the skin
//   dark.tip             the visible tool tip: its document's colours (TeamSpeak's template colours remapped), recoloured now
//   dark.print {kind, text, url, link}  2.7.1: one of TS Media's own lines in the current server tab, as the plugin prints it:
//                        kind info (default), warning, link (printInfoLink: the channel tab) or raw (ts3::print, BBCode as given)

#include <QApplication>
#include <QFontInfo>
#include <QJsonObject>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QWidget>

#include <functional>

#include "darkchat.h"
#include "darkmode.h"
#include "settings.h"
#include "ts3api.h"

// plugin.cpp's exported handlers (the same DLL): the menu pair and the hotkey go the user's way.
extern "C" void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID);
extern "C" void ts3plugin_onHotkeyEvent(const char* keyword);

namespace {

QString yesNo(bool v)
{
    return v ? QString::fromLatin1("1") : QString::fromLatin1("0");
}

} // namespace

bool selfTestDark(const QString& cmd, const QJsonObject& c, const std::function<void(const QString&)>& say)
{
    if (!cmd.startsWith(QLatin1String("dark.")))
        return false;
    if (cmd == QLatin1String("dark.print")) {
        const uint64  sch  = ts3::currentConnection();
        const QString kind = c.value(QString::fromLatin1("kind")).toString(QString::fromLatin1("info"));
        const QString text = c.value(QString::fromLatin1("text")).toString(QString::fromLatin1("Dark mode test line."));
        if (kind == QLatin1String("warning"))
            ts3::printWarning(sch, text);
        else if (kind == QLatin1String("link"))
            ts3::printInfoLink(sch, text, c.value(QString::fromLatin1("url")).toString(QString::fromLatin1("https://example.com/")),
                               c.value(QString::fromLatin1("link")).toString(QString::fromLatin1("a link")));
        else if (kind == QLatin1String("raw"))
            ts3::print(sch, text);
        else
            ts3::printInfo(sch, text);
        say(QString::fromLatin1("dark.print: %1 printed").arg(kind));
        return true;
    }
    DarkMode* dark = DarkMode::instance();
    if (cmd == QLatin1String("dark.on") || cmd == QLatin1String("dark.off")) {
        ts3plugin_onMenuItemEvent(ts3::currentConnection(), PLUGIN_MENU_TYPE_GLOBAL, cmd == QLatin1String("dark.on") ? 10 : 11, 0);
        say(cmd + QString::fromLatin1(": sent (the menu item)"));
        return true;
    }
    if (cmd == QLatin1String("dark.toggle")) {
        ts3plugin_onHotkeyEvent("tsmedia_darkmode");
        say(QString::fromLatin1("dark.toggle: sent (the hotkey)"));
        return true;
    }
    if (!dark) {
        say(cmd + QString::fromLatin1(": no dark mode"));
        return true;
    }
    if (cmd == QLatin1String("dark.state")) {
        const DarkMode::Stats s       = dark->stats();
        const QPalette        now     = QApplication::palette();
        const QPalette        snap    = dark->snapshotPalette();
        const bool            sheetIs = qApp->styleSheet() == dark->snapshotSheet();
        const bool            ours    = qApp->styleSheet() == dark->appliedSheet();
        say(QString::fromLatin1("dark.state: setting %1, enabled %2, active %3, paused %4; sheet %5 chars, ours %6, snapshot %7 (%8 chars); "
                                "palette snapshot %9 (resolve %10 / %11)")
                .arg(yesNo(Settings::instance().darkMode), yesNo(dark->enabled()), yesNo(DarkMode::isActive()), yesNo(dark->paused()))
                .arg(qApp->styleSheet().size())
                .arg(yesNo(ours), yesNo(sheetIs))
                .arg(dark->snapshotSheet().size())
                .arg(yesNo(now == snap && now.resolve() == snap.resolve()))
                .arg(now.resolve())
                .arg(snap.resolve()));
        say(QString::fromLatin1("dark.state: documents %1, recolored %2, pending %3; adoptions %4; title bars %5; tips %6; filter %7; timers %8; "
                                "on %9 ms (sheet %10 ms, chats %11 ms, %12 fragments), off %13 ms, cards %14 ms; widgets %15 at apply, %16 now")
                .arg(s.tracked)
                .arg(s.recolored)
                .arg(s.pending)
                .arg(s.adoptions)
                .arg(s.titleBars)
                .arg(s.tips)
                .arg(yesNo(dark->hasEventFilter()))
                .arg(dark->timersActive())
                .arg(s.lastApplyMs)
                .arg(s.lastSheetMs)
                .arg(s.lastChatsMs)
                .arg(s.fragments)
                .arg(s.lastRemoveMs)
                .arg(s.lastCardsMs)
                .arg(s.widgets)
                .arg(QApplication::allWidgets().size()));
        return true;
    }
    if (cmd == QLatin1String("dark.metrics")) {
        const darkpalette::Metrics m = dark->metrics();
        say(QString::fromLatin1("dark.metrics: k %1 (font %2 px), chat frame %3 px, chat scroll bar %4 px, glyphs %5")
                .arg(dark->scale())
                .arg(QFontInfo(QApplication::font()).pixelSize())
                .arg(m.chatFrame)
                .arg(m.chatBar)
                .arg(dark->glyphDir().isEmpty() ? QString::fromLatin1("none") : QString::fromLatin1("ok")));
        return true;
    }
    if (cmd == QLatin1String("dark.tip")) {
        QWidget* tip = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName() == QLatin1String("qtooltip_label") && w->isVisible())
                tip = w;
        }
        QTextDocument* doc = tip ? tip->findChild<QTextDocument*>() : nullptr;
        if (!doc) {
            say(QString::fromLatin1("dark.tip: visible %1, document 0").arg(yesNo(tip != nullptr)));
            return true;
        }
        const darkpalette::Tokens& t = darkpalette::tokens();
        darkchat::recolor(doc, darkchat::tipMap(t.link, t.error, t.raised), true);
        int           tagged = 0;
        QSet<QString> colors;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextCharFormat f = it.fragment().charFormat();
                tagged += f.hasProperty(darkchat::kOriginal) ? 1 : 0;
                if (f.hasProperty(QTextFormat::ForegroundBrush))
                    colors.insert(f.foreground().color().name());
            }
        }
        QStringList list = colors.values();
        list.sort();
        say(QString::fromLatin1("dark.tip: visible 1, document 1, recolored fragments %1, colours %2").arg(tagged).arg(list.join(QLatin1Char(' '))));
        return true;
    }
    say(QString::fromLatin1("unknown command ") + cmd);
    return true;
}
