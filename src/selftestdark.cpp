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
//   dark.sheets          2.7.2 live: each text view's document style sheet (length, hash, TeamSpeak's dark-mode chat file in it)
//   dark.scan            2.7.2 live (D6): every text document of every window (chats, the info panel, poke windows, documents
//                        TeamSpeak swapped out that a widget still owns): dark mode's kOriginal on fragments, inside the formats
//                        they keep, on block character formats, block and frame formats, and on each view's own char format;
//                        2.7.3 (M5): "dark looks", a dark token's colour on block character formats, table cells, TeamSpeak's
//                        own pictures and each view's own format (after off: 0)

#include <QApplication>
#include <QFile>
#include <QFontInfo>
#include <QTextEdit>
#include <QTextFrame>
#include <QJsonObject>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextTable>
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

// 2.7.3 (M5): f's own colour is one of dark mode's text tokens (after off: a dark look left behind).
int darkLook(const QTextFormat& f)
{
    if (!f.hasProperty(QTextFormat::ForegroundBrush))
        return 0;
    const QRgb                 rgb = f.brushProperty(QTextFormat::ForegroundBrush).color().rgba();
    const darkpalette::Tokens& t   = darkpalette::tokens();
    for (const QColor& c : {t.text, t.secondary, t.muted, t.link, t.success, t.error, t.warning, t.accent}) {
        if (c.rgba() == rgb)
            return 1;
    }
    return 0;
}

// kOriginal on f itself (1) plus inside the formats, lists and maps its properties hold.
int darkIn(const QTextFormat& f, int depth = 0)
{
    if (depth > 8)
        return 0;
    int        n     = f.hasProperty(darkchat::kOriginal) ? 1 : 0;
    const auto visit = [&n, depth](const QVariant& v, const auto& self) -> void {
        if (v.userType() == QMetaType::QTextFormat) {
            n += darkIn(qvariant_cast<QTextFormat>(v), depth + 1);
        } else if (v.userType() == QMetaType::QVariantList) {
            for (const QVariant& e : v.toList())
                self(e, self);
        } else if (v.userType() == QMetaType::QVariantMap) {
            const QVariantMap m = v.toMap();
            for (auto it = m.cbegin(); it != m.cend(); ++it)
                self(it.value(), self);
        }
    };
    const QMap<int, QVariant> props = f.properties();
    for (auto it = props.cbegin(); it != props.cend(); ++it)
        visit(it.value(), visit);
    return n;
}

