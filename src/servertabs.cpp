#include "servertabs.h"

#include <QApplication>
#include <QMainWindow>
#include <QPointer>
#include <QTabBar>
#include <QVector>

#include "ts3api.h"

namespace servertabs {

namespace {

// A tab's text as TeamSpeak shows it: "&&" is a "&" (a single one marks a shortcut), maybe cut short.
struct Text {
    QString text;
    bool    cut = false;
};

Text clean(const QString& raw)
{
    Text t;
    t.text.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) != QLatin1Char('&')) {
            t.text += raw.at(i);
        } else if (i + 1 < raw.size() && raw.at(i + 1) == QLatin1Char('&')) {
            t.text += QLatin1Char('&');
            ++i;
        }
    }
    t.text = t.text.trimmed();
    if (t.text.endsWith(QLatin1String("..."))) {
        t.text.chop(3);
        t.cut = true;
    } else if (t.text.endsWith(QChar(0x2026))) {
        t.text.chop(1);
        t.cut = true;
    }
    t.text = t.text.trimmed();
    return t;
}

bool shows(const QString& tab, const QString& serverName)
{
    const Text    t    = clean(tab);
    const QString name = serverName.trimmed();
    if (t.text.isEmpty() || name.isEmpty())
        return false;
    return t.text == name || (t.cut && name.startsWith(t.text));
}

bool isChatTabBar(const QTabBar* bar)
{
    return bar->inherits("ChatTabBar") || bar->objectName() == QLatin1String("ChatTabBar")
           || (bar->parentWidget() && bar->parentWidget()->inherits("ChatTabWidget"));
}

QWidget* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMainWindow*>(w) && w->isVisible())
            return w;
    }
    return nullptr;
}

QStringList textsOf(const QTabBar* bar)
{
    QStringList texts;
    for (int i = 0; i < bar->count(); ++i)
        texts << bar->tabText(i);
    return texts;
}

} // namespace

QTabBar* bar()
{
    QWidget*            window      = mainWindow();
    const QList<uint64> connections = ts3::connections();
    if (!window || connections.size() < 2)
        return nullptr;
    QList<QTabBar*> found;
    for (QTabBar* b : window->findChildren<QTabBar*>()) {
        if (b->isVisible() && !isChatTabBar(b) && b->count() == connections.size())
            found << b;
    }
    if (found.size() <= 1)
        return found.value(0, nullptr);
    const QString current = ts3::serverName(ts3::currentConnection());
    QTabBar*      best    = nullptr;
    for (QTabBar* b : qAsConst(found)) {
        if (b->currentIndex() >= 0 && shows(b->tabText(b->currentIndex()), current)) {
            if (best)
                return nullptr; // two of them: can't tell
            best = b;
        }
    }
    return best;
}

int indexFor(const QStringList& tabTexts, const QString& serverName, int rank)
{
    int seen = 0;
    for (int i = 0; i < tabTexts.size(); ++i) {
        if (shows(tabTexts.at(i), serverName) && seen++ == rank)
            return i;
    }
    return -1;
}

bool show(quint64 sch)
{
    const QPointer<QTabBar> tabs = bar();
    const QString           name = sch ? ts3::serverName(sch).trimmed() : QString();
    if (!tabs || name.isEmpty())
        return false;
    if (ts3::currentConnection() == sch)
        return true;
    // sch among the connections to a server of that name (TeamSpeak's tabs are in their order).
    const QList<uint64> connections = ts3::connections();
    int                 rank = 0, same = 0;
    for (const uint64 c : connections) {
        if (ts3::serverName(c).trimmed() != name)
            continue;
        if (c == sch)
            rank = same;
        ++same;
    }
    const QStringList texts  = textsOf(tabs);
    const int         first  = indexFor(texts, name, rank);
    const int         before = tabs->currentIndex();
    const uint64      was    = ts3::currentConnection();
    QVector<int>      tried;
    if (first >= 0 && first != before && same == 1 && indexFor(texts, name, 1) < 0) { // the only tab of the only connection to that server
        tabs->setCurrentIndex(first);
        const uint64 now = ts3::currentConnection();
        if (now == sch || now == was)
            return true; // (TeamSpeak may say so a moment later)
        tried << first;  // 2.3 polish review: another connection's tab after all (its label reads like that name)
    }
    // 2.3 polish review: a tab's text is its label, not always the server's name (live test: a tab opened with
    // a label read "TesterB"; a bookmark's name may be anything), so that tab may show no name at all: the tab
    // at sch's place among the connections is tried right after the best named one.
    QVector<int> tries;
    if (first >= 0 && !tried.contains(first))
        tries << first;
    const int place = connections.indexOf(sch);
    if (place >= 0 && place < texts.size() && !tries.contains(place) && !tried.contains(place))
        tries << place;
    for (int i = 0; i < texts.size(); ++i) {
        if (!tries.contains(i) && !tried.contains(i) && shows(texts.at(i), name))
            tries << i;
    }
    // The one TeamSpeak then says is sch's.
    for (const int index : qAsConst(tries)) {
        if (!tabs)
            return false;
        tabs->setCurrentIndex(index);
        if (ts3::currentConnection() == sch)
            return true;
    }
    if (tabs)
        tabs->setCurrentIndex(before);
    return false;
}

} // namespace servertabs
