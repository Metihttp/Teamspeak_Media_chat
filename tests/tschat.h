#pragma once

// Test helper: chat documents built the way TeamSpeak 3.6.2 fills its chat (S0, screenshots of the dark
// skin, TeamSpeak's chat logs and the live test's chat dumps): one block per line, appended at the
// end like TeamSpeak does (a new block that copies the last block's formats, then the line's HTML). Messages
//   [icon MESSAGE_INCOMING/OUTGOING]<16:00:17> "Nick": text
// with the nickname linked to client://<clid>/<uid>~<nick>; the channel tab's system lines
//   [icon MESSAGE_INFO]<15:56:16> *** You are now talking in channel: "Home"
// while the server tab and private chats print their status lines without "*** " (statusHtml):
//   [icon MESSAGE_INFO]<21:49:32> You switched from channel "Default Channel" to "L4 Second"
//   [icon MESSAGE_INFO]<21:49:32> Channel group "Guest" was assigned to "TesterA" by "TeamSpeak ]I[ Server".
//   [icon MESSAGE_INFO]<21:53:16> Chat partner disconnected out of view.
// (names and channels linked, groups quoted as text); day lines *** 9/28/2026 (bold, italic; no icon in the
// screenshots, with one too) and history markers [icon MESSAGE_INFO]*** Chat begins 2026-10-10 04:44:02 ("Log
// begins" in the server tab; no time). Header only (tests, render_gallery, the harness).

#include <QString>
#include <QTextCursor>
#include <QTextDocument>

namespace tschat {

struct Skin {
    bool    dark = true;
    QString time;   // the header's time
    QString nick;   // nicknames and channel links
    QString own;    // the own nickname (TeamSpeak colours it like the others)
    QString link;   // links in messages
    QString system; // system lines
    QString day;    // day lines
    QString text;   // the body (the palette's text: no colour in the HTML)
};

inline Skin darkSkin()
{
    Skin s;
    s.dark   = true;
    s.time   = QStringLiteral("#607d8b");
    s.nick   = QStringLiteral("#1cb0f4");
    s.own    = QStringLiteral("#1cb0f4");
    s.link   = QStringLiteral("#1cb0f4");
    s.system = QStringLiteral("#607d8b");
    s.day    = QStringLiteral("#707475");
    s.text   = QStringLiteral("#dcddde");
    return s;
}

inline Skin lightSkin()
{
    Skin s;
    s.dark   = false;
    s.time   = QStringLiteral("#33597d");
    s.nick   = QStringLiteral("#002f5d");
    s.own    = QStringLiteral("#002f5d");
    s.link   = QStringLiteral("#1c82cc");
    s.system = QStringLiteral("#33597d");
    s.day    = QStringLiteral("#707475");
    s.text   = QStringLiteral("#000000");
    return s;
}

// Like TeamSpeak: a new block at the end (it copies the last block's formats), then the line.
inline void append(QTextDocument* doc, const QString& html)
{
    QTextCursor c(doc);
    c.movePosition(QTextCursor::End);
    if (!doc->isEmpty())
        c.insertBlock();
    c.insertHtml(html);
}

inline QString escaped(const QString& text)
{
    QString out = text.toHtmlEscaped();
    out.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return out;
}

inline QString icon(const char* kind)
{
    return QStringLiteral("<img src=\"iconpath:%1?size=13x13\" width=\"13\" height=\"13\">").arg(QString::fromLatin1(kind));
}

inline QString linkHtml(const Skin& s, const QString& url, const QString& label = QString())
{
    return QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:underline\">%3</a>").arg(url.toHtmlEscaped(), s.link, escaped(label.isEmpty() ? url : label));
}

inline QString clientHtml(const Skin& s, int clid, const QString& uid, const QString& nick)
{
    return QStringLiteral("<a href=\"client://%1/%2~%3\" style=\"color:%4;font-weight:bold;text-decoration:none\">\"%5\"</a>")
        .arg(clid)
        .arg(uid.toHtmlEscaped(), nick.toHtmlEscaped(), s.nick, escaped(nick));
}

inline QString channelHtml(const Skin& s, const QString& name)
{
    return QStringLiteral("<a href=\"channelid://1\" style=\"color:%1;font-weight:bold;text-decoration:none\">\"%2\"</a>").arg(s.nick, escaped(name));
}

// time: "16:00:17" (empty: timestamps off).
inline QString messageHtml(const Skin& s, const QString& time, int clid, const QString& uid, const QString& nick, const QString& bodyHtml, bool outgoing = false)
{
    QString html = icon(outgoing ? "MESSAGE_OUTGOING" : "MESSAGE_INCOMING");
    if (!time.isEmpty())
        html += QStringLiteral("<span style=\"color:%1\">&lt;%2&gt;</span> ").arg(s.time, time);
    return html + clientHtml(s, clid, uid, nick) + QStringLiteral(": ") + bodyHtml;
}