QString ownerOf(QObject* o)
{
    QStringList path;
    for (int i = 0; o && i < 3; ++i, o = o->parent())
        path << QString::fromLatin1("%1#%2").arg(QString::fromLatin1(o->metaObject()->className()), o->objectName());
    return path.join(QLatin1Char('<'));
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
    if (cmd == QLatin1String("dark.sheets")) {
        // 2.7.2 live: every text view's document style sheet (TeamSpeak's _chat.qss, with its _chat_darkmode.qss when it
        // made the chat under a dark palette): length, hash and whether it holds the dark-mode file's header; each
        // distinct sheet to debug/sheet_<hash>.txt.
        QSet<QTextDocument*> seen;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            for (QTextEdit* e : top->findChildren<QTextEdit*>()) {
                QTextDocument* d = e->document();
                if (seen.contains(d))
                    continue;
                seen.insert(d);
                const QString sheet = d->defaultStyleSheet();
                const QString hash  = QString::number(qHash(sheet), 16);
                say(QString::fromLatin1("dark.sheets: %1 visible %2 sheet %3 chars hash %4 darkmode-file %5")
                        .arg(ownerOf(e), yesNo(e->isVisible()))
                        .arg(sheet.size())
                        .arg(hash, yesNo(sheet.contains(QLatin1String("in dark mode")))));
                QFile out(ts3::dataDir() + QString::fromLatin1("/debug/sheet_") + hash + QString::fromLatin1(".txt"));
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    out.write(sheet.toUtf8());
            }
        }
        return true;
    }
    if (cmd == QLatin1String("dark.scan")) {
        QSet<QTextDocument*>          docs;
        QHash<QTextDocument*, QTextEdit*> views;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            for (QTextEdit* e : top->findChildren<QTextEdit*>()) {
                docs.insert(e->document());
                views.insert(e->document(), e);
            }
            for (QTextDocument* d : top->findChildren<QTextDocument*>())
                docs.insert(d);
        }
        int totalFrag = 0, totalNested = 0, totalBlockChar = 0, totalBlock = 0, totalFrame = 0, totalCursor = 0, totalCollection = 0, fragments = 0;
        int totalLooks = 0;
        for (QTextDocument* d : docs) {
            int frag = 0, nested = 0, blockChar = 0, block = 0, frame = 0, cursor = 0, collection = 0, n = 0;
            // 2.7.3 (M5): dark looks (a dark token's colour) on block character formats, table cell formats, TeamSpeak's own
            // pictures and the view's own format: 0 after off.
            int looks = 0;
            for (QTextBlock tb = d->begin(); tb.isValid(); tb = tb.next()) {
                blockChar += darkIn(tb.charFormat());
                block += darkIn(tb.blockFormat());
                looks += darkLook(tb.charFormat());
                for (auto it = tb.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fr = it.fragment();
                    if (!fr.isValid())
                        continue;
                    ++n;
                    const QTextCharFormat cf = fr.charFormat();
                    frag += cf.hasProperty(darkchat::kOriginal) ? 1 : 0;
                    nested += darkIn(cf) - (cf.hasProperty(darkchat::kOriginal) ? 1 : 0);
                    if (cf.isImageFormat() && !darkchat::isModuleFormat(cf))
                        looks += darkLook(cf);
                }
            }
            frame += darkIn(d->rootFrame()->frameFormat());
            const std::function<void(QTextFrame*)> cells = [&looks, &cells](QTextFrame* parent) {
                for (QTextFrame* f : parent->childFrames()) {
                    if (auto* table = qobject_cast<QTextTable*>(f)) {
                        for (int r = 0; r < table->rows(); ++r) {
                            for (int col = 0; col < table->columns(); ++col)
                                looks += darkLook(table->cellAt(r, col).format());
                        }
                    }
                    cells(f);
                }
            };
            cells(d->rootFrame());
            for (QTextFrame* f : d->rootFrame()->childFrames())
                frame += darkIn(f->frameFormat());
            for (const QTextFormat& f : d->allFormats())
                collection += f.hasProperty(darkchat::kOriginal) ? 1 : 0;
            QTextEdit* v = views.value(d);
            if (v) {
                cursor = darkIn(v->currentCharFormat());
                looks += darkLook(v->currentCharFormat());
            }
            fragments += n;
            totalLooks += looks;
            totalFrag += frag;
            totalNested += nested;
            totalBlockChar += blockChar;
            totalBlock += block;
            totalFrame += frame;
            totalCursor += cursor;
            totalCollection += collection;
            if (frag || nested || blockChar || block || frame || cursor || collection || (looks && !DarkMode::isActive()) || c.value(QString::fromLatin1("all")).toBool())
                say(QString::fromLatin1("dark.scan: %1%2 blocks %3 fragments %4: tagged %5, nested %6, block char %7, block %8, frame %9, cursor %10 (format collection %11), dark looks %12")
                        .arg(ownerOf(v ? static_cast<QObject*>(v) : d), v ? QString() : QString::fromLatin1(" [no view]"))
                        .arg(d->blockCount())
                        .arg(n)
                        .arg(frag)
                        .arg(nested)
                        .arg(blockChar)
                        .arg(block)
                        .arg(frame)
                        .arg(cursor)
                        .arg(collection)
                        .arg(looks));
        }
        say(QString::fromLatin1("dark.scan: %1 documents, %2 fragments: tagged %3, nested %4, block char %5, block %6, frame %7, cursor %8 (collection formats with it, used or not: %9); dark looks on separators, cells, TeamSpeak's pictures and view formats %10")
                .arg(docs.size())
                .arg(fragments)
                .arg(totalFrag)
                .arg(totalNested)
                .arg(totalBlockChar)
                .arg(totalBlock)
                .arg(totalFrame)
                .arg(totalCursor)
                .arg(totalCollection)
                .arg(totalLooks));
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
