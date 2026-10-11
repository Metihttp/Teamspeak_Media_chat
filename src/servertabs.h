#pragma once

// 2.3 live music (live test): TeamSpeak's server tabs. TeamSpeak sends the voice of the server tab in front
// only, and its plugin API can't bring a tab to the front; the tab bar of its main window can (the live-test
// driver selects server tabs the same way). GUI thread; nothing is kept between calls.
//
//   bar()       TeamSpeak's server tab bar: a visible tab bar of the main window that isn't a chat tab bar
//               (ChatTabBar, or one in a ChatTabWidget) and has a tab per server connection. Where more than
//               one would do, the one whose current tab shows the current connection's server. nullptr when
//               there is none (one server tab: TeamSpeak shows no tab bar) or it can't tell.
//   indexFor()  which tab shows a server: the tabs whose text is the server's name as TeamSpeak shows it ("&&"
//               for "&", maybe cut short with "..."), the rank-th of them when connections share the name
//               (TeamSpeak adds a tab per connection, in the order of its connection list); -1: none.
//   show()      brings sch's tab to the front; false when it can't tell which tab that is. With several tabs
//               of one name it checks TeamSpeak's current connection after each try and goes back to the
//               tab that was in front when none of them is sch's. A tab's text is its label (2.3 polish
//               review: a bookmark's name, or what the connection was opened with), so the tab at sch's place
//               among the connections is tried too, right after the best named one.

#include <QString>
#include <QStringList>

class QTabBar;

namespace servertabs {

QTabBar* bar();
int      indexFor(const QStringList& tabTexts, const QString& serverName, int rank);
bool     show(quint64 sch);

} // namespace servertabs