inline QString systemHtml(const Skin& s, const QString& time, const QString& textHtml)
{
    QString html = icon("MESSAGE_INFO") + QStringLiteral("<span style=\"color:%1\">").arg(s.system);
    if (!time.isEmpty())
        html += QStringLiteral("&lt;%1&gt; ").arg(time);
    return html + QStringLiteral("*** ") + textHtml + QStringLiteral("</span>");
}

inline QString dayHtml(const Skin& s, const QString& date, bool withIcon = true)
{
    return (withIcon ? icon("MESSAGE_INFO") : QString()) + QStringLiteral("<span style=\"color:%1;font-weight:bold;font-style:italic\">*** %2</span>").arg(s.day, date);
}

// A status line of the server tab or a private chat (no "*** "): TeamSpeak's info icon in the line's colour,
// the time in its own, a space, then the text. color: the line's (the skin's system colour by default;
// TeamSpeak's dark skin uses #7289da for channel and group events and a bold #0aa537 for its welcome).
inline QString statusHtml(const Skin& s, const QString& time, const QString& textHtml, const QString& color = QString(), bool bold = false)
{
    QString html = QStringLiteral("<span style=\"color:%1%2\">").arg(color.isEmpty() ? s.system : color, bold ? QStringLiteral(";font-weight:bold") : QString()) + icon("MESSAGE_INFO");
    if (!time.isEmpty())
        html += QStringLiteral("<span style=\"color:%1;font-weight:normal\">&lt;%2&gt;</span>").arg(s.time, time);
    return html + QStringLiteral(" ") + textHtml + QStringLiteral("</span>");
}

// TeamSpeak's history marker where a logged session begins: "*** Chat begins 2026-10-10 04:44:02" (channel tab,
// private chats) or "*** Log begins ..." (server tab), with the info icon and no time.
inline QString historyHtml(const Skin& s, const QString& dateTime, bool serverTab = false)
{
    return QStringLiteral("<span style=\"color:%1\">").arg(s.system) + icon("MESSAGE_INFO")
           + QStringLiteral("*** %1 begins %2</span>").arg(serverTab ? QStringLiteral("Log") : QStringLiteral("Chat"), dateTime);
}

inline QString channelHtml(const Skin& s, int id, const QString& name)
{
    return QStringLiteral("<a href=\"channelid://%1\" style=\"color:%2;font-weight:bold;text-decoration:none\">\"%3\"</a>").arg(id).arg(s.nick, escaped(name));
}

// ts3api's printInfo: "TS Media chat" in bold in its colour, then the text.
inline QString printHtml(const Skin& s, const QString& text)
{
    return QStringLiteral("<span style=\"color:%1\"><span style=\"color:%2;font-weight:bold\">TS Media chat</span> %3</span>")
        .arg(s.dark ? QStringLiteral("#dcddde") : QStringLiteral("#00008b"), s.dark ? QStringLiteral("#949cf7") : QStringLiteral("#4752c4"), escaped(text));
}

const char kKaiUid[]  = "kTkVNcL7gLaJrl7oa+KDuugw/Zo=";
const char kNoraUid[] = "uUkyXDSdRY4Ue/wBSNnIicdm+YI=";
const char kDexUid[]  = "aDEcQO1on3kRGkZ1vMTCIiFNme0=";

// A dark-skin sample chat (made-up people and links): Kai's links, channel switches, day lines, Nora and Dex-.
inline void fillSampleChat(QTextDocument* doc, const Skin& s)
{
    const QString kai  = QString::fromLatin1(kKaiUid);
    const QString nora = QString::fromLatin1(kNoraUid);
    const QString dex  = QString::fromLatin1(kDexUid);
    const auto    k    = [&](const char* time, const char* url) {
        append(doc, messageHtml(s, QString::fromLatin1(time), 7, kai, QStringLiteral("Kai"), linkHtml(s, QString::fromLatin1(url)), true));
    };
    const auto n = [&](const char* time, const QString& body) { append(doc, messageHtml(s, QString::fromLatin1(time), 9, nora, QStringLiteral("Nora"), body)); };
    append(doc, systemHtml(s, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + channelHtml(s, QStringLiteral("Home"))));
    k("16:00:17", "https://example.com/live/first-stream");
    k("16:00:57", "https://example.com/live/night-owl");
    k("17:19:08", "https://example.com/live/river-run");
    k("17:26:07", "https://example.com/live/summit1989");
    k("17:26:40", "https://example.com/live/quiet-shadow");
    n("17:45:37", linkHtml(s, QStringLiteral("https://example.org/watch/vista")));
    k("17:46:20", "https://example.com/live/harbor-view");
    k("18:10:52", "https://example.com/live/maple-leaf");
    append(doc, systemHtml(s, QStringLiteral("20:02:09"), QStringLiteral("You are now talking in channel: ") + channelHtml(s, QStringLiteral("Home"))));
    append(doc, messageHtml(s, QStringLiteral("22:05:25"), 12, dex, QStringLiteral("Dex-"), QStringLiteral("panel.example.org")));
    n("22:59:30", linkHtml(s, QStringLiteral("https://example.org/partners/apply")));
    n("23:06:36", linkHtml(s, QStringLiteral("mailto:user@example.com"), QStringLiteral("user@example.com")));
    n("23:06:42", QStringLiteral("Sample1010@"));
    append(doc, dayHtml(s, QStringLiteral("9/28/2026"), false)); // day lines have no icon (as the dark skin prints them)
    append(doc, systemHtml(s, QStringLiteral("20:34:51"), QStringLiteral("You are now talking in channel: ") + channelHtml(s, QStringLiteral("Home"))));
    append(doc, dayHtml(s, QStringLiteral("9/29/2026"), false));
    append(doc, systemHtml(s, QStringLiteral("15:55:26"), QStringLiteral("You are now talking in channel: ") + channelHtml(s, QStringLiteral("Home"))));
    n("17:55:29", linkHtml(s, QStringLiteral("https://store.example.com/app/1234567/A_Sample_Game_Title/")));
    k("18:46:59", "https://example.com/images/stars-icon-hero-home.png");
    n("20:13:37", QStringLiteral("WARP"));
}

const char kTesterUid[] = "TX9dkL2PT/zT4sK2RHejI+ISAww=";

// The server tab of the live test (TeamSpeak's dark skin, its real lines): the log's history marker, the
// connection, TeamSpeak's welcome, a channel made, then alternating moves and channel group assignments
// (blocks 5 to 10), someone connecting and leaving. 13 blocks.
inline void fillServerTab(QTextDocument* doc, const Skin& s)
{
    const QString server = QStringLiteral("<a href=\"channelid://0\" style=\"color:%1;font-weight:bold;text-decoration:none\">\"TeamSpeak ]I[ Server\"</a>").arg(s.nick);
    const QString tester = clientHtml(s, 174, QString::fromLatin1(kTesterUid), QStringLiteral("TesterA"));
    const QString purple = QStringLiteral("#7289da");
    append(doc, historyHtml(s, QStringLiteral("2026-10-10 21:40:46"), true));
    append(doc, statusHtml(s, QStringLiteral("21:40:46"), QStringLiteral("Trying to connect to server on <b>127.0.0.1</b>")));
    append(doc, statusHtml(s, QStringLiteral("21:40:46"), QStringLiteral("Welcome to TeamSpeak, check ") + linkHtml(s, QStringLiteral("http://www.teamspeak.com"), QStringLiteral("www.teamspeak.com")) + QStringLiteral(" for latest information"),
                           QStringLiteral("#0aa537"), true));
    append(doc, statusHtml(s, QStringLiteral("21:40:46"), QStringLiteral("Connected to Server: ") + server));
    append(doc, statusHtml(s, QStringLiteral("21:49:21"), QStringLiteral("Channel ") + channelHtml(s, 4, QStringLiteral("L4 Second")) + QStringLiteral(" created by ") + clientHtml(s, 177, QStringLiteral("serveradmin"), QStringLiteral("serveradmin")), purple));
    for (int i = 0; i < 3; ++i) {
        const QString time = QStringLiteral("21:49:%1").arg(32 + 2 * i);
        const bool    out  = i % 2 == 0;
        append(doc, statusHtml(s, time, QStringLiteral("You switched from channel ") + channelHtml(s, out ? 1 : 4, out ? QStringLiteral("Default Channel") : QStringLiteral("L4 Second")) + QStringLiteral(" to ")
                                            + channelHtml(s, out ? 4 : 1, out ? QStringLiteral("L4 Second") : QStringLiteral("Default Channel"))));
        append(doc, statusHtml(s, time, QStringLiteral("Channel group \"Guest\" was assigned to ") + tester + QStringLiteral(" by ") + server + QStringLiteral("."), purple));
    }
    append(doc, statusHtml(s, QStringLiteral("21:50:02"), clientHtml(s, 179, QStringLiteral("serveradmin"), QStringLiteral("Nora")) + QStringLiteral(" connected to channel ") + channelHtml(s, 1, QStringLiteral("Default Channel"))));
    append(doc, statusHtml(s, QStringLiteral("21:50:40"), clientHtml(s, 179, QStringLiteral("serveradmin"), QStringLiteral("Nora")) + QStringLiteral(" disconnected (leaving)")));
}

} // namespace tschat
