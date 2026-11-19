// 2.7 Dark mode (design_dark 5): the tokens' contrast, the sheets (placeholders, size, no font, parse warnings), the exact
// restore (sheet, palette, widget palettes, document sheets, every fragment, the pixels), the order of the way back, a
// skin picked while dark mode is on, the loop guard, the chat's recolouring both ways, lines added while dark, hidden
// chats, every chat's place (also at 200 %: QT_FONT_DPI=192 and QT_SCALE_FACTOR=2 in child processes), unload, title
// bars (a fake DwmSetWindowAttribute), the timing budget, the glyphs, tool tips, the Settings key and card, TS Media's
// tokens and line colours, the Plugins menu pair and the source rules (CRASH RULE). A test "skin" written here (no
// TeamSpeak file is read).

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLibrary>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextTable>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>
#include <QtTest>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <tuple>

#include "appearancesection.h"
#include "chatcolors.h"
#include "chatscale.h"
#include "chatlayoutsection.h"
#include "chatscroll.h"
#include "chattextsection.h"
#include "composesettings.h"
#include "composewidgets.h"
#include "compresssettings.h"
#include "darkchat.h"
#include "darkglyphs.h"
#include "darkmode.h"
#include "darkpalette.h"
#include "darktitlebar.h"
#include "emojiformat.h"
#include "emojisection.h"
#include "fakets3.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "polldoc.h"
#include "polldialog.h"
#include "voicepanel.h"
#include "privacysection.h"
#include "replydoc.h"
#include "settings.h"
#include "settingsdialog.h"
#include "settingstheme.h"
#include "settingswidgets.h"
#include "spoilersection.h"
#include "testmain.h"
#include "textspoilerdoc.h"
#include "ts3api.h"
#include "tschat.h"
#include "uiutil.h"

namespace {

QString a(const char* text)
{
    return QString::fromUtf8(text);
}

QString sourceText(const QString& relative)
{
    QFile f(QString::fromUtf8(TSMEDIA_SOURCE_DIR) + QLatin1Char('/') + relative);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// ---- a test skin, written like TeamSpeak's (its object names, palette(base), a qproperty, the chat classes) ----------

const char kSkinSheet[] = "QTextEdit#ChatTab { background: palette(base); color: #000000; border: 1px solid #a0a0a0; }\n"
                          "QTextEdit#InfoFrame { background: palette(base); border: none; }\n"
                          "QTreeView#ServerTreeView { qproperty-rootIsDecorated: false; }\n"
                          "QTextEdit#ChatLineEdit[helpTextVisible=\"true\"] { color: #808080; }\n"
                          // The fonts TeamSpeak's default.qss gives its headlines and banners (review fix: dark mode keeps them).
                          "QLabel#OptionsHeadlineTitleLabel { font: 18px Arial; color: #202020; }\n"
                          "QLabel#bookmarksBanner, QLabel#identitiesBanner { font-weight: bold; }\n";

const char kSkinChat[] = "p { padding: 0px; margin: 0px; }\n"
                         ".TextMessage_UserLink { color: #002f5d; font-weight: bold; text-decoration: none; }\n"
                         ".TextMessage_ChannelLink, .TextMessage_ServerLink, .TextMessage_OfflineLink { color: #002f5d; font-weight: bold; text-decoration: none; }\n"
                         ".TextMessage_Time { color: #33597d; font-weight: normal; }\n" // as TeamSpeak's (2.7.2: its weight too)
                         ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Connected { color: #33597d; }\n"
                         ".TextMessage_Welcome, .TextMessage_Poke { color: #087d22; }\n"
                         ".TextMessage_ClientKicked { color: #c9070a; }\n"
                         ".TextMessage_ServerError { color: #c9070a; font-weight: bold; }\n"
                         ".InfoFrame_Title { font-size: x-large; }\n"
                         ".Highlight { color: palette(link); }\n" // as TeamSpeak's default_chat.qss
                         ".Active { color: #087d22; }\n"
                         ".Inactive { color: #808080; }\n"
                         ".InfoFrame_Table td.Label { font-weight: bold; }\n";

// Another skin (Options → Design while dark mode is on).
const char kOtherSheet[] = "QTextEdit#ChatTab { background: palette(base); color: #101010; border: 2px solid #406080; }\n"
                           "QTreeView#ServerTreeView { qproperty-rootIsDecorated: false; }\n"
                           "QPushButton { padding: 3px; }\n";

const char kOtherChat[] = "p { padding: 0px; margin: 0px; }\n"
                          ".TextMessage_UserLink { color: #5a1d00; font-weight: bold; text-decoration: none; }\n"
                          ".TextMessage_Time { color: #6b4a2f; }\n"
                          ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft { color: #6b4a2f; }\n"
                          ".TextMessage_ClientKicked { color: #a00000; }\n";

// 2.7.2: skins whose chat text style isn't dark mode's (synthesized; no skin file is copied). kPlainChat: colours only,
// the way TeamSpeak's add-on dark chat file is written (names and channels not bold, links underlined by Qt, history neither
// bold nor italic, no info title size, no label weight; its own paragraph rule here). kCommunityChat: TeamSpeak's weights,
// a bold welcome, a host name without a colour, every info panel class coloured.
const char kPlainChat[] = "p { padding: 0px; margin: 0px; }\n"
                          ".TextMessage_UserLink, .TextMessage_OfflineLink, .TextMessage_ChannelLink, .TextMessage_ServerLink { color: #1f5fbf; }\n"
                          ".TextMessage_Welcome, .TextMessage_Poke { color: #1d8a3a; }\n"
                          ".TextMessage_ClientKicked { color: #c4161c; }\n"
                          ".TextMessage_HostName, .TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Connected, "
                          ".TextMessage_Time { color: #4a6f96; }\n"
                          ".TextMessage_BeginHistory, .TextMessage_EndHistory { color: #6d6d6d; }\n"
                          ".TextMessage_ServerError { color: #d02a2d; }\n"
                          ".InfoFrame_Title .TextMessage_UserLink, .InfoFrame_Title .TextMessage_ChannelLink, .InfoFrame_Title .TextMessage_ServerLink { color: #36507a; }\n"
                          ".InfoFrame_Title .InfoFrame_CustomNick { color: palette(link); }\n"
                          ".Highlight, .InfoFrame_Table td.Highlight { color: palette(link); }\n"
                          ".Important, .InfoFrame_Table td.Important { color: #e00000; }\n"
                          ".Active, .InfoFrame_Table td.Active { color: #2e8b2e; }\n"
                          ".Inactive, .InfoFrame_Table td.Inactive { color: #8a8a8a; }\n";

const char kCommunityChat[] = "p { padding: 0px; margin: 0px; }\n"
                              ".TextMessage_UserLink, .TextMessage_ChannelLink, .TextMessage_ServerLink { color: #127fb0; font-weight: bold; text-decoration: none; }\n"
                              ".TextMessage_OfflineLink { color: #a07d00; font-weight: bold; text-decoration: none; }\n"
                              ".TextMessage_HostName { font-weight: bold; }\n"
                              ".TextMessage_Welcome, .TextMessage_Poke { color: #08802b; font-weight: bold; }\n"
                              ".TextMessage_Normal, .TextMessage_Connected, .TextMessage_ClientJoined, .TextMessage_ClientLeft { color: #4f6773; }\n"
                              ".TextMessage_ClientKicked { color: #b0060a; }\n"
                              ".TextMessage_Time { color: #4f6773; font-weight: normal; }\n"
                              ".TextMessage_BeginHistory, .TextMessage_EndHistory { color: #5d6162; font-weight: bold; font-style: italic; }\n"
                              ".TextMessage_ServerError { color: #b0060a; font-weight: bold; }\n"
                              ".InfoFrame_Title { font-size: x-large; }\n"
                              ".InfoFrame_Title .TextMessage_UserLink, .InfoFrame_Title .TextMessage_ChannelLink, .InfoFrame_Title .TextMessage_ServerLink { color: #127fb0; font-weight: normal; }\n"
                              ".InfoFrame_Title .InfoFrame_CustomNick { color: #127fb0; }\n"
                              ".Highlight, .InfoFrame_Table td.Highlight { color: #127fb0; }\n"
                              ".Important, .InfoFrame_Table td.Important { color: #c62f2e; }\n"
                              ".Active, .InfoFrame_Table td.Active { color: #2f8a52; }\n"
                              ".Inactive, .InfoFrame_Table td.Inactive { color: #5a5e64; }\n"
                              ".InfoFrame_Table td.Label { font-weight: bold; }\n"
                              "a { color: #127fb0; }\n";

// 2.7.3 (M9): colours only, written like TeamSpeak's add-on dark chat file: no paragraph rule (Qt's own 12 px paragraph
// margins), no table or cell spacing, no info title size.
const char kBareChat[] = ".TextMessage_UserLink, .TextMessage_OfflineLink, .TextMessage_ChannelLink, .TextMessage_ServerLink { color: #1f5fbf; }\n"
                         ".TextMessage_Welcome, .TextMessage_Poke { color: #1d8a3a; }\n"
                         ".TextMessage_ClientKicked { color: #c4161c; }\n"
                         ".TextMessage_HostName, .TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Connected, "
                         ".TextMessage_Time { color: #4a6f96; }\n"
                         ".TextMessage_BeginHistory, .TextMessage_EndHistory { color: #6d6d6d; }\n"
                         ".TextMessage_ServerError { color: #d02a2d; }\n"
                         ".InfoFrame_Title .InfoFrame_CustomNick, .Highlight { color: palette(link); }\n"
                         ".Important { color: #e00000; }\n"
                         ".Active { color: #2e8b2e; }\n"
                         ".Inactive { color: #8a8a8a; }\n";

// The test skin with what TeamSpeak's own chat sheet has besides (a host name colour, the info title's links, the custom
// nickname): the 2.7.2 tests' "TeamSpeak's skin" row.
QString teamSpeakLikeChat()
{
    return QString::fromLatin1(kSkinChat)
           + QString::fromLatin1(".TextMessage_HostName { color: #33597d; font-weight: bold; }\n"
                                 ".TextMessage_BeginHistory, .TextMessage_EndHistory { color: #707475; font-weight: bold; font-style: italic; }\n"
                                 ".InfoFrame_Title .TextMessage_UserLink, .InfoFrame_Title .TextMessage_ChannelLink, .InfoFrame_Title .TextMessage_ServerLink "
                                 "{ color: #191970; font-weight: normal; }\n"
                                 ".InfoFrame_Title .InfoFrame_CustomNick { color: palette(link); }\n");
}

QString chatLine(const QString& time, const QString& cls, const QString& body)
{
    return QString::fromLatin1("<p class=\"%1\"><span class=\"TextMessage_Time\">&lt;%2&gt;</span> %3</p>").arg(cls, time, body);
}

QString user(const QString& name)
{
    return QString::fromLatin1("\"<a class=\"TextMessage_UserLink\" href=\"client://0/x~%1\">%1</a>\"").arg(name);
}

QStringList someLines(int n, bool wrapping)
{
    QStringList out;
    for (int i = 0; i < n; ++i) {
        switch (i % 5) {
        case 0:
            out << chatLine(a("20:58:02"), a("TextMessage_ClientJoined"), user(a("Bob")) + a(" entered your channel"));
            break;
        case 1:
            out << chatLine(a("21:03:11"), a("TextMessage_Message Incoming Public"),
                            user(a("Alice")) + a(": <span class=\"TextMessage_Text\">Hi Bob, are we still on for tonight?</span>"));
            break;
        case 2:
            out << chatLine(a("21:03:29"), a("TextMessage_Message Incoming Public"),
                            user(a("Bob")) + a(": <span class=\"TextMessage_Text\">The map: <a href=\"https://example.com/m\">https://example.com/m</a></span>"));
            break;
        case 3:
            out << chatLine(a("21:03:52"), a("TextMessage_Message Incoming Public"),
                            user(a("Bob")) + a(": <span class=\"TextMessage_Text\"><span style=\"color:#4752c4;font-weight:600\">TS Media chat</span> Sent a.pdf.</span>"));
            break;
        default:
            out << chatLine(a("21:04:15"), a("TextMessage_Message Incoming Public"),
                            user(a("Dana")) + QString::fromLatin1(": <span class=\"TextMessage_Text\">%1</span>")
                                                  .arg(wrapping ? QString::fromLatin1("A long message that wraps over several lines in the chat. ").repeated(4 + i % 3)
                                                                : a("Short.")));
            break;
        }
    }
    return out;
}

void appendHtml(QTextEdit* view, const QString& html)
{
    QTextCursor c(view->document());
    c.movePosition(QTextCursor::End);
    if (!view->document()->isEmpty())
        c.insertBlock();
    c.insertHtml(html);
}

void appendLines(QTextEdit* view, const QStringList& lines)
{
    for (int from = 0; from < lines.size(); from += 200) // a few hundred lines per insertion: fast, TeamSpeak's formats
        appendHtml(view, lines.mid(from, 200).join(QString()));
}

// position:length:colour:tagged for every fragment (the prototype's formatsOf).
// pictureColors false: a picture's colour (never drawn, never recoloured) isn't compared (lines TeamSpeak inserted while dark
// mode was on keep the dark one on their pictures).
QString formatsOf(QTextDocument* doc, bool pictureColors = true)
{
    QString out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            out += QString::fromLatin1("%1:%2:%3:%4:%5;")
                       .arg(it.fragment().position())
                       .arg(it.fragment().length())
                       .arg(!pictureColors && f.isImageFormat() ? a("picture")
                            : f.hasProperty(QTextFormat::ForegroundBrush) ? f.foreground().color().name(QColor::HexArgb)
                                                                          : a("-"))
                       .arg(f.hasProperty(darkchat::kOriginal) ? 1 : 0)
                       .arg(f.properties().size());
        }
    }
    return out;
}

QColor colorAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position + 1);
    return c.charFormat().foreground().color();
}

// The first fragment holding text (in block order).
int findText(QTextDocument* doc, const QString& text, int from = 0)
{
    return doc->find(text, from).selectionStart();
}

// Every property of a format, the formats kept inside it too (review fix: what a module would give back).
QString describe(const QTextFormat& f)
{
    const auto valueOf = [](const QVariant& v) {
        if (v.userType() == QMetaType::QTextFormat)
            return QLatin1Char('{') + describe(qvariant_cast<QTextFormat>(v)) + QLatin1Char('}');
        if (v.userType() == QMetaType::QBrush)
            return qvariant_cast<QBrush>(v).color().name(QColor::HexArgb);
        if (v.userType() == QMetaType::QColor)
            return qvariant_cast<QColor>(v).name(QColor::HexArgb);
        return v.toString();
    };
    QStringList               out;
    const QMap<int, QVariant> props = f.properties();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        QString value;
        if (it.value().userType() == QMetaType::QVariantList) {
            QStringList items;
            for (const QVariant& item : it.value().toList())
                items << valueOf(item);
            value = QLatin1Char('[') + items.join(QLatin1Char(',')) + QLatin1Char(']');
        } else {
            value = valueOf(it.value());
        }
        out << QString::number(it.key(), 16) + QLatin1Char('=') + value;
    }
    return out.join(QLatin1Char(';'));
}

QString deepFormatsOf(QTextDocument* doc)
{
    QString out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it)
            out += QString::fromLatin1("%1:%2:%3\n").arg(it.fragment().position()).arg(it.fragment().length()).arg(describe(it.fragment().charFormat()));
    }
    return out;
}

// The first fragment where two formatsOf() strings differ, with the ones around it ("" if they don't differ).
QString firstDiff(const QString& x, const QString& y)
{
    const QStringList xs = x.split(QLatin1Char(';')), ys = y.split(QLatin1Char(';'));
    for (int i = 0; i < qMax(xs.size(), ys.size()); ++i) {
        if (xs.value(i) != ys.value(i))
            return QString::fromLatin1("fragment %1: %2 vs %3, around: %4").arg(i).arg(xs.value(i), ys.value(i), xs.mid(qMax(0, i - 3), 7).join(QLatin1Char(';')));
    }
    return QString();
}

// 2.7.2 (D6): where kOriginal is in a format or in a format kept inside it (as darkchat looks: any QTextFormat or list of
// them in a property), as "where" strings.
void leftoversIn(const QTextFormat& f, const QString& where, int depth, QStringList* out)
{
    if (f.hasProperty(darkchat::kOriginal))
        out->append(where);
    if (depth >= 6)
        return;
    const QMap<int, QVariant> props = f.properties();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        const QString inner = where + QLatin1Char('/') + QString::number(it.key(), 16);
        if (it.value().userType() == QMetaType::QTextFormat) {
            leftoversIn(qvariant_cast<QTextFormat>(it.value()), inner, depth + 1, out);
        } else if (it.value().userType() == QMetaType::QVariantList) {
            const QVariantList list = it.value().toList();
            for (int i = 0; i < list.size(); ++i) {
                if (list.at(i).userType() == QMetaType::QTextFormat)
                    leftoversIn(qvariant_cast<QTextFormat>(list.at(i)), inner + QString::fromLatin1("[%1]").arg(i), depth + 1, out);
            }
        }
    }
}

// Every fragment's format, every block's character format (its separator's, block 0 too) and block format.
QStringList darkLeftovers(QTextDocument* doc)
{
    QStringList out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        leftoversIn(b.charFormat(), QString::fromLatin1("block %1 char format").arg(b.blockNumber()), 0, &out);
        leftoversIn(b.blockFormat(), QString::fromLatin1("block %1 format").arg(b.blockNumber()), 0, &out);
        for (auto it = b.begin(); !it.atEnd(); ++it)
            leftoversIn(it.fragment().charFormat(), QString::fromLatin1("fragment at %1 \"%2\"").arg(it.fragment().position()).arg(it.fragment().text().left(12)), 0, &out);
    }
    return out;
}

// 2.7.2 (D5): "text => every property" of each fragment from block fromBlock on (one per line); a fragment whose text is
// skip isn't listed.
QStringList fragmentsFrom(QTextDocument* doc, int fromBlock, const QString& skip = QString())
{
    QStringList out;
    for (QTextBlock b = doc->findBlockByNumber(fromBlock); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            if (!skip.isEmpty() && it.fragment().text() == skip)
                continue;
            out << it.fragment().text() + a(" => ") + describe(it.fragment().charFormat());
        }
    }
    return out;
}

QString firstListDiff(const QStringList& got, const QStringList& want)
{
    for (int i = 0; i < qMax(got.size(), want.size()); ++i) {
        if (got.value(i) != want.value(i))
            return QString::fromLatin1("item %1:\n  got  %2\n  want %3").arg(i).arg(got.value(i), want.value(i));
    }
    return QString();
}

// formatsOf() of blocks [from, from + count) only.
QString formatsOfBlocks(QTextDocument* doc, int from, int count)
{
    QString out;
    for (QTextBlock b = doc->findBlockByNumber(from); b.isValid() && b.blockNumber() < from + count; b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            out += QString::fromLatin1("%1:%2:%3;").arg(it.fragment().position()).arg(it.fragment().length()).arg(describe(f));
        }
    }
    return out;
}

// TeamSpeak's lines of every class whose text style dark mode's sheet sets (2.7.2, D5): a join with a name and a channel,
// the history marker, a server error, a host name in a status line, a message with a link, the server's welcome (bold in
// the community dark skin, not in dark mode's sheet).
QStringList styledLines()
{
    return {chatLine(a("21:30:05"), a("TextMessage_ClientJoined"),
                     user(a("Kai")) + a(" entered channel \"<a class=\"TextMessage_ChannelLink\" href=\"channelid://1\">Home</a>\"")),
            a("<p class=\"TextMessage_BeginHistory\">*** Chat begins</p>"),
            a("<p class=\"TextMessage_ServerError\">Error: insufficient client permissions</p>"),
            chatLine(a("21:31:00"), a("TextMessage_Normal"), a("Trying to connect to server on <span class=\"TextMessage_HostName\">example.org</span>")),
            chatLine(a("21:31:20"), a("TextMessage_Message Incoming Public"),
                     user(a("Lea")) + a(": <span class=\"TextMessage_Text\"><a href=\"https://example.com/x\">https://example.com/x</a></span>")),
            a("<p class=\"TextMessage_Welcome\">Welcome to the test server</p>")};
}

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position + 1);
    return c.charFormat();
}

// 2.7.3 (M9, M10): a client's info panel as TeamSpeak's template writes it (made-up values): a bottom-aligned first row, a
// label with space above, a list cell with a nested table of badges, and a second table with space below.
QString infoPanelHtml()
{
    return QString::fromUtf8("<p class=\"InfoFrame_Title\"><a class=\"TextMessage_UserLink\" href=\"client://3/z~Dex\">Dex</a> "
                             "<span class=\"InfoFrame_CustomNick\">(Dexter)</span></p>"
                             "<table class=\"InfoFrame_Table\"><tr><td class=\"Bottom Label\">Version:</td><td class=\"Bottom\">3.6.2 on Windows</td></tr>"
                             "<tr><td class=\"Label\">Online since:</td><td class=\"Highlight\">12 minutes</td></tr>"
                             "<tr><td class=\"Label Space-Top\">Server groups:</td><td class=\"Space-Top\">2</td></tr>"
                             "<tr><td class=\"List\"><table class=\"InfoFrame_Table\"><tr><td class=\"Badge\">Admin</td><td>Server Admin</td></tr>"
                             "<tr><td class=\"Badge\">Guest</td><td>Guest</td></tr></table></td><td>-</td></tr>"
                             "<tr><td class=\"Label\">Status:</td><td class=\"Important\">Away</td></tr>"
                             "<tr><td class=\"Label\">Talk power:</td><td class=\"Active\">granted</td></tr></table>"
                             "<table class=\"InfoFrame_Table Space-Bot\"><tr><td class=\"Label\">Description:</td><td>Plays on weekends</td></tr></table>");
}

// 2.7.3 (M9, M5): every frame, table, cell and block of doc with its formats (describe), a block's character format too.
QStringList structureOf(QTextDocument* doc)
{
    // (A format's object index is the document's own numbering: a document rendered twice counts on.)
    const auto whole = [](const QTextFormat& f) {
        QTextFormat g = f;
        g.clearProperty(QTextFormat::ObjectIndex);
        return describe(g);
    };
    QStringList                           out;
    std::function<void(QTextFrame*, int)> walk = [&](QTextFrame* frame, int depth) {
        const QString indent(depth * 2, QLatin1Char(' '));
        auto*         table = qobject_cast<QTextTable*>(frame);
        out << indent + (table ? a("table ") : a("frame ")) + whole(frame->frameFormat());
        if (table) {
            for (int r = 0; r < table->rows(); ++r) {
                for (int c = 0; c < table->columns(); ++c) {
                    const QTextTableCell cell = table->cellAt(r, c);
                    if (cell.row() == r && cell.column() == c)
                        out << indent + QString::fromLatin1(" cell %1,%2 ").arg(r).arg(c) + whole(cell.format());
                }
            }
        }
        for (auto it = frame->begin(); !it.atEnd(); ++it) {
            if (QTextFrame* child = it.currentFrame()) {
                walk(child, depth + 1);
            } else {
                const QTextBlock b = it.currentBlock();
                out << indent + QString::fromLatin1(" block \"%1\" ").arg(b.text().left(14)) + whole(b.blockFormat()) + a(" | char ") + whole(b.charFormat());
            }
        }
    };
    walk(doc->rootFrame(), 0);
    return out;
}

// doc drawn at width (a copy: the document itself keeps its layout).
QImage drawnAt(QTextDocument* doc, int width)
{
    std::unique_ptr<QTextDocument> copy(doc->clone());
    copy->setTextWidth(width);
    QImage image(width, qCeil(copy->size().height()) + 2, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    copy->drawContents(&painter);
    painter.end();
    return image;
}

int heightAt(QTextDocument* doc, int width)
{
    std::unique_ptr<QTextDocument> copy(doc->clone());
    copy->setTextWidth(width);
    return qCeil(copy->size().height());
}

QString imageDiff(const QImage& x, const QImage& y)
{
    if (x.size() != y.size())
        return QString::fromLatin1("%1x%2 vs %3x%4").arg(x.width()).arg(x.height()).arg(y.width()).arg(y.height());
    int n = 0;
    for (int row = 0; row < x.height(); ++row) {
        for (int col = 0; col < x.width(); ++col)
            n += x.pixel(col, row) != y.pixel(col, row) ? 1 : 0;
    }
    return n == 0 ? QString() : QString::fromLatin1("%1 px differ").arg(n);
}

// ---- 2.7.1: TS Media's own lines as TeamSpeak shows them --------------------------------------------------------------

void appendTo(QTextDocument* doc, const QString& html)
{
    QTextCursor c(doc);
    c.movePosition(QTextCursor::End);
    if (!doc->isEmpty())
        c.insertBlock();
    c.insertHtml(html);
}

// TeamSpeak's rendering of a plugin print (notes s0 (h), real chat dumps): one run in its plugin-line blue (#00008b) with
// the BBCode's formatting inside: [color], [b], [url]; "\[" and "\]" are brackets.
QString pluginLineHtml(const QString& bbcode)
{
    QString out = a("<span style=\"color:#00008b\">");
    QString text;
    const auto flush = [&out, &text] {
        out += text.toHtmlEscaped();
        text.clear();
    };
    for (int i = 0; i < bbcode.size();) {
        if (bbcode.at(i) == QLatin1Char('\\') && i + 1 < bbcode.size() && (bbcode.at(i + 1) == QLatin1Char('[') || bbcode.at(i + 1) == QLatin1Char(']'))) {
            text += bbcode.at(i + 1);
            i += 2;
            continue;
        }
        if (bbcode.at(i) == QLatin1Char('[')) {
            const int     close = bbcode.indexOf(QLatin1Char(']'), i);
            const QString tag   = bbcode.mid(i + 1, close - i - 1);
            flush();
            if (tag.startsWith(a("color=")))
                out += QString::fromLatin1("<span style=\"color:%1\">").arg(tag.mid(6));
            else if (tag == a("/color"))
                out += a("</span>");
            else if (tag == a("b"))
                out += a("<b>");
            else if (tag == a("/b"))
                out += a("</b>");
            else if (tag.startsWith(a("url=")))
                out += QString::fromLatin1("<a href=\"%1\">").arg(tag.mid(4));
            else if (tag == a("/url"))
                out += a("</a>");
            i = close + 1;
            continue;
        }
        text += bbcode.at(i++);
    }
    flush();
    return out + a("</span>");
}

// Every shape of TS Media's lines, printed by ts3api with the line colours in use (ts3::setLineColors) and read back from
// the fake TeamSpeak as the BBCode it got: the label, the warning label, a link, /tsmedia help's bold commands, the chat
// layout's bold notice, a tip.
QStringList printEveryShape()
{
    const int    from = fakets3::logLines().size();
    const uint64 sch  = fakets3::kConnection;
    ts3::printInfo(sch, a("Sent a.png to “[Bob]”."));
    ts3::printWarning(sch, a("Upload failed."));
    ts3::printInfoLink(sch, a("2.7.1 is out:"), a("https://example.com/notes"), a("release notes"));
    ts3::print(sch, a("[b]/tsmedia send[/b] — choose files to send to this chat"));
    ts3::print(sch, a("[b]TS Media chat:[/b] new chat layout. Settings > Chat switches back to TeamSpeak classic."));
    ts3::print(sch, a("Tip: drop files on the chat to send them."));
    QStringList       out;
    const QStringList lines = fakets3::logLines();
    for (int i = from; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(a("[chat] ")))
            out << lines.at(i).mid(7);
    }
    return out;
}

// TS Media's prints in the colours of chat for choice (what ChatIntegration gives ts3api), appended as TeamSpeak appends
// them, then two lines of TeamSpeak's own (a nickname; a join with a name and a channel). The number of the first block.
int appendOwnLines(QTextDocument* doc, const chatcolors::Colors& chat, const QString& choice, bool teamSpeakLines = true)
{
    ts3::setLineColors(chatcolors::lineColors(chat, choice));
    const int first = doc->isEmpty() ? 0 : doc->blockCount();
    for (const QString& bbcode : printEveryShape())
        appendTo(doc, pluginLineHtml(bbcode));
    if (teamSpeakLines) {
        appendTo(doc, chatLine(a("21:30:00"), a("TextMessage_Message Incoming Public"), user(a("Kai")) + a(": <span class=\"TextMessage_Text\">hello</span>")));
        appendTo(doc, chatLine(a("21:30:05"), a("TextMessage_ClientJoined"),
                               user(a("Nora")) + a(" entered channel \"<a class=\"TextMessage_ChannelLink\" href=\"channelid://1\">Home</a>\"")));
    }
    return first;
}

// Per block from fromBlock on: the runs of characters of one colour ("text=#rrggbb", "-": none), fragments of one colour
// merged (skins differ in weight); a fragment still carrying kOriginal is marked.
QString charColors(QTextDocument* doc, int fromBlock)
{
    QStringList blocks;
    for (QTextBlock b = doc->findBlockByNumber(fromBlock); b.isValid(); b = b.next()) {
        QStringList runs;
        QString     text, color;
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            const QString c = (f.hasProperty(QTextFormat::ForegroundBrush) ? f.foreground().color().name() : a("-")) + (f.hasProperty(darkchat::kOriginal) ? a("+kOriginal") : QString());
            if (c != color && !text.isEmpty()) {
                runs << text + QLatin1Char('=') + color;
                text.clear();
            }
            color = c;
            text += it.fragment().text();
        }
        if (!text.isEmpty())
            runs << text + QLatin1Char('=') + color;
        blocks << runs.join(QLatin1Char('|'));
    }
    return blocks.join(QLatin1Char('\n'));
}

// The first block where two charColors() strings differ ("" if none).
QString firstLineDiff(const QString& got, const QString& want)
{
    const QStringList g = got.split(QLatin1Char('\n')), w = want.split(QLatin1Char('\n'));
    for (int i = 0; i < qMax(g.size(), w.size()); ++i) {
        if (g.value(i) != w.value(i))
            return QString::fromLatin1("block +%1:\n  got  %2\n  want %3").arg(i).arg(g.value(i), w.value(i));
    }
    return QString();
}

// The chat under dark mode, as ChatIntegration's chatColorsOf gives it while dark mode is on (its accent too).
chatcolors::Colors darkChatColors()
{
    const darkpalette::Tokens& k = darkpalette::tokens();
    chatcolors::Colors         c;
    c.base        = k.window;
    c.text        = k.text;
    c.link        = k.link;
    c.dark        = true;
    c.themeAccent = k.accent;
    return c;
}

// A chat's colours as TS Media prints for it (ChatIntegration: the ChatTab's palette with its background sampled).
chatcolors::Colors skinColorsOf(QTextBrowser* chat)
{
    const QPalette p = chat->palette();
    return chatcolors::withSample(chatcolors::fromPalette(p), chatcolors::sampleBackground(chat), chatcolors::isTexture(p));
}

// What a chat of sheet shows for TS Media's prints made for chat (and TeamSpeak's two lines), as charColors().
QString ownLinesReference(const QString& sheet, const chatcolors::Colors& chat, const QString& choice, bool teamSpeakLines = true)
{
    QTextDocument ref;
    ref.setDefaultStyleSheet(sheet);
    appendOwnLines(&ref, chat, choice, teamSpeakLines);
    return charColors(&ref, 0);
}

const char* const kEveryChoice[] = {"", "blue", "teal", "green", "orange", "pink", "text"};

// The test skin's maps, as DarkMode builds them (no extras).
struct SkinMaps {
    darkchat::ColorMap forward;
    darkchat::ColorMap back;
};
SkinMaps skinMaps()
{
    const darkpalette::Tokens& t    = darkpalette::tokens();
    const auto                 from = darkchat::classLooks(QString::fromLatin1(kSkinChat), QColor(0x1c, 0x82, 0xcc));
    const auto                 to   = darkchat::classLooks(darkpalette::chatSheet(t), t.link);
    return {darkchat::buildMap(from, to, {}, t.window), darkchat::buildMap(to, from, {}, QColor())};
}

// TeamSpeak's lines as tschat writes them (review fix: the chat layout styles these): a channel line, messages with links,
// a status line with a name and a channel.
void fillLayoutChat(QTextDocument* doc, const tschat::Skin& s)
{
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc->addResource(QTextDocument::ImageResource, QUrl(QString::fromLatin1("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
    const QString kai  = QString::fromLatin1(tschat::kKaiUid);
    const QString nora = QString::fromLatin1(tschat::kNoraUid);
    tschat::append(doc, tschat::systemHtml(s, a("15:56:16"), a("You are now talking in channel: ") + tschat::channelHtml(s, a("Home"))));
    tschat::append(doc, tschat::messageHtml(s, a("16:00:17"), 7, kai, a("Kai"), tschat::linkHtml(s, a("https://example.com/live/first"))));
    tschat::append(doc, tschat::messageHtml(s, a("16:00:57"), 7, kai, a("Kai"), a("Anyone up for a round?")));
    tschat::append(doc, tschat::messageHtml(s, a("16:05:12"), 9, nora, a("Nora"), a("Sure: ") + tschat::linkHtml(s, a("https://example.org/map"))));
    tschat::append(doc, tschat::messageHtml(s, a("16:05:40"), 7, kai, a("Kai"), tschat::linkHtml(s, a("https://example.com/live/second"))));
    tschat::append(doc, tschat::statusHtml(s, a("16:06:40"), tschat::clientHtml(s, 9, nora, a("Nora")) + a(" switched to channel ") + tschat::channelHtml(s, a("Lobby"))));
}

// The chat layout's environment on the test skin's chat (light) or dark mode's.
layoutdoc::Env layoutEnv(bool dark)
{
    const darkpalette::Tokens& t = darkpalette::tokens();
    layoutdoc::Env             env;
    env.mode         = layoutart::Mode::Cozy;
    env.tokens       = layoutart::tokensFor(layoutart::Mode::Cozy, QApplication::font(), false);
    env.colors       = dark ? layoutart::colorsFor(true, t.window, t.text, t.link, t.accent)
                            : layoutart::colorsFor(false, QColor(Qt::white), QColor(Qt::black), QColor(0x1c, 0x82, 0xcc));
    env.dpr          = 1.0;
    env.contentWidth = 520;
    env.today        = QDate(2026, 10, 10);
    env.tag          = a("dark");
    return env;
}

// What TeamSpeak inserts under dark mode's document sheet (its classes' colours there), as tschat writes it.
tschat::Skin darkEraSkin()
{
    const darkpalette::Tokens& t = darkpalette::tokens();
    tschat::Skin               s = tschat::lightSkin();
    s.time                       = t.muted.name();
    s.nick                       = t.text.name();
    s.own                        = t.text.name();
    s.link                       = t.link.name();
    s.system                     = t.muted.name();
    return s;
}

} // namespace

// 2.7.2 (D3): TeamSpeak's chat tab widget is a class of its own (ChatTabWidget): dark mode names it to keep the chat
// column's width (a rule for every QTabWidget would reach Options too).
class DarkTestChatTabs : public QTabWidget
{
    Q_OBJECT
  public:
    using QTabWidget::QTabWidget;
};

namespace {

struct TsWindow {
    QMainWindow*  window  = nullptr;
    QTreeWidget*  tree    = nullptr;
    QTabWidget*   tabs    = nullptr;
    QTextBrowser* visible = nullptr; // tab 0
    QTextBrowser* hidden  = nullptr; // tab 1
    QTextEdit*    info    = nullptr;
    QTextEdit*    input   = nullptr;
};

TsWindow buildWindow(int visibleLines, int hiddenLines, bool wrapping)
{
    TsWindow t;
    t.window = new QMainWindow;
    t.window->setObjectName(a("MainWindow"));
    t.window->resize(900, 640);
    QMenu* tools = t.window->menuBar()->addMenu(a("&Tools"));
    tools->addAction(a("Options"));
    QToolBar* bar = t.window->addToolBar(a("Main"));
    bar->addAction(a("Connect"));
    auto* central = new QWidget;
    auto* v       = new QVBoxLayout(central);
    auto* split   = new QSplitter(Qt::Vertical);
    v->addWidget(split);
    auto* top = new QSplitter(Qt::Horizontal);
    t.tree    = new QTreeWidget;
    t.tree->setObjectName(a("ServerTreeView"));
    t.tree->setHeaderHidden(true);
    auto* server = new QTreeWidgetItem(t.tree, {a("Local test server")});
    for (const char* name : {"Lobby", "Gaming", "AFK"})
        new QTreeWidgetItem(server, {QString::fromLatin1(name)});
    server->setExpanded(true);
    top->addWidget(t.tree);
    t.info = new QTextEdit;
    t.info->setObjectName(a("InfoFrame"));
    t.info->setReadOnly(true);
    t.info->document()->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
    t.info->setHtml(a("<p class=\"InfoFrame_Title\"><a class=\"TextMessage_ServerLink\" href=\"x\">Local test server</a></p>"
                      "<table class=\"InfoFrame_Table\"><tr><td class=\"Label\">Clients:</td><td class=\"Highlight\">7 / 32</td></tr>"
                      "<tr><td class=\"Label\">Encryption:</td><td class=\"Active\">Enabled</td></tr></table>"));
    top->addWidget(t.info);
    split->addWidget(top);
    auto* chatWidget = new QWidget;
    chatWidget->setObjectName(a("MainWindowChatWidget"));
    auto* cv = new QVBoxLayout(chatWidget);
    t.tabs   = new DarkTestChatTabs;
    for (int i = 0; i < 2; ++i) {
        auto* b = new QTextBrowser;
        b->setObjectName(a("ChatTab"));
        b->setOpenLinks(false);
        b->document()->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        t.tabs->addTab(b, i == 0 ? a("Lobby") : a("Server"));
        (i == 0 ? t.visible : t.hidden) = b;
    }
    appendLines(t.visible, someLines(visibleLines, wrapping));
    appendLines(t.hidden, someLines(hiddenLines, wrapping));
    cv->addWidget(t.tabs, 1);
    t.input = new QTextEdit;
    t.input->setObjectName(a("ChatLineEdit"));
    t.input->setProperty("helpTextVisible", true);
    t.input->setFixedHeight(40);
    cv->addWidget(t.input);
    split->addWidget(chatWidget);
    t.window->setCentralWidget(central);
    t.window->statusBar()->addWidget(new QLabel(a("Connected")));
    return t;
}

// 2.7.3 (M6): a window with TeamSpeak's bars around the chat (its object names and order): a menu bar, a tool bar of icon
// buttons, the server tabs over the tree, the chat tabs below their pages with the input row (a tool button beside the
// input) under them, a status bar; the main splitter is the central widget.
struct TsBars {
    QMainWindow*  window    = nullptr;
    QMenuBar*     menu      = nullptr;
    QToolBar*     tools     = nullptr;
    QTabWidget*   servers   = nullptr;
    QWidget*      chatArea  = nullptr;
    QTabWidget*   tabs      = nullptr;
    QTextBrowser* visible   = nullptr; // tab 0
    QTextBrowser* hidden    = nullptr; // tab 1
    QToolButton*  emoticons = nullptr;
    QStatusBar*   status    = nullptr;
    QSplitter*    split     = nullptr; // review fix: the main splitter (the server area over the chat area)
};

QIcon squareIcon(int size, const QColor& color)
{
    QPixmap pixmap(size, size);
    pixmap.fill(color);
    return QIcon(pixmap);
}

// into: a main window that exists already, empty (TeamSpeak's at the plugin's load), built into (review fix). info: the
// server tab holds the channel tree and the info panel side by side in a splitter, as TeamSpeak's (review fix).
TsBars buildBarsWindow(int lines, QMainWindow* into = nullptr, bool info = false)
{
    TsBars t;
    t.window = into ? into : new QMainWindow;
    t.window->setObjectName(a("MainWindow"));
    t.window->resize(1000, 760);
    t.menu = t.window->menuBar();
    for (const char* name : {"Connections", "Bookmarks", "Self", "Permissions", "Tools", "Help"})
        t.menu->addMenu(QString::fromLatin1(name))->addAction(a("Item"));
    t.tools = t.window->addToolBar(a("Main"));
    t.tools->setIconSize(QSize(24, 24));
    for (int i = 0; i < 6; ++i) {
        auto* b = new QToolButton;
        b->setIcon(squareIcon(24, QColor::fromHsv(i * 50, 120, 200)));
        b->setAutoRaise(true);
        if (i == 0) {
            b->setPopupMode(QToolButton::MenuButtonPopup);
            auto* menu = new QMenu(b);
            menu->addAction(a("Connect"));
            b->setMenu(menu);
        }
        t.tools->addWidget(b);
        if (i == 2)
            t.tools->addSeparator();
    }
    auto* split      = new QSplitter(Qt::Vertical);
    t.split          = split;
    auto* serverArea = new QWidget;
    serverArea->setObjectName(a("MainWindowServerTabsWidget"));
    auto* sv = new QVBoxLayout(serverArea);
    sv->setContentsMargins(0, 0, 0, 0);
    t.servers = new QTabWidget;
    t.servers->setTabsClosable(true);
    auto* tree = new QTreeWidget;
    tree->setObjectName(a("ServerTreeView"));
    tree->setHeaderHidden(true);
    new QTreeWidgetItem(tree, {a("Local test server")});
    if (info) {
        auto* side  = new QSplitter(Qt::Horizontal);
        auto* panel = new QTextEdit;
        panel->setObjectName(a("InfoFrame"));
        panel->setReadOnly(true);
        panel->setHtml(a("<p>Local test server</p><p>Clients: 7 / 32</p>"));
        side->addWidget(tree);
        side->addWidget(panel);
        t.servers->addTab(side, squareIcon(16, QColor(0x40, 0x80, 0xc0)), a("Local test server"));
    } else {
        t.servers->addTab(tree, squareIcon(16, QColor(0x40, 0x80, 0xc0)), a("Local test server"));
    }
    // Live test fix (2.7.3): TeamSpeak's server tabs are taller than a plain tab widget's (here: larger icons).
    const int big = qRound(t.servers->fontMetrics().height() * 1.6);
    t.servers->setIconSize(QSize(big, big));
    sv->addWidget(t.servers);
    split->addWidget(serverArea);
    t.chatArea = new QWidget;
    t.chatArea->setObjectName(a("MainWindowChatWidget"));
    auto* cv = new QVBoxLayout(t.chatArea);
    cv->setContentsMargins(0, 0, 0, 0);
    t.tabs = new DarkTestChatTabs;
    t.tabs->setTabPosition(QTabWidget::South);
    t.tabs->setTabsClosable(true);
    for (int i = 0; i < 2; ++i) {
        auto* b = new QTextBrowser;
        b->setObjectName(a("ChatTab"));
        b->setOpenLinks(false);
        b->document()->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        t.tabs->addTab(b, squareIcon(16, QColor(0x60, 0x90, 0xd0)), i == 0 ? a("Lobby") : a("Server"));
        (i == 0 ? t.visible : t.hidden) = b;
        appendLines(b, someLines(lines, true));
    }
    cv->addWidget(t.tabs, 1);
    auto* row = new QHBoxLayout;
    cv->addLayout(row);
    auto* input = new QTextEdit;
    input->setObjectName(a("ChatLineEdit"));
    input->setProperty("helpTextVisible", true);
    input->setFixedHeight(input->fontMetrics().lineSpacing() * 2); // TeamSpeak sizes it to its text
    row->addWidget(input, 1);
    t.emoticons = new QToolButton;
    t.emoticons->setAutoRaise(true);
    t.emoticons->setIconSize(QSize(16, 16));
    t.emoticons->setIcon(squareIcon(16, QColor(0xe0, 0xc0, 0x40)));
    row->addWidget(t.emoticons);
    split->addWidget(t.chatArea);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    t.window->setCentralWidget(split);
    t.status = t.window->statusBar();
    t.status->addWidget(new QLabel(a("Connected")));
    // Live test fix (2.7.3): and TeamSpeak's status bar holds fixed-size icons taller than its text (its sync, error and
    // ping icons: 16 px at 100 %, 32 at 200 %), which make it taller than a status bar with text alone.
    auto*     icon = new QWidget;
    const int side = t.status->fontMetrics().height() + qMax(3, t.status->fontMetrics().height() / 5);
    icon->setFixedSize(side, side);
    t.status->addPermanentWidget(icon);
    return t;
}

// The heights around the chat and the chat's viewport (what dark mode must keep), as text for one comparison.
QString barGeometry(const TsBars& t)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    int button = 0;
    for (QToolButton* b : t.tools->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (b->isVisible())
            button = qMax(button, b->height());
    }
    auto* chat = qobject_cast<QTextBrowser*>(t.tabs->currentWidget());
    return QString::fromLatin1("menu %1 (item %2), tool bar %3 (buttons %4), server tabs %5, chat tabs %6, input row %7 (button %8), status %9, "
                               "chat at y %10, viewport %11x%12")
        .arg(t.menu->height())
        .arg(t.menu->actionGeometry(t.menu->actions().first()).height())
        .arg(t.tools->height())
        .arg(button)
        .arg(t.servers->tabBar()->height())
        .arg(t.tabs->tabBar()->height())
        .arg(t.chatArea->height() - t.tabs->height())
        .arg(t.emoticons->height())
        .arg(t.status->height())
        .arg(chat->mapTo(t.window, QPoint(0, 0)).y())
        .arg(chat->viewport()->width())
        .arg(chat->viewport()->height());
}

// A dialog with every control class of design_dark 2.9.
QDialog* buildDialog()
{
    auto* d = new QDialog;
    d->setObjectName(a("OptionsDialog"));
    auto* l = new QVBoxLayout(d);
    auto* g = new QGroupBox(a("Style"));
    auto* f = new QVBoxLayout(g);
    auto* combo = new QComboBox;
    combo->addItems({a("One"), a("Two")});
    f->addWidget(combo);
    auto* check = new QCheckBox(a("Check"));
    check->setChecked(true);
    f->addWidget(check);
    f->addWidget(new QRadioButton(a("Radio")));
    f->addWidget(new QSpinBox);
    f->addWidget(new QDoubleSpinBox);
    f->addWidget(new QLineEdit(a("text")));
    auto* slider = new QSlider(Qt::Horizontal);
    f->addWidget(slider);
    auto* progress = new QProgressBar;
    progress->setValue(40);
    f->addWidget(progress);
    auto* list = new QListWidget;
    list->addItems({a("a"), a("b")});
    f->addWidget(list);
    auto* table = new QTableWidget(2, 2);
    table->setObjectName(a("clientsTableView"));
    f->addWidget(table);
    auto* tool = new QToolButton;
    tool->setText(a("Tool"));
    f->addWidget(tool);
    l->addWidget(g);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setDefault(true);
    l->addWidget(box);
    return d;
}

void pump(int ms = 0)
{
    QElapsedTimer t;
    t.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    } while (t.elapsed() < ms);
}

// Puts the test skin in place like TeamSpeak's start does: the sheet, the skin header's Link in the palette.
void installSkin()
{
    qApp->setStyleSheet(QString::fromLatin1(kSkinSheet));
    QPalette p = QApplication::palette();
    p.setColor(QPalette::Link, QColor(0x1c, 0x82, 0xcc));
    QApplication::setPalette(p);
}

bool samePalette(const QPalette& x, const QPalette& y)
{
    return x == y && x.resolve() == y.resolve();
}

QString paletteDiff(const QPalette& x, const QPalette& y)
{
    QStringList out;
    for (int g = 0; g < QPalette::NColorGroups; ++g) {
        for (int r = 0; r < QPalette::NColorRoles; ++r) {
            const auto group = static_cast<QPalette::ColorGroup>(g);
            const auto role  = static_cast<QPalette::ColorRole>(r);
            if (x.brush(group, role) != y.brush(group, role))
                out << QString::fromLatin1("g%1r%2 %3/%4").arg(g).arg(r).arg(x.color(group, role).name(), y.color(group, role).name());
        }
    }
    return out.join(a(", ")) + QString::fromLatin1(" resolve %1/%2").arg(x.resolve()).arg(y.resolve());
}

// The window once it stopped changing (a deferred layout or scroll still to come would differ from a later grab).
QImage stableGrab(QWidget* window)
{
    QImage last = window->grab().toImage();
    for (int i = 0; i < 20; ++i) {
        pump(30);
        const QImage now = window->grab().toImage();
        if (now == last)
            return now;
        last = now;
    }
    return last;
}

// QObject::receivers is protected: the number of connections to a document's contentsChange.
class Receivers : public QTextDocument
{
  public:
    static int of(QTextDocument* doc) { return static_cast<Receivers*>(doc)->receivers(SIGNAL(contentsChange(int, int, int))); }
};

// The fake DwmSetWindowAttribute (T-13).
struct DwmCall {
    quintptr hwnd;
    unsigned attribute;
    quint32  value;
};
QVector<DwmCall> g_dwm;
bool             g_dwmRefuse20 = false;

long fakeSet(void* hwnd, unsigned attribute, const void* value, unsigned size)
{
    quint32 v = 0;
    memcpy(&v, value, qMin<unsigned>(size, sizeof(v)));
    g_dwm.append({reinterpret_cast<quintptr>(hwnd), attribute, v});
    return attribute == 20 && g_dwmRefuse20 ? long(0x80070057) : 0; // E_INVALIDARG / S_OK
}

// A dialog that notes how many DWM calls had happened when its showEvent came.
class ShowProbe : public QDialog
{
  public:
    int callsAtShow = -1;

    void recreate() // a new native window (WinIdChange), as Qt does when a window is created again
    {
        destroy();
        create();
    }

  protected:
    void showEvent(QShowEvent* e) override
    {
        callsAtShow = g_dwm.size();
        QDialog::showEvent(e);
    }
};

// Counts the paints of a viewport and checks a condition at the first one.
class PaintProbe : public QObject
{
  public:
    std::function<bool()> check;
    int                   paints   = 0;
    bool                  okAtFirst = false;

    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Paint) {
            if (paints == 0 && check)
                okAtFirst = check();
            ++paints;
        }
        return false;
    }
};

QStringList g_messages;
void        collectMessages(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    if (type != QtDebugMsg)
        g_messages << message;
}

struct Pair {
    const char* name;
    QColor      fg;
    QColor      bg;
    double      minimum;
};

} // namespace

class TestDarkMode : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    TS3Functions                   m_savedFuncs;
    QString                        m_savedSheet;
    QPalette                       m_savedPalette;
    Settings                       m_savedSettings;
    QList<QPointer<QWidget>>       m_windows;

    std::unique_ptr<DarkMode> makeDark()
    {
        auto dark = std::make_unique<DarkMode>();
        dark->setGlyphBase(m_dir->path());
        return dark;
    }

    TsWindow shownWindow(int visibleLines, int hiddenLines, bool wrapping = true)
    {
        TsWindow t = buildWindow(visibleLines, hiddenLines, wrapping);
        m_windows << t.window;
        t.window->show();
        QApplication::setActiveWindow(t.window);
        t.tree->setFocus();
        if (!QTest::qWaitForWindowExposed(t.window))
            qWarning("the test window wasn't exposed");
        pump(50);
        return t;
    }

    // splitState: the main splitter's saved state, put back before the window shows (live test fix: as TeamSpeak puts back
    // the chat's height it saved).
    TsBars shownBarsWindow(int lines, QMainWindow* into = nullptr, bool info = false, const QByteArray& splitState = QByteArray())
    {
        TsBars t = buildBarsWindow(lines, into, info);
        if (!splitState.isEmpty())
            t.split->restoreState(splitState);
        m_windows << t.window;
        t.window->show();
        QApplication::setActiveWindow(t.window);
        if (!QTest::qWaitForWindowExposed(t.window))
            qWarning("the test window wasn't exposed");
        pump(50);
        t.tabs->setCurrentIndex(1); // the hidden chat laid out once, as TeamSpeak's tabs are after a while
        pump(30);
        t.tabs->setCurrentIndex(0);
        pump(30);
        return t;
    }

    // 2.7.2 (D7): the Settings window as plugin.cpp builds it (every section), shown (2.7.3: on the page first).
    SettingsDialog* settingsWindow(SettingsDialog::Tab first = SettingsDialog::Tab::Chat)
    {
        auto* dialog = new SettingsDialog(nullptr);
        m_windows << dialog;
        dialog->addSection(SettingsDialog::Tab::Sending, new ComposeSettingsSection(dialog));
        dialog->addSection(SettingsDialog::Tab::ReceivingPlayback, new SpoilerSection(dialog));
        dialog->addSection(SettingsDialog::Tab::Privacy, new PrivacySection, true);
        dialog->addSection(SettingsDialog::Tab::LinkPreviews, new LinkPreviewsSection);
        dialog->addSection(SettingsDialog::Tab::Sending, new CompressSettingsSection(dialog, nullptr));
        dialog->addSection(SettingsDialog::Tab::EmojiGifs, new EmojiSection(dialog));
        dialog->addSection(SettingsDialog::Tab::Chat, new ChatLayoutSection(dialog), true);
        dialog->addSection(SettingsDialog::Tab::Chat, new AppearanceSection(dialog, [](bool) {}), true);
        dialog->addSection(SettingsDialog::Tab::Chat, new ChatTextSection(dialog, ChatTextHooks()));
        dialog->showTab(first);
        dialog->show();
        if (!QTest::qWaitForWindowExposed(dialog))
            qWarning("the Settings window wasn't exposed");
        pump(50);
        return dialog;
    }
    // 2.7.3 (M7): files in TeamSpeak's styles folder (the config folder's here), written as given.
    bool writeStyles(const QVector<QPair<QString, QString>>& files)
    {
        if (!QDir().mkpath(m_dir->path() + a("/styles")))
            return false;
        for (const auto& file : files) {
            QFile f(m_dir->path() + a("/styles/") + file.first);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(file.second.toUtf8()) < 0)
                return false;
        }
        return true;
    }

    // Where the reader is in b, relative to the place captured before: the offset now minus the offset then (px).
    static qreal drift(QTextBrowser* b, const chatscroll::ReaderPlace& before)
    {
        if (before.bottom)
            return chatscroll::atBottom(b) ? 0 : 999;
        const qreal y = chatscroll::readerY(b->document(), before);
        if (y < 0)
            return 999;
        return (b->verticalScrollBar()->value() - y) - before.offset;
    }

  private slots:
    void init()
    {
        m_savedSheet    = qApp->styleSheet();
        m_savedPalette  = QApplication::palette();
        m_savedSettings = Settings::instance();
        m_savedFuncs    = ts3::funcs;
        m_dir           = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        fakets3::install(m_dir->path()); // the Settings window asks TeamSpeak (servers, the config folder)
        Settings::instance() = Settings();
        darktitlebar::setSetAttributeForTests(nullptr);
        g_dwm.clear();
        g_dwmRefuse20 = false;
    }

    void cleanup()
    {
        for (const QPointer<QWidget>& w : qAsConst(m_windows))
            delete w.data();
        m_windows.clear();
        QToolTip::hideText();
        qApp->setStyleSheet(m_savedSheet);
        QApplication::setPalette(m_savedPalette);
        Settings::instance() = m_savedSettings;
        ts3::funcs           = m_savedFuncs;
        darktitlebar::setSetAttributeForTests(nullptr);
        m_dir.reset();
        QVERIFY(!DarkMode::isActive());
    }

    // ---- T-1: every text pair >= 4.5:1, every boundary, indicator and focus >= 3:1 ----------------------------------
    void tokensContrast()
    {
        const darkpalette::Tokens& t   = darkpalette::tokens();
        const QColor selWindow         = ui::flatten(t.selection, t.window);
        const QColor selPanel          = ui::flatten(t.selection, t.panel);
        const QColor selInactiveWindow = ui::flatten(t.selectionInactive, t.window);
        const Pair   pairs[]           = {
            {"text/panel", t.text, t.panel, 4.5},          {"text/window", t.text, t.window, 4.5},     {"text/raised", t.text, t.raised, 4.5},
            {"text/field", t.text, t.field, 4.5},          {"text/hover", t.text, t.hover, 4.5},       {"text/menuHover", t.text, t.menuHover, 4.5},
            {"text/selection", t.text, selWindow, 4.5},    {"text/selection panel", t.text, selPanel, 4.5},
            {"text/selection inactive", t.text, selInactiveWindow, 4.5},
            {"text/raisedHover", t.text, t.raisedHover, 4.5}, {"text/accentTint", t.text, t.accentTint, 4.5}, {"text/accentDeep", t.text, t.accentDeep, 4.5},
            {"secondary/panel", t.secondary, t.panel, 4.5}, {"secondary/window", t.secondary, t.window, 4.5},
            {"secondary/raised", t.secondary, t.raised, 4.5}, {"secondary/field", t.secondary, t.field, 4.5},
            {"secondary/hover", t.secondary, t.hover, 4.5}, {"secondary/menuHover", t.secondary, t.menuHover, 4.5},
            {"muted/panel", t.muted, t.panel, 4.5},        {"muted/window", t.muted, t.window, 4.5},  {"muted/raised", t.muted, t.raised, 4.5},
            {"muted/field", t.muted, t.field, 4.5},        {"muted/hover", t.muted, t.hover, 4.5},
            {"link/panel", t.link, t.panel, 4.5},          {"link/window", t.link, t.window, 4.5},    {"link/raised", t.link, t.raised, 4.5},
            {"link/hover", t.link, t.hover, 4.5},          {"link/selection", t.link, selWindow, 4.5},
            {"accent text/panel", t.accent, t.panel, 4.5}, {"accent text/window", t.accent, t.window, 4.5},
            {"success/panel", t.success, t.panel, 4.5},    {"success/window", t.success, t.window, 4.5}, {"success/raised", t.success, t.raised, 4.5},
            {"warning/panel", t.warning, t.panel, 4.5},    {"warning/window", t.warning, t.window, 4.5}, {"warning/raised", t.warning, t.raised, 4.5},
            {"error/panel", t.error, t.panel, 4.5},        {"error/window", t.error, t.window, 4.5},  {"error/raised", t.error, t.raised, 4.5},
            {"onAccent/accent", t.onAccent, t.accent, 4.5}, {"onAccent/accentHover", t.onAccent, t.accentHover, 4.5},
            {"onAccent/accentPressed", t.onAccent, t.accentPressed, 4.5},
            // boundaries, indicators, focus; disabled text (its own floor)
            {"control/panel", t.control, t.panel, 3},      {"control/window", t.control, t.window, 3}, {"control/raised", t.control, t.raised, 3},
            {"control/field", t.control, t.field, 3},      {"controlHover/raised", t.controlHover, t.raised, 3},
            {"accent/panel", t.accent, t.panel, 3},        {"accent/window", t.accent, t.window, 3},  {"accent/raised", t.accent, t.raised, 3},
            {"accent/field", t.accent, t.field, 3},        {"focus ring text/raised", t.text, t.raised, 3},
            {"thumb/panel", t.thumb, t.panel, 3},          {"thumb/window", t.thumb, t.window, 3},    {"thumb/raised", t.thumb, t.raised, 3},
            {"thumbHover/raised", t.thumbHover, t.raised, 3},
            {"disabled/panel", t.disabled, t.panel, 3},    {"disabled/window", t.disabled, t.window, 3}, {"disabled/raised", t.disabled, t.raised, 3},
            {"disabled/field", t.disabled, t.field, 3},    {"disabled/disabledFill", t.disabled, t.disabledFill, 3},
            {"onAccentOff/accentOff", t.onAccentOff, t.accentOff, 3},
        };
        for (const Pair& p : pairs)
            QVERIFY2(ui::contrastRatio(p.fg, p.bg) >= p.minimum, qPrintable(QString::fromLatin1("%1: %2").arg(QString::fromLatin1(p.name)).arg(ui::contrastRatio(p.fg, p.bg))));
        // The Settings window's tokens: muted text on card, window and sidebar; disabled, control, accent and focus 3:1.
        const settingstheme::Tokens s = darkpalette::settingsTokens(1.0);
        QVERIFY(s.dark);
        for (const QColor& bg : {s.card, s.window, s.sidebar})
            QVERIFY(ui::contrastRatio(s.muted, bg) >= 4.5);
        for (const QColor& fg : {s.disabled, s.control, s.accent, s.focus})
            QVERIFY(ui::contrastRatio(fg, s.card) >= 3.0);
        QVERIFY(ui::contrastRatio(s.onAccent, s.accent) >= 4.5);
    }

    // ---- T-2: the sheets as built --------------------------------------------------------------------------------------
    void sheetBuild()
    {
        const darkpalette::Tokens& t = darkpalette::tokens();
        const QString glyphs = a("C:/Users/someone/AppData/Roaming/TS3Client/plugins/tsmedia/darkmode/200");
        for (const qreal k : {1.0, 1.5, 2.0}) {
            QStringList   unknown;
            const QString sheet = darkpalette::appSheet(t, k, darkpalette::Metrics{1, 17}, glyphs, &unknown);
            QVERIFY2(unknown.isEmpty(), qPrintable(unknown.join(a(","))));
            QVERIFY(!sheet.contains(QLatin1Char('@')));
            QVERIFY(!sheet.contains(QRegularExpression(a("\\{-?[0-9.]+\\}"))));
            QVERIFY(!sheet.contains(QRegularExpression(a("(^|[\\s;{])font(-[a-z]+)?\\s*:"))));
            QVERIFY2(sheet.size() <= 26 * 1024, qPrintable(QString::number(sheet.size())));
            QVERIFY(sheet.contains(a("url(\"") + glyphs + a("/cb-on.png\")")));
            QCOMPARE(sheet.count(QLatin1Char('{')), sheet.count(QLatin1Char('}')));
            // The chat keeps the skin's frame and scroll bar width.
            QVERIFY(sheet.contains(a("QTextEdit#ChatTab { background: #262624; color: #f5f4ee; border: 1px solid #262624; }")));
            QVERIFY(sheet.contains(a("QTextEdit#ChatTab QScrollBar:vertical { width: 17px; }")));
            QVERIFY(sheet.contains(a("background: rgba(217, 119, 87, 77);")));
            QVERIFY(sheet.contains(a("QSlider::handle:hover { background: #ffffff; }")));
        }
        const QString at15 = darkpalette::appSheet(t, 1.5, darkpalette::Metrics{2, 21}, glyphs);
        QVERIFY(at15.contains(a("QMainWindow::separator { background: #1f1e1d; width: 9px; height: 9px; }"))); // {6} at 1.5
        QVERIFY(at15.contains(QString::fromLatin1("QSlider::handle:horizontal { margin: %1px 0px; }").arg(darkpalette::px(-7, 1.5)))); // {-7}
        QVERIFY(at15.contains(a("border: 2px solid #262624;")));                                                   // the skin's frame
        QVERIFY(at15.contains(a("QTextEdit#ChatTab QScrollBar::handle:vertical { margin: 3px 6px; }")));        // (21 - 9) / 2
        // Without glyphs no picture rule is left (the native indicators show); the rest is the same.
        const QString bare = darkpalette::appSheet(t, 1.0, darkpalette::Metrics(), QString());
        QVERIFY(!bare.contains(a("url(")));
        QVERIFY(bare.contains(a("QPushButton:default { background: #d97757; color: #1a1918;")));
        // The chat document sheet.
        QStringList   unknown;
        const QString chat = darkpalette::chatSheet(t, &unknown);
        QVERIFY(unknown.isEmpty());
        QVERIFY(!chat.contains(QLatin1Char('@')));
        QVERIFY(chat.contains(a(".TextMessage_UserLink { color: #f5f4ee; font-weight: bold; text-decoration: none; }")));
        QVERIFY(chat.contains(a("a { color: #e8a383; }")));
        // An unknown token leaves its rule out (and is reported).
        QVERIFY(darkpalette::tokenByName(t, a("nope")).isValid() == false);
        QCOMPARE(darkpalette::cssColor(QColor(1, 2, 3, 4)), a("rgba(1, 2, 3, 4)"));
        QCOMPARE(darkpalette::scaleFactor(QFont()) >= 1.0, true);
    }

    // ---- T-3: no parse warnings, for the application sheet and the document sheet ---------------------------------------
    void sheetParses()
    {
        installSkin();
        TsWindow t      = shownWindow(20, 20);
        QDialog* dialog = buildDialog();
        m_windows << dialog;
        dialog->show();
        pump(30);
        g_messages.clear();
        const QtMessageHandler previous = qInstallMessageHandler(collectMessages);
        {
            auto dark = makeDark();
            dark->setEnabled(true);
            pump(50);
            for (QWidget* w : QApplication::allWidgets())
                w->ensurePolished();
            QTextDocument doc;
            doc.setDefaultStyleSheet(dark->chatSheetFor(t.visible->document()));
            doc.setHtml(someLines(10, false).join(QString()));
            dark->setEnabled(false);
        }
        qInstallMessageHandler(previous);
        for (const QString& m : qAsConst(g_messages)) {
            QVERIFY2(!m.contains(a("Could not parse"), Qt::CaseInsensitive), qPrintable(m));
            QVERIFY2(!m.contains(a("Unknown property"), Qt::CaseInsensitive), qPrintable(m));
        }
        Q_UNUSED(t);
    }

    // ---- T-4 / T-5: on and off give everything back exactly ------------------------------------------------------------
    void exactRestore()
    {
        installSkin();
        TsWindow t = shownWindow(40, 300);
        pump(50);
        const QString  sheet   = qApp->styleSheet();
        const QPalette palette = QApplication::palette();
        QHash<QWidget*, QPalette> widgetPalettes;
        for (QWidget* w : t.window->findChildren<QWidget*>())
            widgetPalettes.insert(w, w->palette());
        const QString visibleBefore = formatsOf(t.visible->document());
        const QString hiddenBefore  = formatsOf(t.hidden->document());
        const QString infoBefore    = formatsOf(t.info->document());
        const QString chatSheet     = t.visible->document()->defaultStyleSheet();
        const QImage  pixelsBefore  = stableGrab(t.window);

        auto dark = makeDark();
        dark->setEnabled(true);
        QVERIFY(DarkMode::isActive());
        QCOMPARE(qApp->styleSheet(), dark->appliedSheet());
        QVERIFY(qApp->styleSheet().constData() == dark->appliedSheet().constData());
        QCOMPARE(t.visible->document()->defaultStyleSheet(), dark->chatSheetFor(t.visible->document()));
        QVERIFY(darkchat::taggedCount(t.visible->document()) > 0);
        QVERIFY(darkchat::taggedCount(t.info->document()) > 0);
        QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(0x26, 0x26, 0x24));
        // LinkVisited is never set (design_dark 1.3).
        QCOMPARE(QApplication::palette().resolve() & (1u << QPalette::LinkVisited), palette.resolve() & (1u << QPalette::LinkVisited));
        pump(100); // the hidden chat by the slice timer
        QVERIFY(darkchat::taggedCount(t.hidden->document()) > 0);
        // Old lines read on the dark chat: the nickname is the theme's text, the time muted.
        const int bob = findText(t.visible->document(), a("Bob"));
        QCOMPARE(colorAt(t.visible->document(), bob), QColor(0xf5, 0xf4, 0xee));
        // The info panel's Highlight (palette(link) in the skin's sheet, read with the skin's palette): the dark link.
        QCOMPARE(colorAt(t.info->document(), findText(t.info->document(), a("7 / 32"))), darkpalette::tokens().link);

        dark->setEnabled(false);
        QVERIFY(!DarkMode::isActive());
        pump(100);
        QCOMPARE(qApp->styleSheet(), sheet);
        QVERIFY2(samePalette(QApplication::palette(), palette), qPrintable(paletteDiff(QApplication::palette(), palette)));
        for (auto it = widgetPalettes.cbegin(); it != widgetPalettes.cend(); ++it)
            QVERIFY2(samePalette(it.key()->palette(), it.value()), qPrintable(it.key()->metaObject()->className() + a(" ") + it.key()->objectName()));
        QCOMPARE(t.visible->document()->defaultStyleSheet(), chatSheet);
        QCOMPARE(t.hidden->document()->defaultStyleSheet(), chatSheet);
        QCOMPARE(t.info->document()->defaultStyleSheet(), chatSheet);
        QCOMPARE(formatsOf(t.visible->document()), visibleBefore);
        QCOMPARE(formatsOf(t.hidden->document()), hiddenBefore);
        QCOMPARE(formatsOf(t.info->document()), infoBefore);
        const QImage pixelsAfter = stableGrab(t.window);
        QCOMPARE(pixelsAfter.size(), pixelsBefore.size());
        int   differ = 0;
        QRect where;
        for (int y = 0; y < pixelsAfter.height(); ++y) {
            for (int x = 0; x < pixelsAfter.width(); ++x) {
                if (pixelsAfter.pixel(x, y) != pixelsBefore.pixel(x, y)) {
                    ++differ;
                    where |= QRect(x, y, 1, 1);
                }
            }
        }
        QWidget* at = differ ? t.window->childAt(where.center()) : nullptr;
        QVERIFY2(differ == 0, qPrintable(QString::fromLatin1("%1 px differ in %2,%3 %4x%5 (%6 %7)")
                                             .arg(differ)
                                             .arg(where.x())
                                             .arg(where.y())
                                             .arg(where.width())
                                             .arg(where.height())
                                             .arg(at ? QString::fromLatin1(at->metaObject()->className()) : QString(), at ? at->objectName() : QString())));
    }

    void removeOrder()
    {
        // The skin paints the chat with palette(base): white again after off (the palette is back before the sheet).
        installSkin();
        TsWindow t    = shownWindow(5, 0);
        const QRgb before = t.visible->grab().toImage().pixel(t.visible->width() / 2, t.visible->height() - 6);
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        const QRgb during = t.visible->grab().toImage().pixel(t.visible->width() / 2, t.visible->height() - 6);
        QCOMPARE(QColor(during), QColor(0x26, 0x26, 0x24));
        dark->setEnabled(false);
        pump(30);
        const QRgb after = t.visible->grab().toImage().pixel(t.visible->width() / 2, t.visible->height() - 6);
        QCOMPARE(QColor(after), QColor(before));
        QCOMPARE(QColor(after), QApplication::palette().color(QPalette::Base));
        // The palette never sets LinkVisited (a role outside the skin's mask that setPalette wouldn't give back).
        const QPalette base = QApplication::palette();
        QCOMPARE(darkpalette::palette(base).resolve() & (1u << QPalette::LinkVisited), base.resolve() & (1u << QPalette::LinkVisited));
    }

    // ---- T-6: another skin picked while dark mode is on ----------------------------------------------------------------
    void skinChangeWhileOn()
    {
        installSkin();
        TsWindow t    = shownWindow(30, 30);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        QCOMPARE(dark->stats().adoptions, 0); // our own apply adopts nothing
        // TeamSpeak's Options → Design → Apply: the sheet, the skin header's Link, the chats' document sheets.
        const QString other = QString::fromLatin1(kOtherSheet);
        qApp->setStyleSheet(other);
        QPalette p = QApplication::palette();
        p.setColor(QPalette::Link, QColor(0xaa, 0x00, 0xaa));
        QApplication::setPalette(p);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(QString::fromLatin1(kOtherChat));
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        QVERIFY(DarkMode::isActive());
        QCOMPARE(qApp->styleSheet(), dark->appliedSheet());
        QCOMPARE(dark->snapshotSheet(), other);
        QCOMPARE(t.visible->document()->defaultStyleSheet(), dark->chatSheetFor(t.visible->document()));
        QCOMPARE(dark->metrics().chatFrame, 2); // measured under the new skin
        // A line added while dark, then off: the other skin's colours.
        appendHtml(t.visible, chatLine(a("22:00:00"), a("TextMessage_ClientLeft"), user(a("Zed")) + a(" left your channel")));
        pump(30);
        dark->setEnabled(false);
        pump(30);
        QCOMPARE(qApp->styleSheet(), other);
        QCOMPARE(QApplication::palette().color(QPalette::Link), QColor(0xaa, 0x00, 0xaa));
        QCOMPARE(t.visible->document()->defaultStyleSheet(), QString::fromLatin1(kOtherChat));
        const int zed = findText(t.visible->document(), a("Zed"));
        QCOMPARE(colorAt(t.visible->document(), zed), QColor(0x5a, 0x1d, 0x00));
        const int left = findText(t.visible->document(), a("left your channel"), zed);
        QCOMPARE(colorAt(t.visible->document(), left), QColor(0x6b, 0x4a, 0x2f));
        // The old lines got back exactly what they had (TeamSpeak never recolours old lines on a skin change).
        const int bob = findText(t.visible->document(), a("Bob"));
        QCOMPARE(colorAt(t.visible->document(), bob), QColor(0x00, 0x2f, 0x5d));
    }

    // ---- T-7: our own apply adopts nothing; a fight over the sheet pauses dark mode --------------------------------------
    void loopGuard()
    {
        installSkin();
        TsWindow t    = shownWindow(5, 0);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        QCOMPARE(dark->stats().adoptions, 0);
        // TeamSpeak gives a chat its skin's sheet again (a new chat): taken over quietly, no repolish, not counted.
        t.visible->document()->setDefaultStyleSheet(QString::fromLatin1(kOtherChat));
        QEvent styleChange(QEvent::StyleChange);
        QCoreApplication::sendEvent(t.window, &styleChange);
        pump(30);
        QCOMPARE(t.visible->document()->defaultStyleSheet(), dark->chatSheetFor(t.visible->document()));
        QCOMPARE(dark->stats().adoptions, 0);
        QString last;
        for (int i = 0; i < 8; ++i) {
            last = QString::fromLatin1(kOtherSheet) + QString::fromLatin1("QLabel#n%1 { color: #000000; }\n").arg(i);
            qApp->setStyleSheet(last);
            pump(20);
        }
        QVERIFY(dark->stats().adoptions <= 5);
        QVERIFY(dark->paused());
        QVERIFY(!DarkMode::isActive());
        QVERIFY(dark->enabled());
        QVERIFY(!dark->hasEventFilter());
        const QString snapshot = dark->snapshotSheet();
        dark->setEnabled(false); // off still restores (the last skin it took over)
        QCOMPARE(qApp->styleSheet(), snapshot);
        QVERIFY(!dark->paused());
        Q_UNUSED(t);
    }

    // ---- T-8: the chat's recolouring both ways (darkchat) ----------------------------------------------------------------
    void recolor()
    {
        const darkpalette::Tokens& t     = darkpalette::tokens();
        const QString              light = QString::fromLatin1(kSkinChat);
        const QString              dark  = darkpalette::chatSheet(t);
        const auto from = darkchat::classLooks(light, QColor(0x1c, 0x82, 0xcc));
        const auto to   = darkchat::classLooks(dark, t.link);
        QVERIFY(from.size() >= 12);
        const darkchat::ColorMap forward = darkchat::buildMap(from, to, {}, t.window);
        const darkchat::ColorMap back    = darkchat::buildMap(to, from, {}, QColor());

        QTextDocument doc;
        doc.setDefaultStyleSheet(light);
        QTextBrowser view;
        view.setDocument(&doc);
        appendLines(&view, someLines(10, false));
        // Someone's [COLOR=#000000], an image, an object, a fragment of TS Media's layout.
        appendHtml(&view, chatLine(a("21:10:00"), a("TextMessage_Message"), user(a("Eve")) + a(": <span style=\"color:#000000\">black text</span>")));
        QTextCursor c(&doc);
        c.movePosition(QTextCursor::End);
        QTextImageFormat image;
        image.setName(a("data:x"));
        image.setForeground(QColor(Qt::black));
        c.insertImage(image);
        QTextCharFormat object;
        object.setObjectType(QTextFormat::UserObject + 7);
        object.setForeground(QColor(Qt::black));
        c.insertText(QString(QChar::ObjectReplacementCharacter), object);
        QTextCharFormat ours;
        ours.setForeground(QColor(0x00, 0x00, 0x8b));
        ours.setProperty(layoutformat::kKind, int(layoutformat::Head));
        c.insertText(a("ours"), ours);
        const QString before = formatsOf(&doc);

        const int changed = darkchat::recolor(&doc, forward, true);
        QVERIFY(changed > 10);
        const int bob = findText(&doc, a("Bob"));
        QCOMPARE(colorAt(&doc, bob), t.text);
        const int black = findText(&doc, a("black text"));
        QVERIFY(ui::contrastRatio(colorAt(&doc, black), t.window) >= 4.5);
        QVERIFY(darkchat::taggedCount(&doc) == changed);
        const int oursAt = findText(&doc, a("ours"));
        QCOMPARE(colorAt(&doc, oursAt), QColor(0x00, 0x00, 0x8b)); // untouched
        // A second pass changes nothing (tagged fragments are left).
        QCOMPARE(darkchat::recolor(&doc, forward, true), 0);
        // 2.7.2 (D6): a click on a recoloured nickname, then TeamSpeak's QTextEdit::append: the new block's separator (and the
        // view's own format) copy that fragment's format, kOriginal included.
        QTextCursor click(&doc);
        click.setPosition(bob + 1);
        view.setTextCursor(click);
        view.append(chatLine(a("21:15:00"), a("TextMessage_ClientJoined"), user(a("Finn")) + a(" entered your channel")));
        QVERIFY(!darkLeftovers(&doc).isEmpty());

        // Lines added while dark (the dark sheet), then back: the skin's class colours, exactly what it inserts.
        doc.setDefaultStyleSheet(dark);
        appendHtml(&view, chatLine(a("21:20:00"), a("TextMessage_ClientLeft"), user(a("Carol")) + a(" left your channel")));
        darkchat::recolor(&doc, back, false);
        doc.setDefaultStyleSheet(light);
        QTextDocument probe;
        probe.setDefaultStyleSheet(light);
        QTextBrowser probeView;
        probeView.setDocument(&probe);
        appendHtml(&probeView, chatLine(a("21:20:00"), a("TextMessage_ClientLeft"), user(a("Carol")) + a(" left your channel")));
        const int carol      = findText(&doc, a("Carol"));
        const int carolProbe = findText(&probe, a("Carol"));
        QCOMPARE(colorAt(&doc, carol), colorAt(&probe, carolProbe));
        QCOMPARE(colorAt(&doc, findText(&doc, a("left your"), carol)), colorAt(&probe, findText(&probe, a("left your"))));
        // The old part is exactly as before.
        QVERIFY(formatsOf(&doc).startsWith(before));
        QCOMPARE(darkchat::taggedCount(&doc), 0);
        // 2.7.2 (D6): nothing of the way back is left anywhere (the appended block's separator too).
        QVERIFY2(darkLeftovers(&doc).isEmpty(), qPrintable(darkLeftovers(&doc).join(a("; "))));
    }

    // classLooks: a link the sheet doesn't colour takes the palette's link colour given.
    void classLooksPlainLink()
    {
        const auto looks = darkchat::classLooks(a(".TextMessage_UserLink { color: #102030; }"), QColor(0x11, 0x22, 0x33));
        QVERIFY(!looks.isEmpty());
        QCOMPARE(looks.first().first, QByteArray("a"));
        QCOMPARE(QColor::fromRgba(looks.first().second.rgb), QColor(0x11, 0x22, 0x33));
        QVERIFY(looks.first().second.underline);
        const auto colored = darkchat::classLooks(a("a { color: #445566; }"), QColor(0x11, 0x22, 0x33));
        QCOMPARE(QColor::fromRgba(colored.first().second.rgb), QColor(0x44, 0x55, 0x66));
        // palette(link) in a skin's sheet is the colour of the palette given (the skin's, not the one in use now).
        QPalette skin;
        skin.setColor(QPalette::Link, QColor(0x12, 0x34, 0x56));
        skin.setColor(QPalette::WindowText, QColor(0x65, 0x43, 0x21));
        QCOMPARE(darkchat::resolvePaletteRefs(a(".Highlight { color: palette(link); } .x { color: Palette( window-text ); } .y { color: palette(nope); }"), skin),
                 a(".Highlight { color: #123456; } .x { color: #654321; } .y { color: palette(nope); }"));
        // TeamSpeak's template colours in a tool tip.
        const darkpalette::Tokens& t   = darkpalette::tokens();
        const darkchat::ColorMap   tip = darkchat::tipMap(t.link, t.error, t.raised);
        QCOMPARE(QColor::fromRgba(tip.exact.value(darkchat::Look{QColor(Qt::blue).rgba(), false, false})), t.link);
        QCOMPARE(QColor::fromRgba(tip.exact.value(darkchat::Look{QColor(Qt::red).rgba(), true, false})), t.error);
    }

    // ---- T-8d (2.7.1): a look knows whether it is a link: TeamSpeak's names, channels and info titles are links (its client
    // and templates put those classes on <a> only), TS Media's line label and text never are; the label is known by its text --
    void lookKnowsLinks()
    {
        const auto looks = darkchat::classLooks(QString::fromLatin1(kSkinChat), QColor(0x1c, 0x82, 0xcc));
        QVERIFY(looks.size() >= 12);
        const QList<QByteArray> links{"a", "UserLink", "ChannelLink", "ServerLink", "OfflineLink", "InfoTitleLink"};
        int                     linkCount = 0;
        for (const auto& look : looks) {
            QVERIFY2(look.second.anchor == links.contains(look.first), look.first.constData());
            linkCount += look.second.anchor ? 1 : 0;
        }
        QCOMPARE(linkCount, links.size());
        const auto dark = darkchat::classLooks(darkpalette::chatSheet(darkpalette::tokens()), darkpalette::tokens().link);
        for (const auto& look : dark)
            QVERIFY2(look.second.anchor == links.contains(look.first), look.first.constData());
        // The nickname's look under dark mode and TS Media's bold text in the chat text colour: the same but for the link.
        QTextCharFormat bold;
        bold.setForeground(darkpalette::tokens().text);
        bold.setFontWeight(QFont::Bold);
        QTextCharFormat nick = bold;
        nick.setAnchor(true);
        nick.setAnchorHref(a("client://7/x~Kai"));
        QVERIFY(!darkchat::lookOf(bold).anchor);
        QVERIFY(darkchat::lookOf(nick).anchor);
        QVERIFY(!(darkchat::lookOf(bold) == darkchat::lookOf(nick)));
        QVERIFY(darkchat::qHash(darkchat::lookOf(bold)) != darkchat::qHash(darkchat::lookOf(nick)));
        // Tool tips: TeamSpeak's template colours, in a link or not.
        const darkpalette::Tokens& t   = darkpalette::tokens();
        const darkchat::ColorMap   tip = darkchat::tipMap(t.link, t.error, t.raised);
        for (const bool anchor : {false, true}) {
            QCOMPARE(QColor::fromRgba(tip.exact.value(darkchat::Look{QColor(Qt::blue).rgba(), true, false, anchor})), t.link);
            QCOMPARE(QColor::fromRgba(tip.exact.value(darkchat::Look{QColor(Qt::red).rgba(), false, true, anchor})), t.error);
        }
        // The label: ts3api's two, exactly; not the chat layout's notice.
        QVERIFY(darkchat::isLineLabel(a("TS Media chat")));
        QVERIFY(darkchat::isLineLabel(a("TS Media chat ⚠")));
        QVERIFY(!darkchat::isLineLabel(a("TS Media chat:")));
        QVERIFY(!darkchat::isLineLabel(a("TS Media chat  ")));
        QVERIFY(!darkchat::isLineLabel(a("TS Media")));
        QVERIFY(!darkchat::isLineLabel(QString()));
        const QString api = sourceText(a("src/ts3api.cpp"));
        QVERIFY(api.contains(a("[b]TS Media chat[/b]")));
        QVERIFY(api.contains(a("[b]TS Media chat ⚠[/b]")));
        // Kept runs: a run is the label when the text before it is.
        const SkinMaps        maps = skinMaps();
        darkchat::ColorMap    back = maps.back;
        const QColor          own(0x12, 0x34, 0x56);
        back.label.insert(darkchat::lookOf(bold), own.rgba());
        QTextImageFormat prefix;
        prefix.setName(a("tsmedia-layout:sys"));
        prefix.setProperty(layoutformat::kKind, int(layoutformat::SystemPrefix));
        prefix.setProperty(layoutformat::kRuns, QVariantList{a("TS Media chat"), QVariant::fromValue<QTextFormat>(bold), a("TS Media chat:"),
                                                             QVariant::fromValue<QTextFormat>(bold)});
        QTextDocument doc;
        QTextCursor(&doc).insertImage(prefix);
        QCOMPARE(darkchat::recolor(&doc, back, false), 1);
        const QVariantList runs = formatAt(&doc, 0).property(layoutformat::kRuns).toList();
        QCOMPARE(qvariant_cast<QTextFormat>(runs.at(1)).foreground().color(), own);
        QVERIFY(qvariant_cast<QTextFormat>(runs.at(3)).foreground().color() != own);
    }

    // ---- T-8b (review fix): what TS Media's modules keep of TeamSpeak's text, both ways (darkchat) --------------------------
    void keptFormats()
    {
        const darkpalette::Tokens& t    = darkpalette::tokens();
        const SkinMaps             maps = skinMaps();
        QTextCharFormat            nick;
        nick.setForeground(QColor(0x00, 0x2f, 0x5d));
        nick.setFontWeight(QFont::Bold);
        nick.setAnchor(true);
        nick.setAnchorHref(a("client://7/x~Kai"));
        QTextCharFormat time;
        time.setForeground(QColor(0x33, 0x59, 0x7d));
        QTextCharFormat link;
        link.setForeground(QColor(0x1c, 0x82, 0xcc));
        link.setFontUnderline(true);
        link.setAnchor(true);
        link.setAnchorHref(a("https://example.com/"));
        QTextCharFormat black; // someone's [COLOR=#000000]
        black.setForeground(QColor(Qt::black));
        // An HD emoji in the nickname: its picture keeps the character's format.
        QTextImageFormat emoji;
        emoji.setName(a("emoji:1"));
        emoji.setProperty(emojiformat::kOriginalText, a("x"));
        emoji.setProperty(emojiformat::kOriginalFormat, QVariant::fromValue<QTextFormat>(nick));
        // The chat layout's head picture keeps the header's runs.
        QTextImageFormat head;
        head.setName(a("tsmedia-layout:head1"));
        head.setProperty(layoutformat::kKind, int(layoutformat::Head));
        head.setProperty(layoutformat::kRuns, QVariantList{a("<16:00:17>"), QVariant::fromValue<QTextFormat>(time), a(" "),
                                                           QVariant::fromValue<QTextFormat>(QTextCharFormat()), a("\"Kai"),
                                                           QVariant::fromValue<QTextFormat>(nick), a("x"), QVariant::fromValue<QTextFormat>(emoji)});
        // A link the layout only gave its underline colour (its colour stays TeamSpeak's) ...
        QTextCharFormat underlined = link;
        underlined.setUnderlineColor(QColor(0x1c, 0x82, 0xcc, 102));
        underlined.setProperty(layoutformat::kOrigChar, QVariant::fromValue<QTextFormat>(link));
        underlined.setProperty(layoutformat::kApplied, QVariantList{int(QTextFormat::TextUnderlineColor), QColor(0x1c, 0x82, 0xcc, 102)});
        // ... and a system row's name it recoloured (that colour is the layout's).
        const QColor    layoutName(0x06, 0x06, 0x07);
        QTextCharFormat recoloured = nick;
        recoloured.setForeground(layoutName);
        recoloured.setProperty(layoutformat::kOrigChar, QVariant::fromValue<QTextFormat>(nick));
        recoloured.setProperty(layoutformat::kApplied, QVariantList{int(QTextFormat::ForegroundBrush), QBrush(layoutName)});
        // A text spoiler's word keeps the hidden runs.
        QTextCharFormat word;
        word.setObjectType(textspoilerdoc::kObjectType);
        word.setProperty(textspoilerdoc::kKind, int(textspoilerdoc::Word));
        word.setProperty(textspoilerdoc::kRuns, QVariantList{a("secret"), QVariant::fromValue<QTextFormat>(black)});

        QTextDocument doc;
        QTextCursor   c(&doc);
        c.insertImage(head);
        c.insertText(a(": "), QTextCharFormat());
        c.insertText(a("map"), underlined);
        c.insertText(a(" "), QTextCharFormat());
        c.insertText(a("Nora"), recoloured);
        c.insertText(QString(QChar::ObjectReplacementCharacter), word);
        const QString before = deepFormatsOf(&doc);
        const quint64 serial = darkchat::keptSerial();

        QVERIFY(darkchat::recolor(&doc, maps.forward, true) >= 4);
        QVERIFY(darkchat::keptSerial() > serial); // the chat layout reads its statistics again
        // The header's runs: the dark colours, each with its way back.
        const QVariantList runs = formatAt(&doc, 0).property(layoutformat::kRuns).toList();
        QCOMPARE(runs.size(), 8);
        const QTextFormat runTime = qvariant_cast<QTextFormat>(runs.at(1));
        const QTextFormat runNick = qvariant_cast<QTextFormat>(runs.at(5));
        QCOMPARE(runTime.foreground().color(), t.muted);
        QCOMPARE(runTime.property(darkchat::kOriginal).toUInt(), QColor(0x33, 0x59, 0x7d).rgba());
        QCOMPARE(runNick.foreground().color(), t.text);
        QVERIFY(!qvariant_cast<QTextFormat>(runs.at(3)).hasProperty(darkchat::kOriginal)); // no colour: nothing to map
        const QTextFormat runEmoji = qvariant_cast<QTextFormat>(runs.at(7));
        QCOMPARE(qvariant_cast<QTextFormat>(runEmoji.property(emojiformat::kOriginalFormat)).foreground().color(), t.text);
        // The link with only the layout's underline colour: TeamSpeak's colour recoloured, also in what the layout gives back.
        const QTextCharFormat mapNow = formatAt(&doc, findText(&doc, a("map")));
        QCOMPARE(mapNow.foreground().color(), t.link);
        QVERIFY(mapNow.hasProperty(darkchat::kOriginal));
        QCOMPARE(qvariant_cast<QTextFormat>(mapNow.property(layoutformat::kOrigChar)).foreground().color(), t.link);
        // The name the layout recoloured keeps the layout's colour; TeamSpeak's under it is dark, and given back with its way back.
        const QTextCharFormat noraNow = formatAt(&doc, findText(&doc, a("Nora")));
        QCOMPARE(noraNow.foreground().color(), layoutName);
        QVERIFY(!noraNow.hasProperty(darkchat::kOriginal));
        const QTextCharFormat given = layoutdoc::originalCharFormat(noraNow);
        QCOMPARE(given.foreground().color(), t.text);
        QCOMPARE(given.property(darkchat::kOriginal).toUInt(), QColor(0x00, 0x2f, 0x5d).rgba());
        // The spoiler's hidden black text is readable once revealed.
        const QVariantList hidden = formatAt(&doc, doc.characterCount() - 2).property(textspoilerdoc::kRuns).toList();
        QCOMPARE(hidden.size(), 2);
        QVERIFY(ui::contrastRatio(qvariant_cast<QTextFormat>(hidden.at(1)).foreground().color(), t.window) >= 4.5);
        // A second pass changes nothing; the way back is exact, inside every kept format.
        QCOMPARE(darkchat::recolor(&doc, maps.forward, true), 0);
        darkchat::recolor(&doc, maps.back, false);
        QCOMPARE(deepFormatsOf(&doc), before);

        // Kept while dark mode was on (no way back stored): the skin's class colours, like TeamSpeak's own lines added then.
        QTextImageFormat darkHead = head;
        QTextCharFormat  darkNick = nick;
        darkNick.setForeground(t.text);
        QTextCharFormat darkTime = time;
        darkTime.setForeground(t.muted);
        darkHead.setProperty(layoutformat::kRuns, QVariantList{a("<16:00:17>"), QVariant::fromValue<QTextFormat>(darkTime), a("\"Kai"),
                                                               QVariant::fromValue<QTextFormat>(darkNick)});
        QTextDocument later;
        QTextCursor(&later).insertImage(darkHead);
        QVERIFY(darkchat::recolor(&later, maps.back, false) == 1);
        const QVariantList back = formatAt(&later, 0).property(layoutformat::kRuns).toList();
        QCOMPARE(qvariant_cast<QTextFormat>(back.at(1)).foreground().color(), QColor(0x33, 0x59, 0x7d));
        QCOMPARE(qvariant_cast<QTextFormat>(back.at(3)).foreground().color(), QColor(0x00, 0x2f, 0x5d));
    }

    // ---- 2.7.1 live test: an album's collapsed item link ----------------------------------------------------------------------
    // chatalbums folds the later files' links of an album into one WORD JOINER that keeps the link's format, its text and the
    // line breaks before it as strings. A Reload, an Enable or a start with dark mode on folds after the recolouring; the
    // marker then kept dark mode's link colour after off, and the album unfolded at the unload in that colour (salmon links
    // in the light chat).
    void albumCollapsedLink()
    {
        const darkpalette::Tokens& t    = darkpalette::tokens();
        const SkinMaps             maps = skinMaps();
        QTextCharFormat            link;
        link.setForeground(QColor(0x1c, 0x82, 0xcc));
        link.setFontUnderline(true);
        link.setAnchor(true);
        link.setAnchorHref(a("ts3file://127.0.0.1?port=9987&filename=sunset.png"));
        const auto fill = [&](QTextDocument* doc) {
            QTextCursor c(doc);
            c.insertText(a("big.jpg"), link); // the album's first link stays
            c.insertText(QString(QChar::LineSeparator), QTextCharFormat());
            c.insertText(a("sunset.png"), link);
        };
        // As chatalbums' collapseLink does: the link's format as the document has it then, with its text and line break.
        const auto collapse = [&](QTextDocument* doc) {
            const int       start  = findText(doc, a("sunset.png"));
            QTextCharFormat marker = formatAt(doc, start);
            marker.setProperty(layoutformat::kCollapsedLabel, a("sunset.png"));
            marker.setProperty(layoutformat::kCollapsedSeparator, QString(QChar::LineSeparator));
            QTextCursor c(doc);
            c.setPosition(start - 1);
            c.setPosition(start + 10, QTextCursor::KeepAnchor);
            c.insertText(QString(QChar(0x2060)), marker);
            return start - 1;
        };
        // Folded in the light chat, then dark mode on and off: the marker is recoloured like the link it is, and given back.
        QTextDocument light;
        fill(&light);
        const int     at     = collapse(&light);
        const QString folded = deepFormatsOf(&light);
        QVERIFY(darkchat::recolor(&light, maps.forward, true) >= 2);
        QCOMPARE(formatAt(&light, at).foreground().color(), t.link);
        QCOMPARE(formatAt(&light, at).stringProperty(layoutformat::kCollapsedLabel), a("sunset.png"));
        darkchat::recolor(&light, maps.back, false);
        QCOMPARE(deepFormatsOf(&light), folded);
        // Folded while dark mode is on (the album folds after the load's recolouring): off gives the skin's colour back, the
        // same document as folded in the light chat, nothing of dark mode left.
        QTextDocument dark;
        fill(&dark);
        QVERIFY(darkchat::recolor(&dark, maps.forward, true) >= 2);
        const int darkAt = collapse(&dark);
        QCOMPARE(formatAt(&dark, darkAt).foreground().color(), t.link);
        QVERIFY(formatAt(&dark, darkAt).hasProperty(darkchat::kOriginal));
        QCOMPARE(darkchat::recolor(&dark, maps.forward, true), 0); // a second pass leaves it
        darkchat::recolor(&dark, maps.back, false);
        QCOMPARE(formatAt(&dark, darkAt).foreground().color(), QColor(0x1c, 0x82, 0xcc));
        QCOMPARE(darkchat::taggedCount(&dark), 0);
        QCOMPARE(deepFormatsOf(&dark), folded);

        // The reply line's line break after its picture has the header's format (here TeamSpeak's time colour): made while
        // dark mode was on, it kept dark mode's colour after off (live: DARKORIG on it).
        QTextCharFormat time;
        time.setForeground(QColor(0x33, 0x59, 0x7d));
        QTextDocument reply;
        QTextCursor   rc(&reply);
        rc.insertText(a("<16:00:17>"), time);
        QVERIFY(darkchat::recolor(&reply, maps.forward, true) == 1);
        QTextCharFormat separator = formatAt(&reply, 0); // replydoc::replace: the header's format, as the document has it then
        separator.setProperty(replydoc::kSeparatorProperty, true);
        rc.insertText(QString(QChar::LineSeparator), separator);
        QCOMPARE(formatAt(&reply, 10).foreground().color(), t.muted);
        darkchat::recolor(&reply, maps.back, false);
        QCOMPARE(formatAt(&reply, 10).foreground().color(), QColor(0x33, 0x59, 0x7d));
        QVERIFY(formatAt(&reply, 10).boolProperty(replydoc::kSeparatorProperty));
        QCOMPARE(darkchat::taggedCount(&reply), 0);
    }

    // ---- 2.7.1 review: a revealed text spoiler's text and a poll card's line break ---------------------------------------------
    // A revealed spoiler's text keeps TeamSpeak's format with the tint (its format before in kBefore, given back at unload),
    // a link's space in a hidden one the link's colour without the link, and the poll card's line break the message's format:
    // TeamSpeak's colours with a text spoiler's or a poll's bookkeeping, recoloured both ways. Before, a link revealed while
    // dark mode was on stayed in dark mode's link colour after off (a salmon link in the light chat), and one revealed in the
    // light chat kept the light link colour on dark mode's background.
    void revealedSpoilerAndPollBreak()
    {
        const darkpalette::Tokens& t    = darkpalette::tokens();
        const SkinMaps             maps = skinMaps();
        const tschat::Skin         s    = tschat::lightSkin();
        const QColor               light(0x1c, 0x82, 0xcc);
        const QString              originalKey = QString::number(darkchat::kOriginal, 16) + QLatin1Char('=');
        const auto                 fill        = [&](QTextDocument* doc) {
            doc->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
            tschat::append(doc, tschat::messageHtml(s, a("16:00:17"), 7, QString::fromLatin1(tschat::kKaiUid), a("Kai"),
                                                    a("look [spoiler]at ") + tschat::linkHtml(s, a("https://example.com/hidden"), a("the map"))
                                                        + a(" now[/spoiler] ") + tschat::linkHtml(s, a("https://example.com/open"))));
        };
        // As ChatTextSpoilers' pass: every pair hidden, or straight into the revealed state (Settings: reveal spoilers).
        const auto hide = [](QTextDocument* doc, bool revealed) {
            int              serial = 0;
            const QTextBlock block  = doc->lastBlock();
            const auto       pairs  = textspoilerdoc::rawPairs(block, replydoc::parseBlock(block).textStart);
            QVector<QString> keys;
            QVector<bool>    open;
            for (int i = 0; i < pairs.size(); ++i) {
                keys << QString::fromLatin1("k%1").arg(i);
                open << revealed;
            }
            return textspoilerdoc::hide(doc, pairs, keys, open, &serial);
        };
        const auto revealAll = [](QTextDocument* doc) {
            int n = 0;
            for (const textspoilerdoc::Spoiler& sp : textspoilerdoc::spoilersIn(doc))
                n += textspoilerdoc::reveal(doc, sp) ? 1 : 0;
            return n;
        };
        const auto hiddenLink = [](QTextDocument* doc) { return formatAt(doc, findText(doc, a("map"))); };
        const auto openLink   = [](QTextDocument* doc) { return formatAt(doc, findText(doc, a("example.com/open"))); };

        // Revealed in the light chat (a click), then dark mode on and off: the revealed link like any link, the way back exact.
        QTextDocument clicked;
        fill(&clicked);
        QCOMPARE(hide(&clicked, false), 1);
        QCOMPARE(revealAll(&clicked), 1);
        QCOMPARE(textspoilerdoc::kindOf(hiddenLink(&clicked)), int(textspoilerdoc::Revealed));
        QCOMPARE(hiddenLink(&clicked).foreground().color(), light);
        const QString revealedLight = deepFormatsOf(&clicked);
        QVERIFY(darkchat::recolor(&clicked, maps.forward, true) >= 3);
        QCOMPARE(openLink(&clicked).foreground().color(), t.link);
        QCOMPARE(hiddenLink(&clicked).foreground().color(), t.link);
        QCOMPARE(hiddenLink(&clicked).property(darkchat::kOriginal).toUInt(), light.rgba());
        QCOMPARE(qvariant_cast<QTextFormat>(hiddenLink(&clicked).property(textspoilerdoc::kBefore)).foreground().color(), t.link);
        QCOMPARE(darkchat::recolor(&clicked, maps.forward, true), 0); // a second pass leaves it
        darkchat::recolor(&clicked, maps.back, false);
        QCOMPARE(deepFormatsOf(&clicked), revealedLight);

        // Hidden in the light chat (the link's space between its words keeps its colour without the link), dark mode on and
        // off: exact.
        QTextDocument hidden;
        fill(&hidden);
        QCOMPARE(hide(&hidden, false), 1);
        const QString hiddenLight = deepFormatsOf(&hidden);
        QVERIFY(darkchat::recolor(&hidden, maps.forward, true) >= 2);
        darkchat::recolor(&hidden, maps.back, false);
        QCOMPARE(deepFormatsOf(&hidden), hiddenLight);

        // Revealed while dark mode is on (a click, and the setting's straight reveal): dark mode's link colour then, the skin's
        // after off, and nothing of dark mode left in what the spoiler gives back at the unload.
        for (const bool straight : {false, true}) {
            QTextDocument dark;
            fill(&dark);
            QVERIFY(darkchat::recolor(&dark, maps.forward, true) >= 3);
            QCOMPARE(hide(&dark, straight), 1);
            if (!straight)
                QCOMPARE(revealAll(&dark), 1);
            QCOMPARE(textspoilerdoc::kindOf(hiddenLink(&dark)), int(textspoilerdoc::Revealed));
            QCOMPARE(hiddenLink(&dark).foreground().color(), t.link);
            darkchat::recolor(&dark, maps.back, false);
            QCOMPARE(hiddenLink(&dark).foreground().color(), light);
            QCOMPARE(openLink(&dark).foreground().color(), light);
            QCOMPARE(darkchat::taggedCount(&dark), 0);
            QVERIFY2(!deepFormatsOf(&dark).contains(originalKey), qPrintable(deepFormatsOf(&dark)));
            QCOMPARE(textspoilerdoc::restoreAll(&dark), 1);
            QCOMPARE(hiddenLink(&dark).foreground().color(), light);
            QVERIFY(!hiddenLink(&dark).hasProperty(textspoilerdoc::kKind));
            QVERIFY2(!deepFormatsOf(&dark).contains(originalKey), qPrintable(deepFormatsOf(&dark)));
        }

        // The poll card's line break (polldoc: the message's format at a 1 px size), made while dark mode was on.
        QTextCharFormat time;
        time.setForeground(QColor(0x33, 0x59, 0x7d));
        QTextDocument poll;
        QTextCursor   pc(&poll);
        pc.insertText(a("<16:00:17>"), time);
        QVERIFY(darkchat::recolor(&poll, maps.forward, true) == 1);
        QTextCharFormat separator = formatAt(&poll, 0);
        separator.clearProperty(QTextFormat::FontPointSize);
        separator.setProperty(QTextFormat::FontPixelSize, 1);
        separator.setProperty(polldoc::kSeparatorProperty, true);
        pc.insertText(QString(QChar::LineSeparator), separator);
        QCOMPARE(formatAt(&poll, 10).foreground().color(), t.muted);
        darkchat::recolor(&poll, maps.back, false);
        QCOMPARE(formatAt(&poll, 10).foreground().color(), QColor(0x33, 0x59, 0x7d));
        QVERIFY(formatAt(&poll, 10).boolProperty(polldoc::kSeparatorProperty));
        QCOMPARE(darkchat::taggedCount(&poll), 0);
    }

    // ---- T-8c (review fix): the chat layout styled TeamSpeak's lines, dark mode on and off, then Classic or unload ----------
    void layoutAcrossToggle()
    {
        const darkpalette::Tokens& k = darkpalette::tokens();
        installSkin();
        TsWindow       t   = shownWindow(0, 0);
        QTextDocument* doc = t.visible->document();
        QTextDocument* ref = t.hidden->document(); // the same lines, never styled
        fillLayoutChat(doc, tschat::lightSkin());
        fillLayoutChat(ref, tschat::lightSkin());
        QTextDocument lightRef;
        lightRef.setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        fillLayoutChat(&lightRef, tschat::lightSkin());
        const QString light = formatsOf(&lightRef);
        QVERIFY2(formatsOf(doc) == light, qPrintable(firstDiff(formatsOf(doc), light)));
        int        styled = 0;
        const auto style  = [doc, &styled](bool dark) {
            layoutformat::LayoutEditScope editing; // as ChatLayout edits
            styled = layoutdoc::styleAll(doc, layoutEnv(dark));
        };
        const auto classic = [doc] {
            layoutformat::LayoutEditScope editing;
            layoutdoc::restoreAll(doc);
        };
        style(false);
        QCOMPARE(styled, 6);
        QCOMPARE(layoutdoc::usualNickColor(doc), QColor(0x00, 0x2f, 0x5d));
        QCOMPARE(layoutdoc::usualLinkColor(doc), QColor(0x1c, 0x82, 0xcc));

        auto          dark   = makeDark();
        const quint64 serial = darkchat::keptSerial();
        dark->setEnabled(true);
        QVERIFY(darkchat::keptSerial() > serial);
        pump(100); // the hidden reference by the slice timer
        QVERIFY(!dark->isPending(ref));
        // (c) The layout reads TeamSpeak's lines as dark mode shows them, before and after its restyle for the dark palette.
        QCOMPARE(layoutdoc::usualNickColor(doc), k.text);
        QCOMPARE(layoutdoc::usualLinkColor(doc), k.link);
        classic();
        style(true);
        QCOMPARE(layoutdoc::usualNickColor(doc), k.text);
        QCOMPARE(layoutdoc::usualLinkColor(doc), k.link);
        // (a) Settings -> Chat -> Chat layout -> Classic while dark mode is on: TeamSpeak's lines exactly as dark mode makes them of
        // lines never styled (readable, each with its way back).
        classic();
        QVERIFY2(formatsOf(doc) == formatsOf(ref), qPrintable(firstDiff(formatsOf(doc), formatsOf(ref))));
        QCOMPARE(colorAt(doc, findText(doc, a("Kai"))), k.text);
        // Cozy again, then off: TeamSpeak's own colours back, also once the layout gives the lines back (Classic, unload).
        style(true);
        dark->setEnabled(false);
        QCOMPARE(layoutdoc::usualNickColor(doc), QColor(0x00, 0x2f, 0x5d)); // (c)
        QCOMPARE(layoutdoc::usualLinkColor(doc), QColor(0x1c, 0x82, 0xcc));
        classic();
        style(false); // the layout's restyle for the light palette
        classic();
        QVERIFY2(formatsOf(doc) == light, qPrintable(firstDiff(formatsOf(doc), light)));
        QCOMPARE(formatsOf(ref), light);

        // (b) Lines that came in while dark mode was on, styled by the layout then: the skin's colours once it is off.
        doc->clear();
        dark->setEnabled(true);
        fillLayoutChat(doc, darkEraSkin()); // TeamSpeak's insertions (the incremental pass sees them)
        pump(30);
        QCOMPARE(darkchat::taggedCount(doc), 0); // they read on the dark chat already
        style(true);
        dark->setEnabled(false);
        QCOMPARE(layoutdoc::usualNickColor(doc), QColor(0x00, 0x2f, 0x5d));
        QCOMPARE(layoutdoc::usualLinkColor(doc), QColor(0x1c, 0x82, 0xcc));
        classic();
        const QString lightText = formatsOf(&lightRef, false);
        QVERIFY2(formatsOf(doc, false) == lightText, qPrintable(firstDiff(formatsOf(doc, false), lightText)));
        // The same when the plugin goes with dark mode on: its shutdown first, the chat layer's restore after it.
        doc->clear();
        dark->setEnabled(true);
        fillLayoutChat(doc, darkEraSkin());
        pump(30);
        style(true);
        dark->shutdown();
        classic();
        QVERIFY2(formatsOf(doc, false) == lightText, qPrintable(firstDiff(formatsOf(doc, false), lightText)));
    }

    // ---- T-9: lines added while dark mode is on ---------------------------------------------------------------------------
    void incrementalPass()
    {
        installSkin();
        TsWindow t    = shownWindow(10, 0, false);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        QTextDocument* doc = t.visible->document();
        PaintProbe     probe;
        probe.check = [doc] {
            const int at = findText(doc, a("dark blue"));
            return at >= 0 && doc->find(a("dark blue")).charFormat().hasProperty(darkchat::kOriginal);
        };
        t.visible->viewport()->installEventFilter(&probe);
        // Another plugin's fixed colour (unreadable on the dark chat), appended as TeamSpeak appends.
        appendHtml(t.visible, a("<p><span style=\"color:#00008b\">dark blue</span></p>"));
        t.visible->verticalScrollBar()->setValue(t.visible->verticalScrollBar()->maximum());
        QTRY_VERIFY(probe.paints > 0);
        QVERIFY(probe.okAtFirst);
        const int at = findText(doc, a("dark blue"));
        QVERIFY(ui::contrastRatio(colorAt(doc, at), darkpalette::tokens().window) >= 4.5);
        // An edit inside the layout's MutatingScope is left alone.
        {
            layoutformat::MutatingScope scope;
            appendHtml(t.visible, a("<p><span style=\"color:#000080\">mutating</span></p>"));
        }
        pump(30);
        QCOMPARE(colorAt(doc, findText(doc, a("mutating"))), QColor(0x00, 0x00, 0x80));
        t.visible->viewport()->removeEventFilter(&probe);
        dark->setEnabled(false);
        QCOMPARE(colorAt(doc, findText(doc, a("dark blue"))), QColor(0x00, 0x00, 0x8b));
    }

    // ---- T-9b (review fix): TS Media's lines printed after the colour choice changed while dark mode was on ---------------
    void lineColorChoiceWhileOn()
    {
        const darkpalette::Tokens& k = darkpalette::tokens();
        installSkin();
        TsWindow                 t    = shownWindow(3, 0, false);
        QTextDocument*           doc  = t.visible->document();
        const chatcolors::Colors skin = chatcolors::fromPalette(t.visible->palette());
        QVERIFY(!skin.dark);
        chatcolors::Colors darkChat;
        darkChat.base        = k.window;
        darkChat.text        = k.text;
        darkChat.link        = k.link;
        darkChat.dark        = true;
        darkChat.themeAccent = k.accent;
        auto dark            = makeDark();
        for (const char* choice : {"green", "blue", "teal", "orange", "pink", "text", ""}) {
            dark->setEnabled(true);
            Settings::instance().chatLineColor = QString::fromLatin1(choice); // Settings -> Chat -> OK while dark mode is on
            // What TS Media prints now (printInfoLink: the prefix in bold, the link underlined, both in the accent).
            const chatcolors::LineColors now  = chatcolors::lineColors(darkChat, Settings::instance().chatLineColor);
            const QString                file = QString::fromLatin1("%1.png").arg(QString::fromLatin1(*choice ? choice : "automatic"));
            appendHtml(t.visible, QString::fromLatin1("<p><span style=\"color:%1;font-weight:600\">TS Media chat</span> Sent "
                                                      "<a href=\"https://example.com/%2\" style=\"color:%1;text-decoration:underline\">%2</a></p>")
                                      .arg(now.accent, file));
            pump(30);
            dark->setEnabled(false);
            const chatcolors::LineColors light = chatcolors::lineColors(skin, Settings::instance().chatLineColor);
            const int                    at    = findText(doc, file);
            QVERIFY(at > 0);
            QCOMPARE(colorAt(doc, at).name(), light.accent);
            // 2.7.1: Same as the chat text too (its bold prefix has the nickname's look in dark mode; 2.7.0 gave it the
            // nickname's colour).
            const int prefix = doc->find(a("TS Media chat"), at, QTextDocument::FindBackward).selectionStart();
            QVERIFY(prefix > 0);
            QCOMPARE(colorAt(doc, prefix).name(), light.accent);
        }
    }

    // ---- T-9c (review fix): dark mode applied before any chat exists (TeamSpeak's start), on TeamSpeak's dark skin -----------
    void snapshotWithoutChat()
    {
        for (QWidget* w : QApplication::allWidgets()) {
            if (w->objectName() == QLatin1String("ChatTab"))
                QSKIP("a chat of another test is still there");
        }
        qApp->setStyleSheet(a("QTextEdit#ChatTab { background: #2f3136; color: #dcddde; border: 1px solid #202225; }\n"));
        auto dark = makeDark();
        dark->setEnabled(true);
        TsWindow       t   = shownWindow(0, 0); // the chats come later
        QTextDocument* doc = t.visible->document();
        QVERIFY(dark->isTracked(doc));
        chatcolors::Colors darkChat;
        darkChat.base        = darkpalette::tokens().window;
        darkChat.text        = darkpalette::tokens().text;
        darkChat.dark        = true;
        darkChat.themeAccent = darkpalette::tokens().accent;
        appendHtml(t.visible, QString::fromLatin1("<p><span style=\"color:%1;font-weight:600\">TS Media chat</span> Sent a.png</p>")
                                  .arg(chatcolors::lineColors(darkChat, QString()).accent));
        pump(30);
        dark->setEnabled(false);
        pump(20);
        const chatcolors::Colors skin = chatcolors::fromPalette(t.visible->palette());
        QVERIFY(skin.dark);
        // The skin's own Automatic colour (2.6's on its dark chat), not the one of TeamSpeak's light chat.
        QCOMPARE(colorAt(doc, findText(doc, a("TS Media chat"))).name(), chatcolors::lineColors(skin, QString()).accent);
        QCOMPARE(chatcolors::lineColors(skin, QString()).accent, a("#949cf7"));
    }

    // ---- T-9d (2.7.1): TS Media's lines printed while dark mode is on get exactly the colours TS Media prints in the skin
    // back when it goes off: every shape (the bold label, the warning, a link, /tsmedia help's bold commands, the chat
    // layout's bold notice, the text), every colour choice, the choice in use when dark mode went on or changed while on, in
    // the visible chat and in a hidden one (waiting for its slice, or recoloured by it). TeamSpeak's names and channels added
    // then still get the skin's colours (live 2.7.0: the bold "TS Media chat" with Same as the chat text, and the bold words
    // of TS Media's text, came back in the nickname colour) ----------------------------------------------------------------
    void ownLinesWhileDark()
    {
        installSkin();
        TsWindow                 t            = shownWindow(3, 3, false);
        const auto               restoreLines = qScopeGuard([] { ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); });
        const chatcolors::Colors skin         = skinColorsOf(t.visible);
        QVERIFY(!skin.dark);
        const QString  sheet  = QString::fromLatin1(kSkinChat);
        QTextDocument* doc    = t.visible->document();
        QTextDocument* hidden = t.hidden->document();
        auto           dark   = makeDark();
        for (const char* name : kEveryChoice) {
            const QString choice = QString::fromLatin1(name);
            const QString want   = ownLinesReference(sheet, skin, choice);
            const QString what   = choice.isEmpty() ? a("automatic") : choice;
            // (a) The choice in use when dark mode goes on.
            Settings::instance().chatLineColor = choice;
            dark->setEnabled(true);
            int first = appendOwnLines(doc, darkChatColors(), choice);
            pump(30); // the incremental pass
            dark->setEnabled(false);
            QVERIFY2(charColors(doc, first) == want, qPrintable(what + a(" (a) ") + firstLineDiff(charColors(doc, first), want)));
            QCOMPARE(darkchat::taggedCount(doc), 0);
            QCOMPARE(colorAt(doc, findText(doc, a("Kai"), doc->findBlockByNumber(first).position())), QColor(0x00, 0x2f, 0x5d));
            QCOMPARE(colorAt(doc, findText(doc, a("Home"), doc->findBlockByNumber(first).position())), QColor(0x00, 0x2f, 0x5d));
            // (b) Automatic when dark mode went on, the choice changed while it is on (Settings -> OK).
            Settings::instance().chatLineColor = QString();
            dark->setEnabled(true);
            Settings::instance().chatLineColor = choice;
            first = appendOwnLines(doc, darkChatColors(), choice);
            pump(30);
            dark->setEnabled(false);
            QVERIFY2(charColors(doc, first) == want, qPrintable(what + a(" (b) ") + firstLineDiff(charColors(doc, first), want)));
            QCOMPARE(darkchat::taggedCount(doc), 0);
            // (c) A hidden chat: printed into it right after dark mode went on (still waiting for the slice: its new lines are
            // mapped back), then once the slice recoloured it.
            for (const bool sliced : {false, true}) {
                dark->setEnabled(true);
                if (sliced)
                    pump(60);
                QCOMPARE(dark->isPending(hidden), !sliced);
                first = appendOwnLines(hidden, darkChatColors(), choice);
                if (sliced)
                    pump(30);
                dark->setEnabled(false);
                const QString got = charColors(hidden, first);
                QVERIFY2(got == want, qPrintable(what + (sliced ? a(" (c, sliced) ") : a(" (c) ")) + firstLineDiff(got, want)));
                QCOMPARE(darkchat::taggedCount(hidden), 0);
            }
        }
    }

    // ---- T-9e (2.7.1): the same lines styled by the chat layout while dark mode is on (Cozy keeps the label in its prefix
    // picture's runs), then off and Settings -> TeamSpeak classic; and the plugin going with dark mode on (DarkMode first,
    // the chat layer's restore after it) ----------------------------------------------------------------------------------
    void ownLinesCozyThenClassic()
    {
        installSkin();
        TsWindow                 t            = shownWindow(0, 0);
        const auto               restoreLines = qScopeGuard([] { ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); });
        const chatcolors::Colors skin         = skinColorsOf(t.visible);
        QTextDocument*           doc          = t.visible->document();
        const auto               classic      = [doc] {
            layoutformat::LayoutEditScope editing;
            layoutdoc::restoreAll(doc);
        };
        for (const char* name : {"text", ""}) {
            const QString choice = QString::fromLatin1(name);
            const QString want   = ownLinesReference(QString::fromLatin1(kSkinChat), skin, choice, false);
            for (const bool unload : {false, true}) {
                const QString what = (choice.isEmpty() ? a("automatic") : choice) + (unload ? a(", unload") : a(", off"));
                doc->clear();
                Settings::instance().chatLineColor = choice;
                auto dark                          = makeDark();
                dark->setEnabled(true);
                appendOwnLines(doc, darkChatColors(), choice, false);
                pump(30);
                int styled = 0;
                {
                    layoutformat::LayoutEditScope editing; // as ChatLayout edits
                    styled = layoutdoc::styleAll(doc, layoutEnv(true));
                }
                QVERIFY2(styled >= 3, qPrintable(what)); // the label lines and the notice are in prefix pictures now
                if (unload)
                    dark->shutdown();
                else
                    dark->setEnabled(false);
                classic();
                QVERIFY2(charColors(doc, 0) == want, qPrintable(what + QLatin1Char(' ') + firstLineDiff(charColors(doc, 0), want)));
                QCOMPARE(darkchat::taggedCount(doc), 0);
            }
        }
    }

    // ---- T-9f (2.7.1): Options -> Addons -> Reload with dark mode on: the lines printed while it was on get their colours
    // back at the unload, and the next load takes those as their colours before (2.7.0 kept the nickname colour for good) --
    void ownLinesAcrossReload()
    {
        installSkin();
        TsWindow                 t            = shownWindow(3, 0, false);
        const auto               restoreLines = qScopeGuard([] { ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); });
        const chatcolors::Colors skin         = skinColorsOf(t.visible);
        QTextDocument*           doc          = t.visible->document();
        for (const char* name : {"text", "", "pink"}) {
            const QString choice               = QString::fromLatin1(name);
            const QString want                 = ownLinesReference(QString::fromLatin1(kSkinChat), skin, choice);
            Settings::instance().chatLineColor = choice;
            auto first                         = makeDark();
            first->setEnabled(true);
            const int from = appendOwnLines(doc, darkChatColors(), choice);
            pump(30);
            first->shutdown(); // the unload (TeamSpeak's main window shown)
            first.reset();
            QVERIFY2(charColors(doc, from) == want, qPrintable(firstLineDiff(charColors(doc, from), want)));
            auto again = makeDark(); // the load, dark mode still on
            again->setEnabled(true);
            pump(30);
            QVERIFY(darkchat::taggedCount(doc) > 0);
            again->setEnabled(false);
            QVERIFY2(charColors(doc, from) == want, qPrintable(firstLineDiff(charColors(doc, from), want)));
            QCOMPARE(darkchat::taggedCount(doc), 0);
        }
    }

    // ---- T-9g (2.7.1): another skin picked in Options -> Design while dark mode is on: the lines printed while it was on
    // get the colours TS Media prints in that skin (not its nickname colour) ----------------------------------------------
    void skinAdoptedOwnLines()
    {
        const auto restoreLines = qScopeGuard([] { ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); });
        for (const char* name : {"text", ""}) {
            QApplication::setPalette(m_savedPalette);
            installSkin();
            TsWindow       t                   = shownWindow(3, 0, false);
            QTextDocument* doc                 = t.visible->document();
            const QString  choice              = QString::fromLatin1(name);
            Settings::instance().chatLineColor = choice;
            auto dark                          = makeDark();
            dark->setEnabled(true);
            pump(30);
            const int from = appendOwnLines(doc, darkChatColors(), choice);
            pump(30);
            qApp->setStyleSheet(QString::fromLatin1(kOtherSheet));
            for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
                e->document()->setDefaultStyleSheet(QString::fromLatin1(kOtherChat));
            pump(80);
            QCOMPARE(dark->stats().adoptions, 1);
            dark->setEnabled(false);
            pump(30);
            QCOMPARE(qApp->styleSheet(), QString::fromLatin1(kOtherSheet));
            const chatcolors::Colors other = skinColorsOf(t.visible);
            QVERIFY(!other.dark);
            const QString want = ownLinesReference(QString::fromLatin1(kOtherChat), other, choice);
            QVERIFY2(charColors(doc, from) == want, qPrintable(QString::fromLatin1(name) + QLatin1Char(' ') + firstLineDiff(charColors(doc, from), want)));
            if (choice == a("text"))
                QCOMPARE(colorAt(doc, findText(doc, a("TS Media chat"), doc->findBlockByNumber(from).position())), QColor(0x10, 0x10, 0x10));
            dark.reset();
            delete t.window;
        }
    }

    // ---- T-9g2 (2.7.1 review): only the application palette changes while dark mode is on (another plugin or tool), on a
    // skin whose chat rule sets no text colour (TeamSpeak's own sets only its background): the lines printed while dark get
    // the skin's colours back, not ones taken from dark mode's sheet (the probe inherited its ChatTab text colour: grey) ---
    void paletteOnlyAdoptionOwnLines()
    {
        const auto    restoreLines = qScopeGuard([] { ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); });
        const QString sheet        = a("QTextEdit#ChatTab { background-color: palette(base); border: 1px solid #a0a0a0; }\n"
                                       "QTreeView#ServerTreeView { qproperty-rootIsDecorated: false; }\n");
        for (const char* name : {"text", "", "pink"}) {
            QApplication::setPalette(m_savedPalette);
            qApp->setStyleSheet(sheet);
            TsWindow       t                   = shownWindow(3, 0, false);
            QTextDocument* doc                 = t.visible->document();
            const QString  choice              = QString::fromLatin1(name);
            Settings::instance().chatLineColor = choice;
            auto dark                          = makeDark();
            dark->setEnabled(true);
            pump(30);
            const int from = appendOwnLines(doc, darkChatColors(), choice);
            pump(30);
            QPalette p = QApplication::palette();
            p.setColor(QPalette::Link, QColor(0x20, 0x60, 0xa0)); // a palette-only change: the sheet stays dark mode's
            QApplication::setPalette(p);
            pump(80);
            QCOMPARE(dark->stats().adoptions, 1);
            QCOMPARE(qApp->styleSheet(), dark->appliedSheet());
            dark->setEnabled(false);
            pump(30);
            QCOMPARE(qApp->styleSheet(), sheet);
            QCOMPARE(QApplication::palette().color(QPalette::Active, QPalette::Link), QColor(0x20, 0x60, 0xa0));
            const chatcolors::Colors skin = skinColorsOf(t.visible);
            QVERIFY(!skin.dark);
            const QString want = ownLinesReference(QString::fromLatin1(kSkinChat), skin, choice);
            QVERIFY2(charColors(doc, from) == want, qPrintable(QString::fromLatin1(name) + QLatin1Char(' ') + firstLineDiff(charColors(doc, from), want)));
            // The label: for Same as the chat text the chat's own text colour (the palette's: the skin's rule has none).
            QCOMPARE(colorAt(doc, findText(doc, a("TS Media chat"), doc->findBlockByNumber(from).position())).name(), chatcolors::lineColors(skin, choice).accent);
            if (choice == a("text"))
                QCOMPARE(skin.text, m_savedPalette.color(QPalette::Active, QPalette::Text));
            dark.reset();
            delete t.window;
        }
    }

    // ---- 2.7.2 (D2): every shape of a skin taken over while dark mode is on (a complete palette with the sheet left as it was,
    // the skin's own palette set again, a sheet and a complete palette in one turn, a sheet only, one role): the snapshot of
    // the skin's chat is never read under dark mode's look, and the lines printed while dark get the skin's colours back.
    // 2.7.1 read the visible chat when the merged palette equalled the application's, under dark mode's ChatTab rule ------
    void fullPaletteAdoptionOwnLines_data()
    {
        QTest::addColumn<int>("shape");
        QTest::newRow("complete palette, sheet kept") << 0;
        QTest::newRow("the skin's palette again, sheet kept") << 1;
        QTest::newRow("sheet and complete palette") << 2;
        QTest::newRow("sheet only") << 3;
        QTest::newRow("one role") << 4;
    }
    void fullPaletteAdoptionOwnLines()
    {
        QFETCH(int, shape);
        const auto    restoreLines = qScopeGuard([] { ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); });
        const QString sheet        = a("QTextEdit#ChatTab { background-color: palette(base); border: 1px solid #a0a0a0; }\n"
                                       "QTreeView#ServerTreeView { qproperty-rootIsDecorated: false; }\n");
        const QString otherSheet   = sheet + a("QPushButton { padding: 3px; }\n");
        for (const char* name : {"text", "", "pink"}) {
            QApplication::setPalette(m_savedPalette);
            qApp->setStyleSheet(sheet);
            TsWindow       t                   = shownWindow(3, 0, false);
            QTextDocument* doc                 = t.visible->document();
            const QString  choice              = QString::fromLatin1(name);
            Settings::instance().chatLineColor = choice;
            auto dark                          = makeDark();
            dark->setEnabled(true);
            pump(30);
            const int from = appendOwnLines(doc, darkChatColors(), choice);
            pump(30);
            QPalette complete = m_savedPalette; // every role the skin's (none of dark mode's)
            complete.setColor(QPalette::Link, QColor(0x20, 0x60, 0xa0));
            QString wantSheet = sheet;
            switch (shape) {
            case 0:
                QApplication::setPalette(complete);
                break;
            case 1:
                QApplication::setPalette(m_savedPalette);
                break;
            case 2:
                qApp->setStyleSheet(otherSheet);
                QApplication::setPalette(complete);
                wantSheet = otherSheet;
                break;
            case 3:
                qApp->setStyleSheet(otherSheet);
                wantSheet = otherSheet;
                break;
            default: {
                QPalette p = QApplication::palette(); // dark mode's, one role changed
                p.setColor(QPalette::Link, QColor(0x20, 0x60, 0xa0));
                QApplication::setPalette(p);
                break;
            }
            }
            pump(80);
            QCOMPARE(dark->stats().adoptions, 1);
            const chatcolors::Colors snapshot = dark->snapshotChatColors();
            QVERIFY2(!snapshot.dark, qPrintable(QString::fromLatin1("%1: snapshot base %2 text %3").arg(QString::fromLatin1(name), snapshot.base.name(), snapshot.text.name())));
            dark->setEnabled(false);
            pump(30);
            QCOMPARE(qApp->styleSheet(), wantSheet);
            const chatcolors::Colors skin = skinColorsOf(t.visible);
            QVERIFY(!skin.dark);
            QCOMPARE(snapshot.base, skin.base);
            QCOMPARE(snapshot.text, skin.text);
            const QString want = ownLinesReference(QString::fromLatin1(kSkinChat), skin, choice);
            QVERIFY2(charColors(doc, from) == want, qPrintable(QString::fromLatin1(name) + QLatin1Char(' ') + firstLineDiff(charColors(doc, from), want)));
            QCOMPARE(colorAt(doc, findText(doc, a("TS Media chat"), doc->findBlockByNumber(from).position())).name(), chatcolors::lineColors(skin, choice).accent);
            dark.reset();
            delete t.window;
        }
    }

    // ---- T-9h (2.7.1): an info panel TeamSpeak rendered while dark mode is on (a client selected), then off before it renders
    // again: its title gets the skin's title colour, not TS Media's text colour --------------------------------------------
    void infoTitleWhileDark()
    {
        installSkin();
        TsWindow      t     = shownWindow(3, 0, false);
        const QString sheet = QString::fromLatin1(kSkinChat)
                              + a(".InfoFrame_Title .TextMessage_UserLink, .InfoFrame_Title .TextMessage_ChannelLink, "
                                  ".InfoFrame_Title .TextMessage_ServerLink { color: #191970; font-weight: normal; }\n");
        t.info->document()->setDefaultStyleSheet(sheet);
        const QString html = a("<p class=\"InfoFrame_Title\"><a class=\"TextMessage_UserLink\" href=\"client://3/z~Dex\">Dex</a></p>");
        QTextDocument ref;
        ref.setDefaultStyleSheet(sheet);
        ref.setHtml(html);
        QCOMPARE(colorAt(&ref, findText(&ref, a("Dex"))), QColor(0x19, 0x19, 0x70));
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        t.info->setHtml(html); // TeamSpeak renders the panel again on a selection
        pump(30);
        QCOMPARE(colorAt(t.info->document(), findText(t.info->document(), a("Dex"))), darkpalette::tokens().text);
        dark->setEnabled(false);
        QCOMPARE(colorAt(t.info->document(), findText(t.info->document(), a("Dex"))), QColor(0x19, 0x19, 0x70));
    }

    // ---- 2.7.2 (D5): lines TeamSpeak printed while dark mode was on get the skin's text style back with its colours (weight,
    // italic, underline, size), off and at the unload, on TeamSpeak's skin, a skin that sets colours only (names not bold,
    // links underlined) and a dark community skin; the lines printed before stay byte-exact. 2.7.1 gave back the colour only:
    // a name printed while dark stayed bold on a skin whose names aren't --------------------------------------------------
    void fontsBackAfterDark_data()
    {
        QTest::addColumn<QString>("sheet");
        QTest::addColumn<bool>("unload");
        const QVector<QPair<const char*, QString>> skins{{"TeamSpeak's skin", teamSpeakLikeChat()},
                                                         {"colours only", QString::fromLatin1(kPlainChat)},
                                                         {"community dark", QString::fromLatin1(kCommunityChat)}};
        for (const auto& skin : skins) {
            for (const bool unload : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1("%1, %2").arg(QString::fromLatin1(skin.first), unload ? a("unload") : a("off")))) << skin.second << unload;
        }
    }
    void fontsBackAfterDark()
    {
        QFETCH(QString, sheet);
        QFETCH(bool, unload);
        installSkin();
        TsWindow t = shownWindow(0, 0, false);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(sheet);
        QTextDocument* doc = t.visible->document();
        appendLines(t.visible, someLines(25, true));
        for (const QString& line : styledLines())
            appendHtml(t.visible, line); // the same classes before dark mode too
        const int     oldBlocks = doc->blockCount();
        const QString oldLines  = formatsOfBlocks(doc, 0, oldBlocks);
        auto          dark      = makeDark();
        dark->setEnabled(true);
        pump(30);
        const int first = doc->blockCount();
        for (const QString& line : styledLines())
            appendHtml(t.visible, line);
        pump(30); // the incremental pass
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        QTextDocument ref;
        ref.setDefaultStyleSheet(sheet);
        for (const QString& line : styledLines())
            appendTo(&ref, line);
        // 2.7.3 (X1): a host name the skin doesn't colour too (it inherits its line's: the probe asks in that line's class).
        const QStringList got  = fragmentsFrom(doc, first);
        const QStringList want = fragmentsFrom(&ref, 0);
        QVERIFY2(got == want, qPrintable(firstListDiff(got, want)));
        QCOMPARE(darkchat::taggedCount(doc), 0);
        QVERIFY2(formatsOfBlocks(doc, 0, oldBlocks) == oldLines, qPrintable(firstDiff(formatsOfBlocks(doc, 0, oldBlocks), oldLines)));
    }

    // ---- 2.7.3 (X1): a host name in a line TeamSpeak printed while dark mode was on ("Trying to connect to server on ...",
    // which TeamSpeak 3.6.2 prints as a TextMessage_Normal line) gets the style's look after off and at the unload: its own
    // host name colour, or, in a style without one, its line's (the community dark skin's informational #607d8b; 2.7.2 left
    // dark mode's #c2c0b6 there). The line printed before dark mode keeps its look exactly -------------------------------
    void hostNameTakesItsLine_data()
    {
        QTest::addColumn<QString>("sheet");
        QTest::addColumn<bool>("unload");
        QTest::addColumn<QString>("color");
        // Synthesized like the community dark skin (a bold host name without a colour, informational lines #607d8b).
        const QString community = QString::fromLatin1(kCommunityChat).replace(a("#4f6773"), a("#607d8b"));
        const QVector<std::tuple<const char*, QString, const char*>> skins{
            {"a style without a host name colour", community, "#607d8b"},
            {"TeamSpeak's skin, its own host name colour", teamSpeakLikeChat(), "#33597d"},
            {"colours only, the host name with the informational lines", QString::fromLatin1(kPlainChat), "#4a6f96"}};
        for (const auto& skin : skins) {
            for (const bool unload : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1("%1, %2").arg(QString::fromLatin1(std::get<0>(skin)), unload ? a("unload") : a("off"))))
                    << std::get<1>(skin) << unload << QString::fromLatin1(std::get<2>(skin));
        }
    }
    void hostNameTakesItsLine()
    {
        QFETCH(QString, sheet);
        QFETCH(bool, unload);
        QFETCH(QString, color);
        installSkin();
        TsWindow t = shownWindow(0, 0, false);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(sheet);
        QTextDocument* doc  = t.visible->document();
        const QString  line = styledLines().at(3); // the host name line, in TeamSpeak's TextMessage_Normal paragraph
        appendHtml(t.visible, line);               // before dark mode
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        appendHtml(t.visible, line); // while dark mode is on (the incremental pass)
        pump(30);
        const int before    = findText(doc, a("example.org"));
        const int whileDark = findText(doc, a("example.org"), before + 1);
        QVERIFY(before >= 0 && whileDark > before);
        QCOMPARE(colorAt(doc, whileDark), darkpalette::tokens().secondary);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        QTextDocument ref;
        ref.setDefaultStyleSheet(sheet);
        appendTo(&ref, line);
        const QString want = describe(formatAt(&ref, findText(&ref, a("example.org"))));
        QCOMPARE(describe(formatAt(doc, whileDark)), want);
        QCOMPARE(colorAt(doc, whileDark).name(), color);
        QCOMPARE(describe(formatAt(doc, before)), want);
        QCOMPARE(darkchat::taggedCount(doc), 0);
    }

    // ---- 2.7.3 review (X1): the line kinds dark mode's chat sheet shows alike (dropped, kicked and banned lines; the
    // informational lines with the time stamps and the channel, server, group and talk power changes) come back after off and
    // at the unload in one colour of the style's for that kind of look (a look maps to one colour), so a style that colours
    // kinds of one such group differently (the community dark skin's grouping, synthesized here: changes in a blue of their
    // own, dropped lines with the informational ones) gets those lines printed while dark mode was on back in the time stamps'
    // and the kick lines' colours, and the host name of an Invalid server address line in the informational lines' (FAQ:
    // "Dark mode and my own TeamSpeak skin"). TeamSpeak's own grouping comes back exactly; lines printed before dark mode
    // keep their look exactly in both ------------------------------------------------------------------------------------
    void lineKindsShownAlike_data()
    {
        QTest::addColumn<QString>("sheet");
        QTest::addColumn<bool>("unload");
        QTest::addColumn<QStringList>("differ"); // text, colour after: lines printed while dark mode was on
        const QString changes = a(".TextMessage_ChannelCreated, .TextMessage_ChannelDeleted, .TextMessage_ChannelEdited, .TextMessage_ChannelMoved, "
                                  ".TextMessage_ServerEdited, .TextMessage_GroupAssigned, .TextMessage_GroupRevoked, .TextMessage_TalkPowerAssigned, "
                                  ".TextMessage_TalkPowerRevoked, .TextMessage_TalkPowerRequested");
        // As TeamSpeak's default style groups them: the changes with the informational lines, dropped with kicked.
        const QString own = teamSpeakLikeChat() + a(".TextMessage_ClientDropped, .TextMessage_ClientBanned { color: #c9070a; }\n") + changes + a(" { color: #33597d; }\n");
        // As the community dark skin groups them (no host name colour, informational lines and time stamps #4f6773, kicks #b0060a).
        const QString grouped = QString::fromLatin1(kCommunityChat) + changes + a(" { color: #5a74c8; }\n.TextMessage_ClientDropped { color: #4f6773; }\n"
                                                                                  ".TextMessage_ClientBanned { color: #b0060a; }\n");
        const QStringList differ{a("dropped (connection lost)"), a("#b0060a"), a("Channel created:"), a("#4f6773"), a("Server name changed"), a("#4f6773"),
                                 a("was added to the server group"), a("#4f6773"), a("bad.example"), a("#4f6773")};
        for (const bool unload : {false, true}) {
            QTest::newRow(qPrintable(QString::fromLatin1("TeamSpeak's grouping, %1").arg(unload ? a("unload") : a("off")))) << own << unload << QStringList();
            QTest::newRow(qPrintable(QString::fromLatin1("the community dark skin's grouping, %1").arg(unload ? a("unload") : a("off")))) << grouped << unload << differ;
        }
    }
    void lineKindsShownAlike()
    {
        QFETCH(QString, sheet);
        QFETCH(bool, unload);
        QFETCH(QStringList, differ);
        installSkin();
        TsWindow t = shownWindow(0, 0, false);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(sheet);
        const QStringList lines{
            chatLine(a("21:31:01"), a("TextMessage_ServerError"), a("Invalid server address: <span class=\"TextMessage_HostName\">bad.example</span>")),
            chatLine(a("21:31:02"), a("TextMessage_ClientDropped"), user(a("Kim")) + a(" dropped (connection lost)")),
            chatLine(a("21:31:03"), a("TextMessage_ChannelCreated"), a("Channel created: <a class=\"TextMessage_ChannelLink\" href=\"channelid://5\">Games</a>")),
            chatLine(a("21:31:04"), a("TextMessage_ServerEdited"), a("Server name changed")),
            chatLine(a("21:31:05"), a("TextMessage_GroupAssigned"), user(a("Ann")) + a(" was added to the server group Admin")),
            chatLine(a("21:31:06"), a("TextMessage_ClientKicked"), user(a("Max")) + a(" was kicked from the server")),
            chatLine(a("21:31:07"), a("TextMessage_ClientJoined"), user(a("Bob")) + a(" entered your channel"))};
        QTextDocument* doc = t.visible->document();
        for (const QString& line : lines)
            appendHtml(t.visible, line); // before dark mode
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        for (const QString& line : lines)
            appendHtml(t.visible, line); // while dark mode is on (the incremental pass)
        pump(30);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        QTextDocument ref;
        ref.setDefaultStyleSheet(sheet);
        for (const QString& line : lines)
            appendTo(&ref, line);
        int checked = 0;
        for (const QString& text : {a("Invalid server address"), a("bad.example"), a("dropped (connection lost)"), a("Channel created:"), a("Games"),
                                    a("Server name changed"), a("was added to the server group"), a("Kim"), a("was kicked from the server"),
                                    a("entered your channel"), a("21:31:05")}) {
            const int before    = findText(doc, text);
            const int whileDark = findText(doc, text, before + 1);
            QVERIFY2(before >= 0 && whileDark > before, qPrintable(text));
            const QTextCharFormat own = formatAt(&ref, findText(&ref, text));
            QCOMPARE(describe(formatAt(doc, before)), describe(own)); // printed before dark mode: exactly as it was
            QTextCharFormat want = own;
            const int       at   = differ.indexOf(text);
            if (at >= 0)
                want.setForeground(QColor(differ.at(at + 1)));
            QVERIFY2(describe(formatAt(doc, whileDark)) == describe(want),
                     qPrintable(QString::fromLatin1("%1: %2, want %3").arg(text, describe(formatAt(doc, whileDark)), describe(want))));
            ++checked;
        }
        QCOMPARE(checked, 11);
        QCOMPARE(darkchat::taggedCount(doc), 0);
    }

    // ---- 2.7.2 (D5): the same lines styled by the chat layout while dark mode is on (Cozy and Compact recolour names and
    // channels in status lines): after off and TeamSpeak classic the name has the skin's text style; while styled, a property
    // the layout set itself stays the layout's -----------------------------------------------------------------------------
    void fontsBackInLayouts_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("Cozy") << int(layoutart::Mode::Cozy);
        QTest::newRow("Compact") << int(layoutart::Mode::Compact);
    }
    void fontsBackInLayouts()
    {
        QFETCH(int, mode);
        installSkin();
        TsWindow       t     = shownWindow(0, 0);
        const QString  sheet = QString::fromLatin1(kPlainChat);
        QTextDocument* doc   = t.visible->document();
        doc->setDefaultStyleSheet(sheet);
        // TeamSpeak's status line with a name and a channel (its classes; the info icon the layout knows rows by).
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        const auto line = [] {
            return a("<p class=\"TextMessage_ClientSwitched\">") + tschat::icon("MESSAGE_INFO")
                   + a("<span class=\"TextMessage_Time\">&lt;16:06:40&gt;</span> <a class=\"TextMessage_UserLink\" href=\"client://9/") + QString::fromLatin1(tschat::kNoraUid)
                   + a("~Nora\">\"Nora\"</a> connected to channel <a class=\"TextMessage_ChannelLink\" href=\"channelid://1\">\"Lobby\"</a></p>");
        };
        const auto env = [mode] {
            layoutdoc::Env e = layoutEnv(true);
            e.mode           = static_cast<layoutart::Mode>(mode);
            e.tokens         = layoutart::tokensFor(e.mode, QApplication::font(), false);
            return e;
        };
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        doc->addResource(QTextDocument::ImageResource, QUrl(a("iconpath:MESSAGE_INFO?size=13x13")), icon);
        for (int i = 0; i < 3; ++i)
            appendHtml(t.visible, line());
        pump(30);
        int styled = 0;
        {
            layoutformat::LayoutEditScope editing; // as ChatLayout edits
            styled = layoutdoc::styleAll(doc, env());
        }
        QVERIFY(styled >= 1);
        const int nora = findText(doc, a("Nora"));
        QVERIFY(nora >= 0);
        QVERIFY(formatAt(doc, nora).hasProperty(layoutformat::kApplied)); // the layout coloured the name
        dark->setEnabled(false);
        // While styled: the layout's colour stays, the text style is the skin's already.
        const QTextCharFormat styledName = formatAt(doc, findText(doc, a("Nora")));
        QVERIFY(styledName.hasProperty(layoutformat::kApplied));
        QVERIFY2(styledName.fontWeight() <= QFont::Normal, qPrintable(describe(styledName)));
        {
            layoutformat::LayoutEditScope editing;
            layoutdoc::restoreAll(doc); // Settings -> Chat -> TeamSpeak classic
        }
        QTextDocument ref;
        ref.setDefaultStyleSheet(sheet);
        ref.addResource(QTextDocument::ImageResource, QUrl(a("iconpath:MESSAGE_INFO?size=13x13")), icon);
        appendTo(&ref, line());
        const QString got  = describe(formatAt(doc, findText(doc, a("Nora"))));
        const QString want = describe(formatAt(&ref, findText(&ref, a("Nora"))));
        QCOMPARE(got, want);
        QCOMPARE(describe(formatAt(doc, findText(doc, a("Lobby")))), describe(formatAt(&ref, findText(&ref, a("Lobby")))));
        QVERIFY(darkLeftovers(doc).isEmpty());
        // A property the layout set itself (a mention pill's weight) stays the layout's on the way back.
        const darkpalette::Tokens& k     = darkpalette::tokens();
        const auto                 maps  = darkchat::buildMap(darkchat::classFormats(darkpalette::chatSheet(k), k.link),
                                                              darkchat::classFormats(sheet, QColor(0x1c, 0x82, 0xcc)), {}, QColor());
        QTextCharFormat            nick  = darkchat::classFormats(darkpalette::chatSheet(k), k.link).value(1).format; // UserLink
        QTextCharFormat            pill  = nick;
        pill.setProperty(layoutformat::kOrigChar, QVariant::fromValue<QTextFormat>(nick));
        pill.setForeground(QColor(0x11, 0x22, 0x33));
        pill.setFontWeight(QFont::DemiBold);
        pill.setProperty(layoutformat::kApplied, QVariantList{int(QTextFormat::ForegroundBrush), QVariant::fromValue(QBrush(QColor(0x11, 0x22, 0x33))), int(QTextFormat::FontWeight),
                                                              int(QFont::DemiBold)});
        QVERIFY(darkchat::mapBack(pill, maps));
        QCOMPARE(pill.fontWeight(), int(QFont::DemiBold));
        QVERIFY(pill.fontUnderline()); // the skin's (Qt's for a link it doesn't style): not the layout's
        QCOMPARE(pill.foreground().color(), QColor(0x11, 0x22, 0x33));
        QVERIFY(!qvariant_cast<QTextFormat>(pill.property(layoutformat::kOrigChar)).hasProperty(QTextFormat::FontWeight)); // its format before: the skin's
    }

    // ---- 2.7.2 (D5): the text style coming back moves lines (a weight, a size): every chat keeps its reader's place, a chat
    // at its end stays at its end (in placesKeptScaled's children too: fontsBackKeepsPlaceScaled) ---------------------------
    void fontsBackKeepsPlace()
    {
        const QFont savedFont = QApplication::font();
        if (!qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
            QFont font(a("Segoe UI"));
            font.setPixelSize(qRound(8.25 * qMax(96, qEnvironmentVariableIntValue("QT_FONT_DPI")) / 72.0));
            QApplication::setFont(font);
        }
        const auto restoreFont = qScopeGuard([&savedFont] { QApplication::setFont(savedFont); });
        installSkin();
        TsWindow t = shownWindow(0, 0);
        t.visible->document()->setDefaultStyleSheet(QString::fromLatin1(kPlainChat));
        appendLines(t.visible, someLines(30, true));
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        // 400 wrapping lines with names, history markers and errors, printed while dark (bold there, not in the skin).
        QStringList lines;
        for (int i = 0; i < 40; ++i)
            lines << styledLines() + someLines(5, true);
        appendLines(t.visible, lines.mid(0, 400));
        pump(30);
        t.visible->verticalScrollBar()->setValue(t.visible->verticalScrollBar()->maximum() / 2);
        pump(30);
        qInfo("fontsBackKeepsPlace: font %d px, device pixel ratio %.2f", QFontInfo(QApplication::font()).pixelSize(), t.window->devicePixelRatioF());
        const chatscroll::ReaderPlace place = chatscroll::captureReader(t.visible);
        QVERIFY(!place.bottom && !place.cursor.isNull());
        const int heightBefore = qRound(t.visible->document()->size().height());
        dark->setEnabled(false);
        pump(30);
        QVERIFY2(qAbs(drift(t.visible, place)) <= 2, qPrintable(QString::number(drift(t.visible, place))));
        qInfo("fontsBackKeepsPlace: document %d -> %d px", heightBefore, qRound(t.visible->document()->size().height()));
        // At its end, the chat stays at its end.
        dark->setEnabled(true);
        pump(30);
        appendLines(t.visible, lines.mid(0, 100));
        pump(30);
        t.visible->verticalScrollBar()->setValue(t.visible->verticalScrollBar()->maximum());
        pump(20);
        QVERIFY(chatscroll::atBottom(t.visible));
        dark->setEnabled(false);
        pump(30);
        QVERIFY(chatscroll::atBottom(t.visible));
    }

    void fontsBackKeepsPlaceScaled_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("value");
        QTest::newRow("QT_FONT_DPI=192") << a("QT_FONT_DPI") << a("192");
        QTest::newRow("QT_SCALE_FACTOR=2") << a("QT_SCALE_FACTOR") << a("2");
    }
    void fontsBackKeepsPlaceScaled()
    {
        QFETCH(QString, name);
        QFETCH(QString, value);
        QProcess            child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(a("QT_FONT_DPI"));
        env.remove(a("QT_SCALE_FACTOR"));
        env.insert(name, value);
        env.insert(a("QT_QPA_FONTDIR"), QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR", a("C:/Windows"))) + a("/Fonts"));
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        const QString results = m_dir->filePath(a("fonts.txt"));
        child.start(QCoreApplication::applicationFilePath(), {a("-platform"), a("offscreen"), a("-o"), results + a(",txt"), a("fontsBackKeepsPlace")});
        QVERIFY(child.waitForFinished(120000));
        QFile         file(results);
        const QString out = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString::fromLocal8Bit(child.readAll());
        QVERIFY2(child.exitCode() == 0, qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::fontsBackKeepsPlace()")), qPrintable(out.right(3000)));
        const QRegularExpressionMatch m = QRegularExpression(a("fontsBackKeepsPlace: font ([0-9]+) px, device pixel ratio ([0-9.]+)")).match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(m.captured(0)));
        if (name == a("QT_FONT_DPI"))
            QVERIFY(m.captured(1).toInt() >= 21);
        else
            QVERIFY(m.captured(2).toDouble() >= 1.9);
    }

    // ---- 2.7.2 (D5, D9): a class's whole format (also one the sheet doesn't colour), the text style a map gives back, and
    // a "no colour" target for the info panel's classes only ---------------------------------------------------------------
    void classFormatsFonts()
    {
        const darkpalette::Tokens& k     = darkpalette::tokens();
        const auto                 dark  = darkchat::classFormats(darkpalette::chatSheet(k), k.link);
        const auto                 plain = darkchat::classFormats(QString::fromLatin1(kPlainChat), QColor(0x1c, 0x82, 0xcc));
        const auto                 find  = [](const QVector<darkchat::ClassFormat>& list, const char* key) {
            for (const darkchat::ClassFormat& c : list) {
                if (c.key == key)
                    return c;
            }
            return darkchat::ClassFormat();
        };
        // Every probe yields an entry; the coloured ones are classLooks.
        QCOMPARE(plain.size(), dark.size());
        int colored = 0;
        for (const darkchat::ClassFormat& c : plain)
            colored += c.colored ? 1 : 0;
        QCOMPARE(colored, darkchat::classLooks(QString::fromLatin1(kPlainChat), QColor(0x1c, 0x82, 0xcc)).size());
        const darkchat::ClassFormat userPlain = find(plain, "UserLink");
        QVERIFY(userPlain.colored);
        QVERIFY(!userPlain.format.hasProperty(QTextFormat::FontWeight));
        QVERIFY(userPlain.format.fontUnderline());
        const darkchat::ClassFormat userDark = find(dark, "UserLink");
        QCOMPARE(userDark.format.fontWeight(), int(QFont::Bold));
        // The way back gives a name printed while dark the skin's style: no weight, Qt's underline.
        const darkchat::ColorMap back = darkchat::buildMap(dark, plain, {}, QColor());
        const auto               fix  = back.fonts.value(userDark.look);
        QStringList              ids;
        for (const auto& p : fix)
            ids << QString::fromLatin1("%1=%2").arg(p.id, 0, 16).arg(p.value.isValid() ? p.value.toString() : a("none"));
        QVERIFY2(ids.contains(QString::fromLatin1("%1=none").arg(int(QTextFormat::FontWeight), 0, 16)), qPrintable(ids.join(a(", "))));
        QTextCharFormat name = userDark.format;
        name.setAnchorHref(a("client://3/x~Kai"));
        QVERIFY(darkchat::mapBack(name, back));
        name.clearProperty(QTextFormat::AnchorHref);
        QTextCharFormat wantName = userPlain.format;
        wantName.clearProperty(QTextFormat::AnchorHref);
        QCOMPARE(describe(name), describe(wantName));
        // Time and Normal share a dark look; their skin weights disagree on TeamSpeak's skin (normal vs none): no weight fix.
        const auto skin    = darkchat::classFormats(QString::fromLatin1(kSkinChat), QColor(0x1c, 0x82, 0xcc));
        const auto skinMap = darkchat::buildMap(dark, skin, {}, QColor());
        for (const auto& p : skinMap.fonts.value(find(dark, "Time").look))
            QVERIFY(p.id != QTextFormat::FontWeight);
        // ... and agree on the colours-only skin (none): the dark sheet's explicit normal goes.
        bool cleared = false;
        for (const auto& p : back.fonts.value(find(dark, "Time").look))
            cleared = cleared || (p.id == QTextFormat::FontWeight && !p.value.isValid());
        QVERIFY(cleared);
        // A chat's map has the colours of 2.7.1's (classLooks), the same looks.
        const darkchat::ColorMap old = darkchat::buildMap(darkchat::classLooks(darkpalette::chatSheet(k), k.link),
                                                          darkchat::classLooks(QString::fromLatin1(kPlainChat), QColor(0x1c, 0x82, 0xcc)), {}, QColor());
        QCOMPARE(back.exact, old.exact);
        QVERIFY(back.uncolored.isEmpty());
    }

    void classFormatsKnowNoColour()
    {
        const darkpalette::Tokens& k     = darkpalette::tokens();
        const QString              sheet = teamSpeakLikeChat();
        const auto                 skin  = darkchat::classFormats(sheet, QColor(0x1c, 0x82, 0xcc));
        const auto                 dark  = darkchat::classFormats(darkpalette::chatSheet(k), k.link);
        darkchat::ClassFormat      label, darkLabel, host;
        for (const darkchat::ClassFormat& c : skin) {
            if (c.key == "Label")
                label = c;
            if (c.key == "HostName")
                host = c;
        }
        for (const darkchat::ClassFormat& c : dark) {
            if (c.key == "Label")
                darkLabel = c;
        }
        QCOMPARE(label.key, QByteArray("Label"));
        QVERIFY(!label.colored);
        QCOMPARE(label.format.fontWeight(), int(QFont::Bold));
        QVERIFY(darkLabel.colored);
        QCOMPARE(QColor::fromRgba(darkLabel.look.rgb), k.secondary);
        QVERIFY(darkLabel.look == host.look ? false : true); // the host name's look on the skin side differs (its colour)
        // The info panel's map: the label's dark look goes to "no colour"; the chats' map gives it the host name's colour.
        const darkchat::ColorMap info = darkchat::buildMap(dark, skin, {}, QColor(), {}, true);
        QVERIFY(info.uncolored.contains(darkLabel.look));
        QVERIFY(!info.exact.contains(darkLabel.look));
        const darkchat::ColorMap chat = darkchat::buildMap(dark, skin, {}, QColor());
        QVERIFY(chat.uncolored.isEmpty());
        QCOMPARE(QColor::fromRgba(chat.exact.value(darkLabel.look)), QColor(0x33, 0x59, 0x7d));
        QTextCharFormat cell = darkLabel.format;
        QVERIFY(darkchat::mapBack(cell, info));
        QVERIFY(!cell.hasProperty(QTextFormat::ForegroundBrush));
        QCOMPARE(cell.fontWeight(), int(QFont::Bold));
    }

    // ---- 2.7.2 (D9): an info panel TeamSpeak rendered while dark mode was on shows exactly what TeamSpeak renders in the skin
    // after off and at the unload: its labels without a colour where the skin sets none (2.7.1: the host name's colour), its
    // title, custom nickname and value classes as the skin has them; the chats' host names keep their way back ----------
    void infoPanelWhileDark_data()
    {
        QTest::addColumn<QString>("sheet");
        QTest::addColumn<bool>("unload");
        const QVector<QPair<const char*, QString>> skins{{"TeamSpeak's skin", teamSpeakLikeChat()},
                                                         {"colours only", QString::fromLatin1(kPlainChat)},
                                                         {"community dark", QString::fromLatin1(kCommunityChat)}};
        for (const auto& skin : skins) {
            for (const bool unload : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1("%1, %2").arg(QString::fromLatin1(skin.first), unload ? a("unload") : a("off"))))
                    << skin.second << unload;
        }
    }
    void infoPanelWhileDark()
    {
        QFETCH(QString, sheet);
        QFETCH(bool, unload);
        installSkin();
        TsWindow t = shownWindow(3, 0, false);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(sheet);
        // A client's panel as TeamSpeak's template writes it (made-up values).
        const QString html = a("<p class=\"InfoFrame_Title\"><a class=\"TextMessage_UserLink\" href=\"client://3/z~Dex\">Dex</a> "
                               "<span class=\"InfoFrame_CustomNick\">(Dexter)</span></p>"
                               "<table class=\"InfoFrame_Table\"><tr><td class=\"Bottom Label\">Version:</td><td>3.6.2 on Windows</td></tr>"
                               "<tr><td class=\"Label\">Online since:</td><td class=\"Highlight\">12 minutes</td></tr>"
                               "<tr><td class=\"Label Space-Top\">Status:</td><td class=\"Important\">Away</td></tr>"
                               "<tr><td class=\"Label\">Talk power:</td><td class=\"Active\">granted</td></tr></table>");
        QTextDocument ref;
        ref.setDefaultStyleSheet(darkchat::resolvePaletteRefs(sheet, QApplication::palette()));
        ref.setHtml(html);
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        t.info->setHtml(html); // TeamSpeak renders the panel again on a selection
        appendHtml(t.visible, styledLines().at(3)); // a host name in a chat
        pump(30);
        QVERIFY(colorAt(t.info->document(), findText(t.info->document(), a("Status:"))) == darkpalette::tokens().secondary);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        const QStringList got  = fragmentsFrom(t.info->document(), 0);
        const QStringList want = fragmentsFrom(&ref, 0);
        QVERIFY2(got == want, qPrintable(firstListDiff(got, want)));
        QVERIFY(!formatAt(t.info->document(), findText(t.info->document(), a("Status:"))).hasProperty(QTextFormat::ForegroundBrush));
        QVERIFY(darkLeftovers(t.info->document()).isEmpty());
        // The chats are unchanged: a host name comes back in the skin's colour, or (2.7.3, X1) its line's where it has none.
        QTextDocument chatRef;
        chatRef.setDefaultStyleSheet(sheet);
        appendTo(&chatRef, styledLines().at(3));
        QCOMPARE(describe(formatAt(t.visible->document(), findText(t.visible->document(), a("example.org")))),
                 describe(formatAt(&chatRef, findText(&chatRef, a("example.org")))));
    }

    // ---- 2.7.2 (D6): no dark-mode bookkeeping (kOriginal) is left on any format of any chat, info panel or poke window after
    // off or the unload: not on the blocks TeamSpeak appended while dark (QTextEdit::append copies its cursor's format into
    // them), not in the chat cursor's own format (it copies that into every later line), not on an HD emoji's picture
    // (ChatEmoji merges the recoloured character's format into it), not on the chat layout's recoloured fragments, not in
    // documents TeamSpeak swapped out; 2 lines appended after off carry none either ------------------------------------------
    void noBookkeepingLeft_data()
    {
        QTest::addColumn<int>("way");
        QTest::newRow("off") << 0;
        QTest::newRow("unload with the window shown") << 1;
        QTest::newRow("Cozy, off, then TeamSpeak classic") << 2;
    }
    void noBookkeepingLeft()
    {
        QFETCH(int, way);
        installSkin();
        TsWindow t = shownWindow(30, 30);
        // The hidden tab shows a document TeamSpeak swaps later (both owned here).
        auto first  = std::make_unique<QTextDocument>();
        auto second = std::make_unique<QTextDocument>();
        for (QTextDocument* d : {first.get(), second.get()})
            d->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        t.hidden->setDocument(first.get());
        appendLines(t.hidden, someLines(20, false));
        QTextDocument* doc = t.visible->document();
        if (way == 2)
            fillLayoutChat(doc, tschat::lightSkin()); // lines the layout styles
        // A TS Media warning line before dark mode (ts3api's printWarning: the bold label with U+26A0, in the plugin blue).
        appendHtml(t.visible, a("<p><span style=\"color:#00008b\"><b>TS Media chat \u26a0</b> Upload failed.</span></p>"));
        const int oldBlocks = doc->blockCount();
        t.visible->moveCursor(QTextCursor::End); // TeamSpeak's chat cursor sits at the end
        t.hidden->moveCursor(QTextCursor::End);
        // The poke window's text: a nickname in TeamSpeak's blue.
        auto* poke = new QDialog;
        m_windows << poke;
        poke->setObjectName(a("PokeDialog"));
        auto* pokeText = new QTextBrowser(poke);
        pokeText->setObjectName(a("msgTextBrowser"));
        (new QVBoxLayout(poke))->addWidget(pokeText);
        pokeText->setHtml(a("<p><a href=\"client://4/q~Ola\" style=\"color:#0000ff\">Ola</a> poked you: <b>look at the map</b></p>"));
        poke->show();
        pump(30);
        const QString oldVisible = formatsOfBlocks(doc, 0, oldBlocks - 1);

        auto dark = makeDark();
        dark->setEnabled(true);
        pump(100); // the hidden chat by the slice timer
        QVERIFY(darkchat::taggedCount(first.get()) > 0);
        QVERIFY(darkchat::taggedCount(pokeText->document()) > 0);
        // While dark: the warning's U+26A0 becomes an HD emoji's picture as ChatEmoji makes it (the recoloured character's
        // format merged into the picture's, the text format kept).
        {
            layoutformat::MutatingScope scope;
            QTextCursor                 c = doc->find(QString(QChar(0x26a0)));
            QVERIFY(!c.isNull());
            const QTextCharFormat glyph = c.charFormat();
            QVERIFY(glyph.hasProperty(darkchat::kOriginal));
            QTextImageFormat image;
            image.merge(glyph);
            image.setObjectType(QTextFormat::ImageObject);
            image.setProperty(emojiformat::kOriginalText, QString(QChar(0x26a0)));
            image.setProperty(emojiformat::kOriginalFormat, QVariant::fromValue<QTextFormat>(glyph));
            image.setName(a("tsmedia-emoji:26a0@16"));
            image.setWidth(16);
            image.setHeight(16);
            c.insertText(QString(QChar::ObjectReplacementCharacter), image);
        }
        // TeamSpeak's lines (QTextEdit::append, the cursor at the end on a recoloured fragment).
        for (int i = 0; i < 5; ++i)
            t.visible->append(chatLine(a("22:00:00"), a("TextMessage_ClientJoined"), user(a("Ivo")) + a(" entered your channel")));
        for (int i = 0; i < 3; ++i)
            t.hidden->append(chatLine(a("22:00:10"), a("TextMessage_ClientLeft"), user(a("Uma")) + a(" left your channel")));
        t.info->setHtml(a("<p class=\"InfoFrame_Title\"><a class=\"TextMessage_UserLink\" href=\"client://3/z~Dex\">Dex</a></p>"
                          "<table class=\"InfoFrame_Table\"><tr><td class=\"Label\">Version:</td><td>3.6.2</td></tr></table>"));
        pokeText->append(a("<p><a href=\"client://4/q~Ola\" style=\"color:#0000ff\">Ola</a> poked you again</p>"));
        pump(30);
        // TeamSpeak swaps the hidden chat's document (a reconnect's history), the new one filled.
        t.hidden->setDocument(second.get());
        appendLines(t.hidden, someLines(10, false));
        dark->checkDocuments();
        t.hidden->moveCursor(QTextCursor::End);
        for (int i = 0; i < 2; ++i)
            t.hidden->append(chatLine(a("22:00:20"), a("TextMessage_ClientLeft"), user(a("Uma")) + a(" left your channel")));
        pump(30);
        if (way == 2) {
            layoutformat::LayoutEditScope editing;
            QVERIFY(layoutdoc::styleAll(doc, layoutEnv(true)) >= 3);
        }
        // Without the fix there is something to find.
        QVERIFY(!darkLeftovers(doc).isEmpty());
        if (way == 1)
            dark->shutdown();
        else
            dark->setEnabled(false);
        if (way == 2) {
            layoutformat::LayoutEditScope editing;
            layoutdoc::restoreAll(doc);
        }
        const auto check = [&](const char* when) {
            const QVector<QPair<const char*, QTextDocument*>> docs{{"visible chat", doc},        {"hidden chat", second.get()},
                                                                   {"swapped out", first.get()}, {"info panel", t.info->document()},
                                                                   {"poke window", pokeText->document()}};
            for (const auto& d : docs) {
                const QStringList left = darkLeftovers(d.second);
                QVERIFY2(left.isEmpty(), qPrintable(QString::fromLatin1("%1, %2: %3").arg(QString::fromLatin1(when), QString::fromLatin1(d.first), left.mid(0, 6).join(a("; ")))));
            }
            for (QTextEdit* view : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info, static_cast<QTextEdit*>(pokeText)})
                QVERIFY2(!view->currentCharFormat().hasProperty(darkchat::kOriginal), qPrintable(QString::fromLatin1(when) + a(": ") + view->objectName()));
        };
        check("after the way back");
        // The glyph's picture keeps the warning's text format of the skin (what the emoji module gives back).
        // TeamSpeak's next lines after off copy nothing of dark mode.
        for (int i = 0; i < 2; ++i) {
            t.visible->append(chatLine(a("22:10:00"), a("TextMessage_ClientJoined"), user(a("Ivo")) + a(" entered your channel")));
            t.hidden->append(chatLine(a("22:10:00"), a("TextMessage_ClientLeft"), user(a("Uma")) + a(" left your channel")));
            pokeText->append(a("<p>and again</p>"));
        }
        check("2 lines after");
        if (way != 2) // (Cozy: the layout's restore gives the lines back in their own formats; compared by layoutAcrossToggle)
            QVERIFY2(formatsOfBlocks(doc, 0, oldBlocks - 1) == oldVisible, qPrintable(firstDiff(formatsOfBlocks(doc, 0, oldBlocks - 1), oldVisible)));
        t.hidden->setDocument(nullptr);
    }

    // ---- T-9i (2.7.1): a custom skin with a picture as the chat's background (dark here, which the palette doesn't know): the
    // lines printed while dark mode was on get the colours TS Media prints on that picture (the sampled background) ---------
    void pictureSkinOwnLines()
    {
        const auto    restoreLines = qScopeGuard([] { ts3::setLineColors(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString())); });
        const QString png          = m_dir->filePath(a("chatbg.png"));
        QImage        picture(64, 64, QImage::Format_RGB32);
        picture.fill(QColor(0x2a, 0x1a, 0x1a));
        QVERIFY(picture.save(png));
        const QString skinSheet = QString::fromLatin1(kSkinSheet) + QString::fromLatin1("QTextEdit#ChatTab { background-image: url(%1); color: #d8dee9; }\n").arg(png);
        qApp->setStyleSheet(skinSheet);
        TsWindow                 t    = shownWindow(3, 0, false);
        QTextDocument*           doc  = t.visible->document();
        const chatcolors::Colors skin = skinColorsOf(t.visible);
        QVERIFY(skin.dark && skin.sampled); // the picture's colour, not the palette's light Base
        QVERIFY(!chatcolors::fromPalette(t.visible->palette()).dark);
        for (const char* name : {"", "blue"}) {
            const QString choice               = QString::fromLatin1(name);
            Settings::instance().chatLineColor = choice;
            const QString want                 = ownLinesReference(QString::fromLatin1(kSkinChat), skin, choice);
            auto          dark                 = makeDark();
            dark->setEnabled(true);
            const int from = appendOwnLines(doc, darkChatColors(), choice);
            pump(30);
            dark->setEnabled(false);
            QCOMPARE(qApp->styleSheet(), skinSheet);
            QVERIFY2(charColors(doc, from) == want, qPrintable(QString::fromLatin1(name) + QLatin1Char(' ') + firstLineDiff(charColors(doc, from), want)));
            const chatcolors::LineColors lines = chatcolors::lineColors(skin, choice);
            QCOMPARE(colorAt(doc, findText(doc, a("TS Media chat"), doc->findBlockByNumber(from).position())).name(), lines.accent);
            QCOMPARE(colorAt(doc, findText(doc, a("Sent a.png"), doc->findBlockByNumber(from).position())).name(), lines.body);
        }
    }

    // ---- T-10: hidden chats ------------------------------------------------------------------------------------------------
    void hiddenChats()
    {
        installSkin();
        TsWindow       t      = shownWindow(10, 200);
        QTextDocument* hidden = t.hidden->document();
        const QString  before = formatsOf(hidden);
        {
            // Off before the slice ran: a chat never recoloured is left as it is.
            auto dark = makeDark();
            dark->setEnabled(true);
            QVERIFY(dark->isPending(hidden));
            QCOMPARE(darkchat::taggedCount(hidden), 0);
            dark->setEnabled(false);
            QCOMPARE(formatsOf(hidden), before);
        }
        auto dark = makeDark();
        dark->setEnabled(true);
        QVERIFY(dark->isPending(hidden));
        // Shown before the slice: recoloured at its Show, before its first paint.
        PaintProbe probe;
        probe.check = [hidden] { return darkchat::taggedCount(hidden) > 0; };
        t.hidden->viewport()->installEventFilter(&probe);
        t.tabs->setCurrentIndex(1);
        QTRY_VERIFY(probe.paints > 0);
        QVERIFY(probe.okAtFirst);
        QVERIFY(!dark->isPending(hidden));
        t.hidden->viewport()->removeEventFilter(&probe);
        dark->setEnabled(false);
        QCOMPARE(formatsOf(hidden), before);
        // The slice timer reaches a chat that stays hidden.
        t.tabs->setCurrentIndex(0);
        pump(20);
        dark->setEnabled(true);
        QVERIFY(dark->isPending(hidden));
        pump(60);
        QVERIFY(!dark->isPending(hidden));
        QVERIFY(darkchat::taggedCount(hidden) > 0);
        dark->setEnabled(false);
        QCOMPARE(formatsOf(hidden), before);
    }

    // ---- live test fix (2.7.3): a hidden chat shown after off is the style's ----------------------------------------------------
    // Once the chat layer runs, off leaves a hidden chat whose layout waits for its Show (a chat made while dark mode was on,
    // such as a private chat that came in; one resized while it was on, such as after a style pick) with its place waiting:
    // dark mode's filter on that chat until its Show. That Show tracked the chat again (dark mode's sheet and its colours on
    // every line, also the lines after off: cream names and gray times on the white chat) and kept it until the next on/off,
    // whose off gave it a stale sheet. 2.7.2 the same. Now the Show after off puts its place back and nothing else.
    void hiddenChatShownAfterOff_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("a chat made while dark mode is on") << a("made");
        QTest::newRow("a chat resized while dark mode is on") << a("resized");
        QTest::newRow("a chat never shown before dark mode") << a("never");
    }
    void hiddenChatShownAfterOff()
    {
        QFETCH(QString, how);
        installSkin();
        TsWindow t = shownWindow(60, 300);
        if (how == a("resized")) { // laid out once, as TeamSpeak's tabs are after a while
            t.tabs->setCurrentIndex(1);
            pump(30);
            t.tabs->setCurrentIndex(0);
            pump(30);
        }
        // The style's look of the lines, never under dark mode (the reference).
        QTextBrowser reference;
        reference.document()->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        const auto colorsOf = [](QTextDocument* doc) {
            QStringList out;
            for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat f = it.fragment().charFormat();
                    out << it.fragment().text().left(8) + QLatin1Char('=')
                               + (f.hasProperty(QTextFormat::ForegroundBrush) ? f.foreground().color().name() : a("-"));
                }
            }
            return out;
        };
        appendLines(&reference, someLines(300, true)); // under the style (a link takes the palette's link colour then)
        auto dark = makeDark();
        dark->setDeferHiddenPlaces(true); // as once the chat layer runs (plugin.cpp)
        dark->setEnabled(true);
        pump(30);
        QTextBrowser* chat = t.hidden;
        if (how == a("made")) {
            // TeamSpeak makes the chat with its style's sheet and its tab, the chat layer reports it (chatsChanged), and
            // its lines come in while it stays hidden.
            chat = new QTextBrowser;
            chat->setObjectName(a("ChatTab"));
            chat->setOpenLinks(false);
            chat->document()->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
            t.tabs->addTab(chat, a("Private"));
            dark->checkDocuments();
            pump(20);
            QVERIFY(dark->isTracked(chat->document()));
            appendLines(chat, someLines(300, true));
        } else if (how == a("resized")) { // a new size while hidden (its stack lays out only the shown page)
            chat->resize(chat->width() + 60, chat->height() + 80);
        }
        pump(60);
        QVERIFY(!chat->isVisible());
        QVERIFY(chat->testAttribute(Qt::WA_PendingResizeEvent));
        dark->setEnabled(false);
        pump(30);
        QVERIFY(dark->placeWaits(chat)); // its place waits for its Show
        QCOMPARE(dark->stats().tracked, 0);
        t.tabs->setCurrentWidget(chat);
        pump(60);
        QVERIFY(!dark->placeWaits(chat));
        QCOMPARE(dark->stats().tracked, 0);
        QVERIFY(!dark->isTracked(chat->document()));
        QCOMPARE(chat->document()->defaultStyleSheet(), QString::fromLatin1(kSkinChat));
        QVERIFY2(darkLeftovers(chat->document()).isEmpty(), qPrintable(darkLeftovers(chat->document()).mid(0, 5).join(a("; "))));
        QCOMPARE(colorsOf(chat->document()), colorsOf(reference.document()));
        // A line after off is the style's too.
        appendLines(chat, someLines(2, true));
        appendLines(&reference, someLines(2, true));
        pump(20);
        QCOMPARE(colorsOf(chat->document()), colorsOf(reference.document()));
        // And the next on and off give it the style's sheet back (it kept a stale one).
        dark->setEnabled(true);
        pump(30);
        QVERIFY(dark->isTracked(chat->document()));
        dark->setEnabled(false);
        pump(30);
        QCOMPARE(dark->stats().tracked, 0);
        QCOMPARE(chat->document()->defaultStyleSheet(), QString::fromLatin1(kSkinChat));
        QCOMPARE(colorsOf(chat->document()), colorsOf(reference.document()));
    }

    // ---- T-11: every chat's place --------------------------------------------------------------------------------------------
    void placesKept()
    {
        // In a child process with Windows' fonts (placesKeptScaled): TeamSpeak's 8.25 pt, at the DPI QT_FONT_DPI asks for
        // (this executable assumes 96 DPI for fonts, TeamSpeak doesn't: its font grows with QT_FONT_DPI).
        const QFont savedFont = QApplication::font();
        if (!qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
            QFont font(a("Segoe UI"));
            font.setPixelSize(qRound(8.25 * qEnvironmentVariableIntValue("QT_FONT_DPI") / 72.0));
            if (font.pixelSize() <= 0)
                font.setPixelSize(11);
            QApplication::setFont(font);
        }
        const auto restoreFont = qScopeGuard([&savedFont] { QApplication::setFont(savedFont); });
        installSkin();
        TsWindow t = shownWindow(400, 1500);
        // The hidden chat at 40 %, the visible one in the middle (wrapping lines).
        t.tabs->setCurrentIndex(1);
        pump(30);
        t.hidden->verticalScrollBar()->setValue(t.hidden->verticalScrollBar()->maximum() * 2 / 5);
        pump(30);
        t.tabs->setCurrentIndex(0);
        pump(30);
        t.visible->verticalScrollBar()->setValue(t.visible->verticalScrollBar()->maximum() / 2);
        pump(30);
        auto dark = makeDark();
        qInfo("placesKept: font %d px, size factor %.2f, device pixel ratio %.2f", QFontInfo(QApplication::font()).pixelSize(),
              darkpalette::scaleFactor(QApplication::font()), t.window->devicePixelRatioF());
        for (int round = 0; round < 2; ++round) {
            const chatscroll::ReaderPlace visible = chatscroll::captureReader(t.visible);
            const chatscroll::ReaderPlace hidden  = chatscroll::captureReader(t.hidden);
            QVERIFY(!visible.bottom && !visible.cursor.isNull());
            QVERIFY(!hidden.bottom && !hidden.cursor.isNull());
            dark->setEnabled(round == 0);
            pump(30);
            QVERIFY2(qAbs(drift(t.visible, visible)) <= 1, qPrintable(QString::number(drift(t.visible, visible))));
            t.tabs->setCurrentIndex(1);
            pump(30);
            QVERIFY2(qAbs(drift(t.hidden, hidden)) <= 1, qPrintable(QString::number(drift(t.hidden, hidden))));
            t.tabs->setCurrentIndex(0);
            pump(30);
        }
        // At its end, the chat stays at its end.
        t.visible->verticalScrollBar()->setValue(t.visible->verticalScrollBar()->maximum());
        pump(20);
        for (const bool on : {true, false}) {
            QVERIFY(chatscroll::atBottom(t.visible));
            dark->setEnabled(on);
            pump(30);
            QVERIFY(chatscroll::atBottom(t.visible));
        }
        // Review fix: toggles once the chat layer runs (plugin.cpp). A chat never shown yet (Qt lays it out at its first Show,
        // for its real size) isn't laid out by a toggle; its place waits for its Show, also over two toggles, and the shutdown
        // places it at once.
        dark->setDeferHiddenPlaces(true);
        const auto freshChat = [&t](const QString& name) {
            auto* b = new QTextBrowser;
            b->setObjectName(a("ChatTab"));
            b->setOpenLinks(false);
            b->document()->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
            appendLines(b, someLines(800, true));
            t.tabs->addTab(b, name);
            return b;
        };
        for (int toggles = 1; toggles <= 3; ++toggles) {
            QTextBrowser* fresh = freshChat(QString::fromLatin1("Fresh %1").arg(toggles));
            pump(20);
            QVERIFY(!fresh->isVisible() && fresh->testAttribute(Qt::WA_PendingResizeEvent));
            const chatscroll::ReaderPlace place = chatscroll::captureReader(fresh);
            QVERIFY(place.bottom); // never shown: at its end
            for (int i = 0; i < qMin(toggles, 2); ++i) {
                dark->setEnabled(!dark->enabled());
                pump(30);
                QCOMPARE(dark->deferredPlaces(), 1);
                QVERIFY(fresh->testAttribute(Qt::WA_PendingResizeEvent)); // not laid out by the toggle
            }
            if (toggles == 3) {
                // Laid out for its size and placed now, no filter of ours left on it (what Qt does at its later Show is
                // TeamSpeak's again, as before 2.7's fix).
                dark->shutdown();
                QCOMPARE(dark->deferredPlaces(), 0);
                t.tabs->setCurrentWidget(fresh);
                pump(30);
                QCOMPARE(dark->deferredPlaces(), 0);
                t.tabs->setCurrentIndex(0);
                pump(30);
                continue;
            }
            t.tabs->setCurrentWidget(fresh);
            pump(30);
            QCOMPARE(dark->deferredPlaces(), 0);
            QVERIFY2(qAbs(drift(fresh, place)) <= 1, qPrintable(QString::fromLatin1("toggles %1: drift %2, scroll %3/%4")
                                                                    .arg(toggles)
                                                                    .arg(drift(fresh, place))
                                                                    .arg(fresh->verticalScrollBar()->value())
                                                                    .arg(fresh->verticalScrollBar()->maximum())));
            t.tabs->setCurrentIndex(0);
            pump(30);
        }
    }

    // The same at 200 % (font DPI and Qt's own scaling), each in a child process (the scale is per process).
    void placesKeptScaled_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("value");
        QTest::newRow("100%") << QString() << QString();
        QTest::newRow("QT_FONT_DPI=192") << a("QT_FONT_DPI") << a("192");
        QTest::newRow("QT_SCALE_FACTOR=2") << a("QT_SCALE_FACTOR") << a("2");
    }

    void placesKeptScaled()
    {
        QFETCH(QString, name);
        QFETCH(QString, value);
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(a("QT_FONT_DPI"));
        env.remove(a("QT_SCALE_FACTOR"));
        if (!name.isEmpty())
            env.insert(name, value);
        env.insert(a("QT_QPA_FONTDIR"), QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR", a("C:/Windows"))) + a("/Fonts"));
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        const QString results = m_dir->filePath(a("places.txt")); // the test log goes to a file (-o), the console gets none
        child.start(QCoreApplication::applicationFilePath(), {a("-platform"), a("offscreen"), a("-o"), results + a(",txt"), a("placesKept")});
        QVERIFY(child.waitForFinished(120000));
        QFile         file(results);
        const QString out = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString::fromLocal8Bit(child.readAll());
        QVERIFY2(child.exitCode() == 0, qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::placesKept()")), qPrintable(out.right(3000)));
        // It really ran at 200 %: the font twice as large, or Qt's own scale 2.
        const QRegularExpressionMatch m = QRegularExpression(a("placesKept: font ([0-9]+) px, size factor ([0-9.]+), device pixel ratio ([0-9.]+)")).match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(m.captured(0)));
        if (name == a("QT_FONT_DPI"))
            QVERIFY(m.captured(2).toDouble() >= 1.9);
        else if (name == a("QT_SCALE_FACTOR"))
            QVERIFY(m.captured(3).toDouble() >= 1.9);
        else
            QCOMPARE(m.captured(1).toInt(), 11);
    }

    // ---- T-12: unload --------------------------------------------------------------------------------------------------------
    void unload()
    {
        darktitlebar::setSetAttributeForTests(fakeSet);
        installSkin();
        TsWindow       t       = shownWindow(30, 30);
        QDialog*       dialog  = buildDialog();
        m_windows << dialog;
        dialog->show();
        pump(30);
        const QString  sheet   = qApp->styleSheet();
        const QPalette palette = QApplication::palette();
        const QString  formats = formatsOf(t.visible->document());
        const int      before  = Receivers::of(t.visible->document());
        auto*          dark    = new DarkMode;
        dark->setGlyphBase(m_dir->path());
        dark->setEnabled(true);
        pump(50);
        QVERIFY(Receivers::of(t.visible->document()) > before);
        const int bars = dark->stats().titleBars;
        QVERIFY(bars >= 2);
        g_dwm.clear();
        dark->shutdown();
        QVERIFY(!DarkMode::isActive());
        QCOMPARE(qApp->styleSheet(), sheet);
        QVERIFY(samePalette(QApplication::palette(), palette));
        QCOMPARE(formatsOf(t.visible->document()), formats);
        QCOMPARE(dark->timersActive(), 0);
        QVERIFY(!dark->hasEventFilter());
        QCOMPARE(Receivers::of(t.visible->document()), before);
        int reverted = 0;
        for (const DwmCall& c : qAsConst(g_dwm))
            reverted += c.attribute == 20 && c.value == 0 ? 1 : 0;
        QCOMPARE(reverted, bars);
        // Nothing of ours reacts afterwards.
        qApp->setStyleSheet(QString::fromLatin1(kOtherSheet));
        pump(30);
        QCOMPARE(qApp->styleSheet(), QString::fromLatin1(kOtherSheet));
        QCOMPARE(dark->stats().adoptions, 0);
        dark->shutdown(); // twice
        dark->setEnabled(false);
        delete dark;
        pump(10);
        QCOMPARE(qApp->styleSheet(), QString::fromLatin1(kOtherSheet));
    }

    // ---- T-11b (live test): the chat's scale pass a theme switch starts (its StyleChange) took the reader's place in the
    // middle of the switch; dark mode puts the place back afterwards and says so (placesRestored), the pass keeps that one.
    // Live at 200 %: off, inside a wrapped message, the chat jumped 12 messages up 200 ms after the switch -----------------
    void scalePassKeepsRestoredPlace()
    {
        TsWindow     t      = shownWindow(400, 0);
        auto         dark   = makeDark();
        int          placed = 0;
        QObject::connect(dark.get(), &DarkMode::placesRestored, [&placed] { ++placed; });
        ChatScale::Host host;
        auto*           scaler = new ChatScale(t.visible, host, t.window);
        QObject::connect(dark.get(), &DarkMode::placesRestored, scaler, &ChatScale::retake);
        QScrollBar* bar = t.visible->verticalScrollBar();
        for (const bool on : {true, false}) {
            bar->setValue(bar->maximum() / 3);
            scaler->request(); // the switch's StyleChange: a pass begins with the place of that moment
            bar->setValue(bar->maximum() / 2); // where the reader really is (what dark mode captures and puts back)
            pump(5);
            const chatscroll::ReaderPlace want = chatscroll::captureReader(t.visible);
            dark->setEnabled(on);
            QCOMPARE(placed, on ? 1 : 2);
            scaler->settle();
            QVERIFY2(qAbs(drift(t.visible, want)) <= 2, qPrintable(QString::number(drift(t.visible, want))));
        }
    }

    // ---- T-11c (review fix): a skin with another chat font picked while dark mode is on (Options -> Design -> Apply). The
    // chat's scale pass takes the reader's place at the chat's FontChange, before its document gets the font. Dark mode takes
    // the skin over on its own turn, when the chat already shows other lines at the same scroll value. Which of that turn and
    // the pass's first hold comes first follows Qt's repolish order (a hash order; dark mode's timer restarts at every
    // window's event): here the hold is held back by its spacing (one ran a moment ago), so dark mode's turn comes first.
    // Dark mode hands a pass only the places it took before anything changed (on and off): the pass keeps its own here ------
    void skinChangeKeepsScalePassPlace()
    {
        installSkin();
        TsWindow t    = shownWindow(400, 0);
        auto     dark = makeDark();
        int      placed = 0;
        QObject::connect(dark.get(), &DarkMode::placesRestored, [&placed] { ++placed; });
        // ChatIntegration's host (every FontChange or StyleChange asks for a pass) and its retakeScalePlaces.
        auto            holder = std::make_shared<QPointer<ChatScale>>();
        ChatScale::Host host;
        host.fontEvent = [holder] {
            if (*holder)
                (*holder)->request();
        };
        auto* scaler = new ChatScale(t.visible, host, t.window);
        *holder      = scaler;
        QObject::connect(dark.get(), &DarkMode::placesRestored, scaler, [scaler] {
            if (scaler->pending())
                scaler->retake();
        });
        auto down = qScopeGuard([holder] {
            if (*holder)
                (*holder)->prepareShutdown();
        });
        dark->setEnabled(true);
        QCOMPARE(placed, 1);
        pump(400); // the pass the switch started has settled
        QVERIFY(!scaler->pending());
        QScrollBar* bar = t.visible->verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        pump(5);
        const chatscroll::Place before = chatscroll::capture(t.visible);
        QVERIFY(!before.bottom);
        const int value = bar->value();
        const int holds = scaler->holds();
        scaler->request(); // a hold now (the next one waits for its 100 ms spacing)...
        pump(10);
        QCOMPARE(scaler->holds(), holds + 1);
        scaler->settle(); // ...and its pass done: the reader where they were
        QVERIFY(qAbs(bar->value() - value) <= 1);
        const QFont font   = t.visible->font();
        const int   passes = scaler->passes();
        // TeamSpeak's Apply: the skin header's Link in the palette, then the skin's sheet with a larger chat font.
        QPalette p = QApplication::palette();
        p.setColor(QPalette::Link, QColor(0x20, 0x60, 0xa0));
        QApplication::setPalette(p);
        qApp->setStyleSheet(QString::fromLatin1(kSkinSheet) + a("QTextEdit#ChatTab { font-size: 22px; }\n"));
        pump(500);
        QCOMPARE(dark->stats().adoptions, 1);
        QVERIFY(DarkMode::isActive());
        QVERIFY(t.visible->font() != font); // the skin's chat font is in use (dark mode keeps the skin's fonts)
        QVERIFY(scaler->passes() > passes);
        QVERIFY(!scaler->pending());
        const qreal want = layoutdoc::anchorY(t.visible->document(), chatscroll::anchorNow(t.visible->document(), before));
        QVERIFY(want >= 0);
        QVERIFY2(qAbs(bar->value() - want) <= 2, qPrintable(QString::fromLatin1("scroll %1, the reader's place %2").arg(bar->value()).arg(want)));
        QCOMPARE(placed, 1); // nothing handed over: dark mode took its places after the skin had changed
        // Off: dark mode's place (taken before the switch) goes to a pass the switch started.
        dark->setEnabled(false);
        QCOMPARE(placed, 2);
    }

    // ---- T-12b (live test): TeamSpeak's Quit hides its windows, then lets the plugin go (before aboutToQuit). Putting the
    // skin back then repolished every half-destroyed window (a crash in the style sheet code): only our hooks go -------------
    void unloadAtQuit()
    {
        darktitlebar::setSetAttributeForTests(fakeSet);
        installSkin();
        TsWindow      t      = shownWindow(30, 30);
        const QString sheet  = qApp->styleSheet();
        const int     before = Receivers::of(t.visible->document());
        auto*         dark   = new DarkMode;
        dark->setGlyphBase(m_dir->path());
        dark->setEnabled(true);
        pump(50);
        QVERIFY(DarkMode::isActive());
        const QString ours = qApp->styleSheet();
        QVERIFY(ours != sheet);
        for (QWidget* top : QApplication::topLevelWidgets())
            top->hide(); // TeamSpeak's Quit: every window hidden first
        g_dwm.clear();
        dark->shutdown();
        QVERIFY(!DarkMode::isActive());
        QCOMPARE(qApp->styleSheet(), ours); // left as it is: no repolish of windows going away
        QVERIFY(g_dwm.isEmpty());           // nor title bar calls
        QCOMPARE(dark->timersActive(), 0);
        QVERIFY(!dark->hasEventFilter());
        QCOMPARE(Receivers::of(t.visible->document()), before);
        delete dark;
        // Nothing of ours reacts afterwards (the sheet is a heap copy: TeamSpeak may replace or keep it).
        qApp->setStyleSheet(QString::fromLatin1(kOtherSheet));
        pump(30);
        QCOMPARE(qApp->styleSheet(), QString::fromLatin1(kOtherSheet));
    }

    // ---- T-12c (review fix): at Quit a window other than the main one may still be shown (another plugin's window; one of
    // TeamSpeak's own: Qt doesn't hide a window's own windows with it). The main window hidden is the sign; a Disable or
    // Reload (in Options, over the shown main window) still puts everything back -----------------------------------------------
    void unloadAtQuitWithOtherWindows()
    {
        for (const bool quit : {true, false}) {
            QApplication::setPalette(m_savedPalette);
            installSkin();
            const QString skin = qApp->styleSheet();
            TsWindow      t    = shownWindow(30, 30);
            auto*         own  = new QDialog(t.window); // TeamSpeak's own window (Options), a window of the main window
            own->setObjectName(a("OptionsDialog"));
            own->show();
            auto* other = new QWidget; // another plugin's window
            other->setObjectName(a("OtherPluginWindow"));
            other->resize(200, 120);
            m_windows << other;
            other->show();
            pump(30);
            auto* dark = new DarkMode;
            dark->setGlyphBase(m_dir->path());
            dark->setEnabled(true);
            pump(50);
            QVERIFY(DarkMode::isActive());
            const QString ours = qApp->styleSheet();
            QVERIFY(ours != skin);
            if (quit)
                t.window->hide(); // TeamSpeak's Quit: its main window hidden first
            QVERIFY(own->isVisible());
            QVERIFY(other->isVisible());
            dark->shutdown();
            QVERIFY(!DarkMode::isActive());
            QCOMPARE(qApp->styleSheet(), quit ? ours : skin);
            QCOMPARE(dark->timersActive(), 0);
            QVERIFY(!dark->hasEventFilter());
            delete dark;
            delete other;
            delete t.window;
        }
    }

    // ---- T-D (2.7.1): dark mode says when TeamSpeak's look changed (themeChanged) once per on, off, skin taken over and the
    // off of a Disable or Reload, with the new look in place and inside its place bracket (what the chat layer changes then,
    // a picture growing above the reader here, keeps the reader's line); never at TeamSpeak's Quit ----------------------------
    void themeChangedInsideBracket()
    {
        installSkin();
        TsWindow t = shownWindow(300, 30);
        t.visible->verticalScrollBar()->setValue(t.visible->verticalScrollBar()->maximum() / 2);
        pump(30);
        int     emits  = 0;
        bool    active = false;
        QString sheet;
        QColor  base;
        int     sleepMs = 0;
        const auto receiver = [&] {
            ++emits;
            active = DarkMode::isActive();
            sheet  = qApp->styleSheet();
            base   = chatcolors::fromPalette(t.visible->palette()).base;
            // What the chat layer does then: a picture above the reader drawn at another size.
            QTextCursor      c(t.visible->document());
            QTextImageFormat grown;
            grown.setName(a("data:grown"));
            grown.setWidth(40);
            grown.setHeight(300);
            c.insertImage(grown);
            c.insertBlock();
            if (sleepMs > 0)
                QTest::qSleep(sleepMs);
        };
        const auto toggle = [&](DarkMode* d, bool on) {
            const chatscroll::ReaderPlace before = chatscroll::captureReader(t.visible);
            QVERIFY(!before.bottom);
            d->setEnabled(on);
            pump(30);
            QVERIFY2(qAbs(drift(t.visible, before)) <= 1, qPrintable(QString::number(drift(t.visible, before))));
        };
        auto dark = makeDark();
        QObject::connect(dark.get(), &DarkMode::themeChanged, receiver);
        toggle(dark.get(), true);
        QCOMPARE(emits, 1);
        QVERIFY(active);
        QCOMPARE(sheet, dark->appliedSheet());
        QVERIFY(chatcolors::isDark(base));
        dark->setEnabled(true); // on again: nothing
        QCOMPARE(emits, 1);
        sleepMs = 30;
        toggle(dark.get(), false);
        sleepMs = 0;
        QCOMPARE(emits, 2);
        QVERIFY(!active);
        QCOMPARE(sheet, QString::fromLatin1(kSkinSheet));
        QVERIFY(!chatcolors::isDark(base));
        QVERIFY2(dark->stats().lastCardsMs >= 25, qPrintable(QString::number(dark->stats().lastCardsMs))); // the receivers' time
        // A skin picked in Options -> Design while on: once more after its adoption, with the dark look again.
        toggle(dark.get(), true);
        QCOMPARE(emits, 3);
        qApp->setStyleSheet(QString::fromLatin1(kOtherSheet));
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(QString::fromLatin1(kOtherChat));
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        QCOMPARE(emits, 4);
        QVERIFY(active);
        // The plugin goes (Disable or Reload: TeamSpeak's main window shown): once, TeamSpeak's look back.
        dark->shutdown();
        QCOMPARE(emits, 5);
        QVERIFY(!active);
        QCOMPARE(sheet, QString::fromLatin1(kOtherSheet));
        dark.reset();
        // TeamSpeak's Quit: nothing put back, nothing said.
        auto quit = makeDark();
        QObject::connect(quit.get(), &DarkMode::themeChanged, receiver);
        quit->setEnabled(true);
        QCOMPARE(emits, 6);
        for (QWidget* top : QApplication::topLevelWidgets())
            top->hide();
        quit->shutdown();
        QCOMPARE(emits, 6);
    }

    // ---- T-13: title bars (a fake DwmSetWindowAttribute) ---------------------------------------------------------------------
    void titleBars()
    {
        darktitlebar::setSetAttributeForTests(fakeSet);
        auto dark = makeDark();
        dark->setEnabled(true);
        g_dwm.clear();
        auto* probe = new ShowProbe;
        m_windows << probe;
        probe->show();
        pump(20);
        const quintptr id = static_cast<quintptr>(probe->internalWinId());
        QVERIFY(id != 0);
        QVERIFY(probe->callsAtShow > 0); // before its showEvent
        const darkpalette::Tokens& t   = darkpalette::tokens();
        const auto                 ref = [](const QColor& c) { return quint32(c.red()) | (quint32(c.green()) << 8) | (quint32(c.blue()) << 16); };
        QHash<unsigned, quint32>   set;
        for (const DwmCall& c : qAsConst(g_dwm)) {
            if (c.hwnd == id)
                set.insert(c.attribute, c.value);
        }
        QCOMPARE(set.value(20), 1u);
        QCOMPARE(set.value(35), ref(t.panel));
        QCOMPARE(set.value(36), ref(t.text));
        QCOMPARE(set.value(34), ref(t.border));
        // A new handle (the window recreated) gets it again.
        g_dwm.clear();
        probe->hide();
        probe->recreate();
        probe->show();
        pump(20);
        const quintptr id2 = static_cast<quintptr>(probe->internalWinId());
        bool           again = false;
        for (const DwmCall& c : qAsConst(g_dwm))
            again = again || (c.hwnd == id2 && c.attribute == 20 && c.value == 1);
        QVERIFY(again);
        // Skipped: a window with its own caption, popups, tool tips.
        g_dwm.clear();
        auto* own = new QDialog;
        own->setProperty(darktitlebar::kOwnCaption, true);
        m_windows << own;
        own->show();
        auto* menu = new QMenu;
        m_windows << menu;
        menu->addAction(a("x"));
        menu->popup(QPoint(10, 10));
        pump(20);
        for (const DwmCall& c : qAsConst(g_dwm)) {
            QVERIFY(c.hwnd != static_cast<quintptr>(own->internalWinId()));
            QVERIFY(c.hwnd != static_cast<quintptr>(menu->internalWinId()));
        }
        menu->hide();
        // Off: FALSE and the system's colours.
        g_dwm.clear();
        dark->setEnabled(false);
        set.clear();
        for (const DwmCall& c : qAsConst(g_dwm)) {
            if (c.hwnd == id2)
                set.insert(c.attribute, c.value);
        }
        QCOMPARE(set.value(20, 7), 0u);
        QCOMPARE(set.value(35), 0xFFFFFFFFu);
        QCOMPARE(set.value(36), 0xFFFFFFFFu);
        QCOMPARE(set.value(34), 0xFFFFFFFFu);
        // Windows 10 before 20H1: 19 when 20 is refused.
        g_dwmRefuse20 = true;
        g_dwm.clear();
        auto* older = new QDialog;
        m_windows << older;
        dark->setEnabled(true);
        older->show();
        pump(20);
        bool nineteen = false;
        for (const DwmCall& c : qAsConst(g_dwm))
            nineteen = nineteen || (c.hwnd == static_cast<quintptr>(older->internalWinId()) && c.attribute == 19 && c.value == 1);
        QVERIFY(nineteen);
        dark->setEnabled(false);
    }

    // ---- T-14: the timing budget ----------------------------------------------------------------------------------------------
    void timing()
    {
        installSkin();
        TsWindow t = shownWindow(2000, 1500);
        // 1 000 more widgets (cards, bars and players of the chat layer, other windows' controls).
        auto* host = new QWidget;
        m_windows << host;
        auto* grid = new QGridLayout(host);
        for (int i = 0; i < 125; ++i) {
            grid->addWidget(new QLabel(QString::number(i)), i, 0);
            grid->addWidget(new QPushButton(a("Button")), i, 1);
            grid->addWidget(new QLineEdit(a("text")), i, 2);
            grid->addWidget(new QCheckBox(a("Check")), i, 3);
            auto* combo = new QComboBox;
            combo->addItem(a("a"));
            grid->addWidget(combo, i, 4);
            grid->addWidget(new QToolButton, i, 5);
            grid->addWidget(new QRadioButton(a("Radio")), i, 6);
            grid->addWidget(new QSpinBox, i, 7);
        }
        host->show();
        pump(100);
        auto dark = makeDark();
        dark->setDeferHiddenPlaces(true); // as once the chat layer runs (review fix: the hidden chat costs the toggle nothing)
        QVector<qint64> on, off;
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer clock;
            clock.start();
            dark->setEnabled(true);
            on << clock.elapsed();
            pump(20);
            clock.restart();
            dark->setEnabled(false);
            off << clock.elapsed();
            pump(20);
        }
        std::sort(on.begin(), on.end());
        std::sort(off.begin(), off.end());
        QVERIFY(t.hidden->testAttribute(Qt::WA_PendingResizeEvent)); // the hidden chat waits for its Show
        QCOMPARE(dark->deferredPlaces(), 1);
        qInfo("dark mode timing: %d widgets, %d blocks visible, %d hidden; on %lld ms, off %lld ms (median of 3)", QApplication::allWidgets().size(),
              t.visible->document()->blockCount(), t.hidden->document()->blockCount(), static_cast<long long>(on.at(1)),
              static_cast<long long>(off.at(1)));
        QVERIFY2(on.at(1) <= 900, qPrintable(QString::number(on.at(1))));
        QVERIFY2(off.at(1) <= 900, qPrintable(QString::number(off.at(1))));
    }

    // ---- T-15: the glyphs -----------------------------------------------------------------------------------------------------
    void glyphs()
    {
        const darkpalette::Tokens& t = darkpalette::tokens();
        for (const qreal k : {1.0, 2.0}) {
            const QString folder = darkglyphs::ensure(m_dir->path(), k, t);
            QVERIFY(!folder.isEmpty());
            QVERIFY(folder.endsWith(a("/darkmode/") + QString::number(qRound(k * 100))));
            for (const QString& name : darkglyphs::names()) {
                const QImage image(folder + QLatin1Char('/') + name + a(".png"));
                QVERIFY2(!image.isNull(), qPrintable(name));
                QCOMPARE(image.width(), darkglyphs::sizeOf(name, k));
            }
            QCOMPARE(darkglyphs::sizeOf(a("cb-on"), k), qRound(16 * k));
        }
        // Not rewritten when it is current; drawn again when the colours change.
        const QString   folder = darkglyphs::ensure(m_dir->path(), 1.0, t);
        const QFileInfo before(folder + a("/cb-on.png"));
        const QDateTime written = before.lastModified();
        QFile::setPermissions(folder + a("/cb-on.png"), QFileDevice::ReadOwner); // a rewrite would fail now
        QCOMPARE(darkglyphs::ensure(m_dir->path(), 1.0, t), folder);
        QFile::setPermissions(folder + a("/cb-on.png"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QCOMPARE(QFileInfo(folder + a("/cb-on.png")).lastModified(), written);
        darkpalette::Tokens other = t;
        other.accent              = QColor(0x22, 0x88, 0x44);
        QVERIFY(darkglyphs::stamp(1.0, other) != darkglyphs::stamp(1.0, t));
        QCOMPARE(darkglyphs::ensure(m_dir->path(), 1.0, other), folder);
        QFile marker(folder + a("/glyphs.txt"));
        QVERIFY(marker.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromLatin1(marker.readAll()).trimmed(), darkglyphs::stamp(1.0, other));
        // A folder that can't be made: no glyphs, and the sheet has no pictures.
        QFile blocker(m_dir->path() + a("/blocked"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("x");
        blocker.close();
        QVERIFY(darkglyphs::ensure(m_dir->path() + a("/blocked"), 1.0, t).isEmpty());
        auto dark = std::make_unique<DarkMode>();
        dark->setGlyphBase(m_dir->path() + a("/blocked"));
        dark->setEnabled(true);
        QVERIFY(dark->glyphDir().isEmpty());
        QVERIFY(!qApp->styleSheet().contains(a("url(")));
        dark->setEnabled(false);
    }

    // ---- T-16: tool tips --------------------------------------------------------------------------------------------------------
    void toolTip()
    {
        installSkin();
        TsWindow t    = shownWindow(5, 0);
        auto     dark = makeDark();
        dark->setEnabled(true);
        const QString html = a("<style>td{padding:0px 8px 0px 0px;} .label{font-weight:bold;} .blue{color: blue;} .red{color: red;}</style>"
                               "<table><tr><td class=\"label\">Nickname:</td><td><span class=\"blue\">Bob</span></td></tr>"
                               "<tr><td class=\"label\">Status:</td><td><span class=\"red\">Away</span></td></tr></table>");
        QToolTip::showText(t.tree->viewport()->mapToGlobal(QPoint(10, 10)), html, t.tree->viewport());
        QWidget* tip = nullptr;
        QTRY_VERIFY((tip = [] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (w->objectName() == QLatin1String("qtooltip_label") && w->isVisible())
                    return w;
            }
            return static_cast<QWidget*>(nullptr);
        }()) != nullptr);
        auto*          label = qobject_cast<QLabel*>(tip);
        QTextDocument* doc   = tip->findChild<QTextDocument*>();
        QVERIFY(label && doc);
        QTRY_VERIFY(darkchat::taggedCount(doc) >= 2);
        QCOMPARE(colorAt(doc, findText(doc, a("Bob"))), darkpalette::tokens().link);
        QCOMPARE(colorAt(doc, findText(doc, a("Away"))), darkpalette::tokens().error);
        QCOMPARE(label->text(), html); // TeamSpeak's text stays: the tip doesn't move
        dark->setEnabled(false);
    }

    // ---- T-17: the setting and its card --------------------------------------------------------------------------------------
    void appearanceSection()
    {
        // The switch acts at once through the hook; the form never gets dirty; Restore defaults and Cancel leave it.
        QList<bool> asked;
        auto*       dialog = new SettingsDialog(nullptr);
        m_windows << dialog;
        auto* section = new AppearanceSection(dialog, [&asked](bool on) {
            asked << on;
            Settings::instance().darkMode = on;
        });
        dialog->addSection(SettingsDialog::Tab::Chat, section, true);
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        SwitchBox* box = section->darkModeSwitch();
        QVERIFY(box && box->isEnabled());
        QCOMPARE(box->text(), a("&Dark mode"));
        QVERIFY(!box->isChecked());
        QVERIFY(box->accessibleDescription().startsWith(a("Makes all of TeamSpeak dark right away")));
        QPushButton* apply = nullptr;
        QPushButton* restore = nullptr;
        for (QDialogButtonBox* bb : dialog->findChildren<QDialogButtonBox*>()) {
            if (bb->button(QDialogButtonBox::Apply))
                apply = bb->button(QDialogButtonBox::Apply);
            if (bb->button(QDialogButtonBox::RestoreDefaults))
                restore = bb->button(QDialogButtonBox::RestoreDefaults);
        }
        QVERIFY(apply && restore);
        box->click();
        QCOMPARE(asked, QList<bool>({true}));
        QVERIFY(box->isChecked());
        QVERIFY(!apply->isEnabled());
        restore->click();
        QVERIFY(box->isChecked());
        QVERIFY(Settings::instance().darkMode);
        apply->click(); // Apply after Restore defaults: the key is not the form's
        QVERIFY(Settings::instance().darkMode);
        // The menu or the hotkey changed it meanwhile: refresh() mirrors it without calling the hook.
        Settings::instance().darkMode = false;
        section->refresh();
        QVERIFY(!box->isChecked());
        QCOMPARE(asked.size(), 1);
        Settings::instance().darkMode = true;
        dialog->discardAndClose(); // Cancel
        QVERIFY(Settings::instance().darkMode);
        // The Chat page's first card is Appearance (the section was added at the top).
        // Without the hook: disabled, with the current value.
        AppearanceSection bare;
        QVERIFY(!bare.darkModeSwitch()->isEnabled());
        QVERIFY(bare.darkModeSwitch()->isChecked());
        // The Chat page's description.
        QVERIFY(sourceText(a("src/settingsdialog.cpp")).contains(a("How TeamSpeak and the chat look to you. Others see them as before.")));
    }

    // ---- T-18: TS Media's own windows and lines ----------------------------------------------------------------------------
    void tokensInTsMedia()
    {
        // Inactive: today's colours (tst_coresend keeps its strings).
        QCOMPARE(chatcolors::lineColors(chatcolors::teamSpeakLight(), QString()).accent, a("#4752c4"));
        chatcolors::Colors dark;
        dark.base = darkpalette::tokens().window;
        dark.text = darkpalette::tokens().text;
        dark.dark = true;
        const chatcolors::LineColors plain = chatcolors::lineColors(dark, QString());
        QCOMPARE(plain.accent, a("#949cf7"));
        dark.themeAccent                 = darkpalette::tokens().accent;
        const chatcolors::LineColors on = chatcolors::lineColors(dark, QString());
        QCOMPARE(on.accent, a("#d97757")); // 4.86:1 on #262624 already
        QVERIFY(ui::contrastRatio(QColor(on.warning), dark.base) >= 4.5);
        QCOMPARE(chatcolors::lineColors(dark, a("teal")).accent, chatcolors::lineColors([&] {
                     chatcolors::Colors c = dark;
                     c.themeAccent        = QColor();
                     return c;
                 }(), a("teal")).accent);
        // The chat layout's accents follow it.
        const layoutart::Colors withAccent = layoutart::colorsFor(true, dark.base, dark.text, darkpalette::tokens().link, darkpalette::tokens().accent);
        QCOMPARE(withAccent.accent, darkpalette::tokens().accent);
        QCOMPARE(withAccent.focusBar, darkpalette::tokens().accent);
        QVERIFY(ui::contrastRatio(withAccent.announce, dark.base) >= 3.0);

        // The Settings window and a window that isn't one take the dark tokens while it is on.
        installSkin();
        auto mode = makeDark();
        mode->setEnabled(true);
        auto* dialog = new SettingsDialog(nullptr);
        m_windows << dialog;
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        const settingstheme::Tokens want = darkpalette::settingsTokens(settingstheme::sizeFactor(dialog->font(), dialog));
        const settingstheme::Tokens have = dialog->themeTokens();
        QCOMPARE(have.window, want.window);
        QCOMPARE(have.card, want.card);
        QCOMPARE(have.accent, want.accent);
        QCOMPARE(have.navSel, want.navSel);
        QVERIFY(have.dark);
        auto* plainWindow = new QWidget;
        m_windows << plainWindow;
        const settingstheme::Tokens other = settingstheme::tokensFor(plainWindow);
        QCOMPARE(other.window, want.window);
        QCOMPARE(other.accent, want.accent);
        mode->setEnabled(false);
        pump(20);
        QVERIFY(dialog->themeTokens().accent != want.accent || !dialog->themeTokens().dark);
    }

    // ---- T-18b (review fix): the send window's checked toggle on a picture (a tile's Spoiler eye) draws its glyph in the
    // primary colour's text colour: dark on dark mode's accent (5.6:1, 6.7:1 on hover; white was 3.1:1 and 2.6:1), white on
    // the plugin's blurple without it ---------------------------------------------------------------------------------------------
    void mediaToggleGlyph()
    {
        const auto glyphPixels = [](bool checked, bool* white, bool* onAccent) {
            QWidget host;
            host.resize(64, 64);
            auto* button = new composeui::IconButton(composeui::Glyph::Spoiler, composeui::IconButton::Look::OnMedia, 32, &host);
            button->setCheckable(true);
            button->setChecked(checked);
            const QImage img  = button->grab().toImage().convertToFormat(QImage::Format_ARGB32);
            const QColor ink  = darkpalette::tokens().onAccent;
            *white            = false;
            *onAccent         = false;
            for (int y = img.height() / 4; y < img.height() * 3 / 4; ++y) { // the glyph's square (half the side, centred)
                for (int x = img.width() / 4; x < img.width() * 3 / 4; ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.alpha() < 250)
                        continue;
                    *white    = *white || (c.red() > 235 && c.green() > 235 && c.blue() > 235);
                    *onAccent = *onAccent || (qAbs(c.red() - ink.red()) + qAbs(c.green() - ink.green()) + qAbs(c.blue() - ink.blue()) <= 12);
                }
            }
        };
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(20);
        const composeui::Theme theme = composeui::themeFor(QApplication::palette());
        QCOMPARE(theme.primary, darkpalette::tokens().accent);
        QVERIFY(ui::contrastRatio(theme.onPrimary, theme.primary) >= 4.5);
        QVERIFY(ui::contrastRatio(theme.onPrimary, theme.primaryHover) >= 4.5);
        bool white = false, onAccent = false;
        glyphPixels(true, &white, &onAccent);
        QVERIFY(onAccent);
        QVERIFY(!white);
        dark->setEnabled(false);
        pump(20);
        QPalette light = QApplication::palette();
        light.setColor(QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
        light.setColor(QPalette::WindowText, QColor(0x10, 0x10, 0x10));
        QApplication::setPalette(light);
        glyphPixels(true, &white, &onAccent);
        QVERIFY(white); // the plugin's own look is unchanged
        QVERIFY(!onAccent);
    }

    // ---- T-18c (review fix, source checks: ChatIntegration is plugin-only): the cards a chat already shows are drawn again
    // when the theme's accent changes, also between two dark backgrounds (dark mode on or off over a dark skin, Classic or
    // no bubbles): every place that notes what the previews were drawn for notes the accent, and both checks compare it ---------
    void cardsFollowTheAccent()
    {
        const QString ci = sourceText(a("src/chatintegration.cpp"));
        QVERIFY(ci.contains(a("style.accent                    = colors.themeAccent;")));
        QCOMPARE(ci.count(a("layoutBubbles      = surfacesKey(style);")), 3);
        QCOMPARE(ci.count(a("layoutAccent       = style.accent;")), 3);
        // (2.7.1: in redrawForLook, the relayout timer's body.)
        QVERIFY(ci.contains(a("v->layoutBubbles = bubbles;\n    v->layoutAccent  = style.accent;")));
        QVERIFY(ci.contains(a("bubbles == v->layoutBubbles\n        && style.accent == v->layoutAccent)")));
        QVERIFY(ci.contains(a("surfacesKey(style) != view.layoutBubbles || style.accent != view.layoutAccent)")));
        QVERIFY(sourceText(a("src/chatintegration.h")).contains(a("QColor                  layoutAccent;")));
    }

    // ---- T-E (2.7.1, source checks: ChatIntegration is plugin-only): TS Media's pictures follow TeamSpeak's look at once.
    // Dark mode says when (on, off, a skin taken over, and the off of a Disable or Reload; never at TeamSpeak's Quit), inside
    // its place bracket; the chat layer takes the chats' colours now, the layout's bubbles, then draws every picture again:
    // visible chats at once, hidden ones at their Show or at the unload (live 2.7.0: dark cards after Disable) --------------
    void cardsFollowTheTheme()
    {
        const auto bodyOf = [](const QString& text, const char* head) {
            const int from = text.indexOf(a(head));
            return from < 0 ? QString() : text.mid(from, text.indexOf(a("\n}\n"), from) - from);
        };
        const QString plugin = sourceText(a("src/plugin.cpp"));
        QVERIFY(plugin.contains(a("QObject::connect(g_dark.data(), &DarkMode::themeChanged, chat, &ChatIntegration::themeChanged);")));
        QVERIFY(plugin.contains(a("QObject::connect(g_dark.data(), &DarkMode::themeChanged, g_music.data(), &MusicController::refreshNow);")));
        const QString dm = sourceText(a("src/darkmode.cpp"));
        QCOMPARE(dm.count(a("emit themeChanged();")), 1);
        QCOMPARE(dm.count(a("announceTheme();")), 3);
        for (const char* fn : {"void DarkMode::apply()", "void DarkMode::remove()", "void DarkMode::adoptSkin()"}) {
            const QString body = bodyOf(dm, fn);
            QVERIFY2(body.contains(a("announceTheme();")), fn);
            QVERIFY2(body.indexOf(a("announceTheme();")) < body.indexOf(a("restorePlaces(places);")), fn);
        }
        const QString announce = bodyOf(dm, "void DarkMode::announceTheme()");
        QVERIFY(announce.contains(a("sendPostedEvents(nullptr, QEvent::LayoutRequest)")));
        QVERIFY(announce.indexOf(a("sendPostedEvents(nullptr, QEvent::LayoutRequest)")) < announce.indexOf(a("emit themeChanged();")));
        const int quit = dm.indexOf(a("if (m_applied && closing)"));
        QVERIFY(quit > 0);
        QVERIFY(!dm.mid(quit, dm.indexOf(a("return;"), quit) - quit).contains(a("announceTheme")));
        // Review fix: the one test of TeamSpeak's Quit, which plugin.cpp asks before dark mode goes, for the chat layer too.
        QVERIFY(bodyOf(dm, "void DarkMode::shutdown()").contains(a("const bool closing     = teamSpeakClosing(&mainShown, &othersShown);")));
        QVERIFY(bodyOf(dm, "bool DarkMode::teamSpeakClosing(").contains(a("return m_quitting || QCoreApplication::closingDown() || !main;")));
        const QString unload = a("g_chat->beginUnload(g_dark ? g_dark->teamSpeakClosing() : QCoreApplication::closingDown());");
        QVERIFY(plugin.contains(unload));
        QVERIFY(plugin.indexOf(unload) < plugin.indexOf(a("g_dark->shutdown();")));

        const QString ci    = sourceText(a("src/chatintegration.cpp"));
        const QString theme = bodyOf(ci, "void ChatIntegration::themeChanged()");
        QVERIFY(theme.contains(a("sampleWaiting();")));
        QVERIFY(theme.indexOf(a("sampleWaiting();")) < theme.indexOf(a("m_layout->themeChanged();")));
        QVERIFY(theme.indexOf(a("m_layout->themeChanged();")) < theme.indexOf(a("redrawForLook(")));
        QVERIFY(theme.contains(a("lookPending = true;")));
        // Review fix: the spoiler bars, TS Media's lines and the reply lines follow in the same call (tst_chattextspoilers
        // checks the bars themselves); nothing at TeamSpeak's Quit.
        QVERIFY(theme.indexOf(a("if (m_unloading && !m_unloadDraws)")) > 0);
        QVERIFY(theme.indexOf(a("if (m_unloading && !m_unloadDraws)")) < theme.indexOf(a("sampleWaiting();")));
        for (const char* call : {"m_textSpoilers->themeChanged();", "updateLineColors();", "m_replies->themeChanged();", "emit chatColorsChanged();"})
            QVERIFY2(theme.contains(a(call)), call);
        QVERIFY(theme.indexOf(a("m_layout->themeChanged();")) < theme.indexOf(a("m_textSpoilers->themeChanged();")));
        const QString redraw = bodyOf(ci, "void ChatIntegration::redrawForLook(");
        QVERIFY(redraw.contains(a("isSwapped(")));
        QVERIFY(redraw.contains(a("drawForLook(browser, *v, key)")));
        QVERIFY(redraw.contains(a("v->lookPending = false;"))); // review fix: the timer's redraw ends the wait for the Show too
        QVERIFY(ci.contains(a("redrawForLook(guard.data(), false);"))); // the relayout timer: one body
        // Review fix: a redraw for the look never puts a stand-in where a picture is drawn.
        const QString draw = bodyOf(ci, "bool ChatIntegration::drawForLook(");
        for (const char* needle : {"Core::StillDecode::CachedOnly", "Core::StillDecode::CheapOnly", "Core::StillDecode::Any", "kUnloadDecodeMs",
                                   "onScreen(browser, view, key, true)", "refreshPreview(browser, key, false, true)", "m_core->setStillDecode(before);",
                                   "if (draw(Core::StillDecode::CachedOnly) || fromKept())\n        return true;", "m_unloadDecodeNs += clock.nsecsElapsed();",
                                   // 2.7.3 (M3): near the view, a picture drawn from a kept stand-in is asked for as any draw too.
                                   "if (draw(Core::StillDecode::CachedOnly))\n            return true;\n        const bool kept = fromKept();\n"
                                   "        if (kept && !standIn)\n            return true;\n        return draw(Core::StillDecode::Any) || kept;",
                                   // The unload's cheap decodes: the pictures of lookOld, where the draw with what Core has just put one whose
                                   // still isn't at hand (2.7.3 review: so a chat hidden since a pick needs nothing more).
                                   "if (!view.lookOld.contains(key) || m_unloadDecodeNs >= kUnloadDecodeMs * 1000000)\n        return false;"})
            QVERIFY2(draw.contains(a(needle)), needle);
        // 2.7.3 (M3): every still a dark draw used is noted, stand-ins and nothing too; a kept nothing answers nothing. Review
        // fix: a blurhash is noted without its image (booked as nothing) and made again by Core for the box asked.
        const QString still = bodyOf(ci, "MediaStill ChatIntegration::stillFor(");
        QVERIFY(still.contains(a("if (kept.still.image.isNull()) {\n                if (kept.still.source != MediaStill::BlurHash)\n"
                                 "                    return kept.still;\n                MediaStill s  = m_core->blurHashStill(key, box);")));
        QVERIFY(still.contains(a("KeptStill kept{key, box, s, m_core->stillStandIns() != standIns || s.image.isNull() || s.source != MediaStill::Full};")));
        QVERIFY(still.contains(a("if (s.source == MediaStill::BlurHash)\n            kept.still.image = QImage();\n        m_recorded.append(kept);")));
        // Core's blurhash still: the cache entry still() uses for that source and size, never a stand-in counted.
        const QString hash = bodyOf(sourceText(a("src/core.cpp")), "MediaStill Core::blurHashStill(");
        QVERIFY(hash.contains(a("QString::number(static_cast<int>(MediaStill::BlurHash))")) && hash.contains(a("decodeBlurHash(it.value().link, maxPixels)")));
        QVERIFY(!hash.contains(a("m_stillStandIns")));
        QVERIFY(bodyOf(ci, "bool ChatIntegration::refreshPreview(").contains(a("if (keepDrawn && m_core->stillStandIns() != standIns && view->resourced.contains(key)) {")));
        // 2.7.2 (D1): at the unload what Core has, then the still a picture was drawn from in dark mode, then (only for a picture
        // never drawn in dark mode) a cheap decode; the stills are noted only while dark mode is on, a draw for TeamSpeak's
        // look drops them, so does the budget's swap; every draw of ours asks stillFor, not Core.
        QVERIFY(draw.indexOf(a("fromKept()")) < draw.indexOf(a("Core::StillDecode::CheapOnly")));
        QVERIFY(draw.contains(a("m_stillOverride  = &kept;")) && draw.contains(a("m_stillOverride  = nullptr;")));
        QCOMPARE(ci.count(a("m_recordStills = DarkMode::isActive();")), 3);
        QVERIFY(bodyOf(ci, "QVector<ChatIntegration::KeptStill> ChatIntegration::keptAfterDraw(").contains(a("if (!DarkMode::isActive())\n        return {};")));
        QVERIFY(bodyOf(ci, "void ChatIntegration::pictureSwapped(").contains(a("view->keptStills.remove(name.mid(kScheme.length()));")));
        // Review fix: an album grid a new plan frees (renamed as its items arrive) leaves the books, its tiles' stills with it.
        const QString freed = bodyOf(sourceText(a("src/chatalbums.cpp")), "void ChatIntegration::applyAlbums(");
        QVERIFY(freed.contains(a("view->scaler->forget(resourceName(id));")));
        QVERIFY(freed.indexOf(a("view->keptStills.remove(id);")) > freed.indexOf(a("view->scaler->forget(resourceName(id));")));
        QVERIFY(!bodyOf(ci, "QImage ChatIntegration::renderFor(").contains(a("m_core->still(")));
        QVERIFY(bodyOf(sourceText(a("src/chatalbums.cpp")), "QImage ChatIntegration::renderAlbumFor(").contains(a("t.still            = stillFor(key, pixels);")));
        QVERIFY(ci.contains(a("                else\n                    drawForLook(b, view, key);"))); // a stale picture in an older look
        const int show = ci.indexOf(a("case QEvent::Show:"));
        QVERIFY(show > 0 && ci.mid(show, 600).contains(a("lookPending")) && ci.mid(show, 600).contains(a("drawOldLooks(browser, *v, true, \"at show\");")));
        const int holds = ci.indexOf(a("m_activeHolds = 0;"));
        const QString atUnload = ci.mid(holds, ci.indexOf(a("prepareShutdown();"), holds) - holds);
        QVERIFY(holds > 0 && atUnload.contains(a("lookPending")));
        QVERIFY(atUnload.contains(a("if (m_unloadDraws) {"))); // review fix: never at TeamSpeak's Quit
        const QString begin = bodyOf(ci, "void ChatIntegration::beginUnload(bool teamSpeakClosing)");
        QVERIFY(begin.contains(a("m_unloadDraws = !teamSpeakClosing;")));
        QVERIFY(begin.contains(a("it->lookPending = false;")) && begin.contains(a("it->lookOld.clear();")));
        // Core: a mode that decodes nothing, one without a worker, and the count of stand-ins (tst_linkui checks them).
        const QString core = bodyOf(sourceText(a("src/core.cpp")), "MediaStill Core::still(");
        QVERIFY(core.contains(a("m_stillDecode == StillDecode::CachedOnly")));
        QVERIFY(core.contains(a("if (m_stillDecode == StillDecode::Any)")));
        QVERIFY(core.indexOf(a("if (m_stillDecode == StillDecode::Any)")) < core.indexOf(a("startStillDecode(key, cacheKey, source, path, maxPixels);")));
        QCOMPARE(core.count(a("++m_stillStandIns;")), 2);
        // The reply lines: visible chats now, hidden ones at their Show; any full layout of a chat's lines ends that wait.
        const QString replies = sourceText(a("src/chatreplies.cpp"));
        const QString rtheme  = bodyOf(replies, "void ChatReplies::themeChanged()");
        QVERIFY(rtheme.contains(a("it->lookPending = true;")) && rtheme.contains(a("refreshLines(browser);")));
        QVERIFY(replies.contains(a("if (View* view = type == QEvent::Show ? viewOf(browser) : nullptr; view && view->lookPending) {")));
        QVERIFY(bodyOf(replies, "void ChatReplies::refreshLines(").contains(a("view->lookPending = false;")));
        const QString layout = bodyOf(sourceText(a("src/chatlayout.cpp")), "void ChatLayout::themeChanged()");
        QVERIFY(layout.contains(a("Mode::Classic")));
        QVERIFY(layout.contains(a("refreshEnv(")));
    }

    // ---- T-19: the Plugins menu pair and the hotkey -------------------------------------------------------------------------
    void menus()
    {
        const QString plugin = sourceText(a("src/plugin.cpp"));
        for (const char* needle : {"MenuDarkOn          = 10", "MenuDarkOff         = 11", "{MenuDarkOn, i18n::t(\"Turn on dark mode\")}",
                                   "{MenuDarkOff, i18n::t(\"Turn off dark mode\")}", "{\"tsmedia_darkmode\", i18n::t(\"Turn dark mode on or off\")}",
                                   "setPluginMenuEnabled(id.constData(), MenuDarkOn, dark ? 0 : 1)",
                                   "setPluginMenuEnabled(id.constData(), MenuDarkOff, dark ? 1 : 0)",
                                   "onGuiThread([] { setDarkMode(!Settings::instance().darkMode); })",
                                   "new AppearanceSection(dialog, [](bool on) { setDarkMode(on); })",
                                   "connect(g_dark, &DarkMode::enabledChanged, appearance, &AppearanceSection::refresh)",
                                   "connect(chat, &ChatIntegration::chatsChanged, g_dark.data(), &DarkMode::checkDocuments)",
                                   "connect(g_dark.data(), &DarkMode::placesRestored, chat, &ChatIntegration::retakeScalePlaces)",
                                   "g_dark->shutdown();"})
            QVERIFY2(plugin.contains(a(needle)), needle);
        // The dark mode goes before Core is made, and away before the chat layer.
        QVERIFY(plugin.indexOf(a("g_dark = new DarkMode;")) < plugin.indexOf(a("auto* core = new Core;")));
        QVERIFY(plugin.indexOf(a("g_dark->shutdown();")) < plugin.indexOf(a("delete g_chat.data();")));
        QVERIFY(plugin.indexOf(a("proxy::shutdown();")) < plugin.indexOf(a("g_dark->shutdown();")));

        // The real DLL's menu items: ids 10 and 11 with their texts, after the chat proxy pair.
        QLibrary dll(QString::fromUtf8(TSMEDIA_PLUGIN_DLL));
        QVERIFY2(dll.load(), qPrintable(dll.errorString()));
        using InitMenus = void (*)(PluginMenuItem***, char**);
        using FreeMem   = void (*)(void*);
        using InitKeys  = void (*)(PluginHotkey***);
        auto initMenus  = reinterpret_cast<InitMenus>(dll.resolve("ts3plugin_initMenus"));
        auto freeMemory = reinterpret_cast<FreeMem>(dll.resolve("ts3plugin_freeMemory"));
        auto initKeys   = reinterpret_cast<InitKeys>(dll.resolve("ts3plugin_initHotkeys"));
        QVERIFY(initMenus && freeMemory && initKeys);
        PluginMenuItem** items = nullptr;
        char*            icon  = nullptr;
        initMenus(&items, &icon);
        QStringList texts;
        QList<int>  ids;
        for (int i = 0; items && items[i]; ++i) {
            ids << items[i]->id;
            texts << QString::fromUtf8(items[i]->text);
            freeMemory(items[i]);
        }
        freeMemory(items);
        const int on = ids.indexOf(10);
        QVERIFY(on >= 0);
        QCOMPARE(texts.at(on), a("Turn on dark mode"));
        QCOMPARE(ids.at(on + 1), 11);
        QCOMPARE(texts.at(on + 1), a("Turn off dark mode"));
        QCOMPARE(ids.at(on - 1), 9); // after the chat proxy pair
        PluginHotkey** keys = nullptr;
        initKeys(&keys);
        bool found = false;
        for (int i = 0; keys && keys[i]; ++i) {
            if (QString::fromUtf8(keys[i]->keyword) == a("tsmedia_darkmode")) {
                found = true;
                QCOMPARE(QString::fromUtf8(keys[i]->description), a("Turn dark mode on or off"));
            }
            freeMemory(keys[i]);
        }
        freeMemory(keys);
        QVERIFY(found);
        dll.unload();
    }

    // ---- T-4b (review fix): the skin's fonts stay (TeamSpeak's headlines and banners, a custom skin's chat font) -----------
    void fontsKept()
    {
        // The reader of the skin's sheet: the font declarations with their selectors, whatever the spacing, comments and strings.
        const QString sheet = a("/* QLabel { font: 1px; } */\n"
                                "QLabel#OptionsHeadlineTitleLabel { font: 18px \"Segoe UI\"; color: #123456; }\n"
                                "QLabel#appNameLabel{font:32px 'A;B' ; font-weight:bold}\n"
                                "QLabel#bookmarksBanner,\n QLabel#identitiesBanner { font-weight: bold; qproperty-minimumSize: 0px 32px; }\n"
                                "QWidget[x=\"{;}\"] { background: url(\"a;b.png\"); FONT-SIZE: 9pt; }\n"
                                "QPushButton { padding: 3px; }\n");
        QCOMPARE(darkpalette::fontRules(sheet), a("QLabel#OptionsHeadlineTitleLabel { font: 18px \"Segoe UI\"; }\n"
                                                  "QLabel#appNameLabel { font:32px 'A;B'; font-weight:bold; }\n"
                                                  "QLabel#bookmarksBanner, QLabel#identitiesBanner { font-weight: bold; }\n"
                                                  "QWidget[x=\"{;}\"] { FONT-SIZE: 9pt; }\n"));
        QVERIFY(darkpalette::fontRules(QString()).isEmpty());

        // The test skin with a custom skin's chat font: every widget's font as the skin gives it, while on and after off, and no
        // FontChange reaches the chat (the chat zoom and TeamSpeak's chat font stay untouched).
        qApp->setStyleSheet(QString::fromLatin1(kSkinSheet) + a("QTextEdit#ChatTab { font-size: 14pt; }\n"));
        TsWindow t      = shownWindow(20, 0, false);
        auto*    dialog = new QDialog;
        m_windows << dialog;
        auto* l        = new QVBoxLayout(dialog);
        auto* headline = new QLabel(a("Design"));
        headline->setObjectName(a("OptionsHeadlineTitleLabel"));
        auto* banner = new QLabel(a("Bookmarks"));
        banner->setObjectName(a("bookmarksBanner"));
        l->addWidget(headline);
        l->addWidget(banner);
        dialog->show();
        pump(30);
        QCOMPARE(headline->font().pixelSize(), 18);
        QVERIFY(banner->font().bold());
        QCOMPARE(t.visible->font().pointSize(), 14);
        QHash<QWidget*, QFont> fonts;
        for (QWidget* w : t.window->findChildren<QWidget*>() + dialog->findChildren<QWidget*>())
            fonts.insert(w, w->font());
        int  fontChanges = 0;
        QStringList seen;
        struct Counter : QObject {
            int*         n;
            QStringList* fontsSeen;
            Counter(int* count, QStringList* list) : n(count), fontsSeen(list) {}
            bool eventFilter(QObject* o, QEvent* e) override
            {
                if (e->type() == QEvent::FontChange) {
                    ++*n;
                    *fontsSeen << static_cast<QWidget*>(o)->font().toString() + QString::fromLatin1(" mask %1").arg(static_cast<QWidget*>(o)->font().resolve());
                }
                return false;
            }
        } counter(&fontChanges, &seen);
        t.visible->installEventFilter(&counter);
        const auto same = [&fonts] {
            for (auto it = fonts.cbegin(); it != fonts.cend(); ++it) {
                if (it.key()->font() != it.value())
                    return it.key()->metaObject()->className() + a(" ") + it.key()->objectName() + a(": ") + it.key()->font().toString();
            }
            return QString();
        };
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        QVERIFY(dark->appliedSheet().contains(a("QTextEdit#ChatTab { font-size: 14pt; }")));
        QVERIFY(dark->appliedSheet().contains(a("QLabel#OptionsHeadlineTitleLabel { font: 18px Arial; }")));
        QCOMPARE(same(), QString());
        // Qt takes a sheet's font back and gives it again while it swaps sheets (TeamSpeak's own skin switch does the same):
        // the chat ends with the skin's font every time.
        QVERIFY(!seen.isEmpty() && seen.last().startsWith(t.visible->font().toString()));
        dark->setEnabled(false);
        pump(30);
        QCOMPARE(same(), QString());
        QVERIFY(seen.last().startsWith(t.visible->font().toString()));
        t.visible->removeEventFilter(&counter);
        // A skin without a chat font (TeamSpeak's own): no FontChange reaches the chat at all.
        qApp->setStyleSheet(QString::fromLatin1(kSkinSheet));
        pump(30);
        fontChanges = 0;
        seen.clear();
        t.visible->installEventFilter(&counter);
        dark->setEnabled(true);
        pump(30);
        dark->setEnabled(false);
        pump(30);
        t.visible->removeEventFilter(&counter);
        QVERIFY2(fontChanges == 0, qPrintable(seen.join(QLatin1Char('|'))));
    }

    // ---- review fix: a new chat whose document got the address of a freed one is themed --------------------------------------
    void staleDocument()
    {
        installSkin();
        TsWindow t    = shownWindow(5, 0, false);
        auto     dark = makeDark();
        dark->setEnabled(true);
        // The address of a freed document given to the next one (the allocator may do it any time): the same storage here.
        alignas(QTextDocument) unsigned char storage[sizeof(QTextDocument)];
        bool                                 alive = false;
        const auto                           freeDoc = qScopeGuard([&storage, &alive] {
            if (alive)
                reinterpret_cast<QTextDocument*>(storage)->~QTextDocument();
        });
        // A private chat closed: its document freed (its entry stays until ChatIntegration's next discover).
        QTextDocument* closed = new (storage) QTextDocument;
        alive                 = true;
        closed->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        auto* view = new QTextBrowser;
        view->setObjectName(a("ChatTab"));
        view->setDocument(closed);
        t.tabs->addTab(view, a("Private"));
        dark->checkDocuments();
        QVERIFY(dark->isTracked(closed));
        delete view;
        closed->~QTextDocument();
        alive = false;
        QVERIFY(dark->isTracked(closed)); // the stale entry (its pointer only compared)
        // Another chat whose document got the same address.
        QTextDocument* reused = new (storage) QTextDocument;
        alive                 = true;
        QCOMPARE(static_cast<void*>(reused), static_cast<void*>(storage));
        reused->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        std::unique_ptr<QTextBrowser> chat(new QTextBrowser); // goes before the document it shows
        chat->setObjectName(a("ChatTab"));
        chat->setDocument(reused);
        appendLines(chat.get(), someLines(5, false));
        t.tabs->addTab(chat.get(), a("Another"));
        t.tabs->setCurrentWidget(chat.get()); // Polish and Show: themed before its first paint
        pump(30);
        QCOMPARE(reused->defaultStyleSheet(), dark->chatSheetFor(reused));
        QVERIFY(darkchat::taggedCount(reused) > 0);
        dark->setEnabled(false);
        QCOMPARE(reused->defaultStyleSheet(), QString::fromLatin1(kSkinChat));
        QCOMPARE(darkchat::taggedCount(reused), 0);
    }

    // ---- review fix: the bookkeeping of tool tips and title bars keeps only what is alive -----------------------------------
    void bookkeeping()
    {
        darktitlebar::setSetAttributeForTests(fakeSet);
        installSkin();
        TsWindow t    = shownWindow(3, 0, false);
        auto     dark = makeDark();
        dark->setEnabled(true);
        for (int i = 0; i < 10; ++i) {
            auto* d = new QDialog;
            d->show();
            pump(5);
            delete d;
        }
        auto* last = new QDialog;
        m_windows << last;
        last->show();
        pump(10);
        QCOMPARE(dark->barEntries(), dark->stats().titleBars);
        QVERIFY(dark->stats().titleBars >= 2);
        // Qt makes a new tip label (and document) for the next tip once one has hidden.
        const auto visibleTip = [] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (w->objectName() == QLatin1String("qtooltip_label") && w->isVisible())
                    return w;
            }
            return static_cast<QWidget*>(nullptr);
        };
        for (int i = 0; i < 6; ++i) {
            QToolTip::showText(t.tree->viewport()->mapToGlobal(QPoint(10, 10)), QString::fromLatin1("<b>Bob</b> <span style=\"color:blue\">%1</span>").arg(i),
                               t.tree->viewport());
            QTRY_VERIFY(visibleTip() != nullptr);
            QToolTip::hideText();
            QTRY_VERIFY(visibleTip() == nullptr);
            pump(30); // the label goes (deleteLater)
        }
        QToolTip::showText(t.tree->viewport()->mapToGlobal(QPoint(10, 10)), a("<b>Bob</b> <span style=\"color:blue\">last</span>"), t.tree->viewport());
        QTRY_VERIFY(visibleTip() != nullptr);
        QVERIFY2(dark->tipEntries() <= 2, qPrintable(QString::number(dark->tipEntries())));
        QToolTip::hideText();
        dark->setEnabled(false);
    }

    // ---- review fix: multi-line fields have the fields' outline, focus border and disabled look ----------------------------
    void textFields()
    {
        const darkpalette::Tokens& k     = darkpalette::tokens();
        const QString              sheet = darkpalette::appSheet(k, 1.0, darkpalette::Metrics{1, 17}, QString());
        for (const char* rule : {"QTextEdit, QPlainTextEdit { background: #262624; color: #f5f4ee; border: 1px solid #7d7b75;",
                                 "QTextEdit:hover, QPlainTextEdit:hover { border-color: #96948e; }",
                                 "QTextEdit:focus, QPlainTextEdit:focus { border-color: #d97757; }",
                                 "QTextEdit[readOnly=\"true\"], QPlainTextEdit[readOnly=\"true\"] { border-color: #3e3e3a; }",
                                 "QTextEdit:disabled, QPlainTextEdit:disabled { background: #2a2a28; color: #7f7d77; border-color: #3e3e3a; }"})
            QVERIFY2(sheet.contains(a(rule)), rule);
        QVERIFY(ui::contrastRatio(k.control, k.window) >= 3.0);
        installSkin();
        auto* dialog = new QDialog;
        m_windows << dialog;
        auto* l    = new QVBoxLayout(dialog);
        auto* edit = new QTextEdit;
        auto* off  = new QPlainTextEdit;
        off->setEnabled(false);
        l->addWidget(edit);
        l->addWidget(off);
        dialog->show();
        pump(20);
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(20);
        QCOMPARE(edit->palette().color(QPalette::Active, QPalette::Text), k.text);
        QCOMPARE(off->palette().color(QPalette::Disabled, QPalette::Text), k.disabled); // not the enabled text: it takes no input
        dark->setEnabled(false);
    }

    // ---- review fix: the line preview under the switch follows dark mode ---------------------------------------------------
    void linePreviewFollows()
    {
        QColor        base(Qt::white);
        ChatTextHooks hooks;
        hooks.linePreview = [&base](const QString& choice) {
            chatcolors::Colors c;
            c.base                             = base;
            c.text                             = chatcolors::isDark(base) ? darkpalette::tokens().text : QColor(Qt::black);
            c.dark                             = chatcolors::isDark(base);
            const chatcolors::LineColors lines = chatcolors::lineColors(c, choice);
            return LinePreviewColors{c.base, c.text, QColor(lines.accent), QColor(lines.warning)};
        };
        auto* section = new ChatTextSection(nullptr, hooks);
        m_windows << section;
        QCOMPARE(section->preview()->base(), QColor(Qt::white));
        base = darkpalette::tokens().window; // the visible chat's colours once dark mode is on
        section->refreshColors();
        QCOMPARE(section->preview()->base(), base);
        QVERIFY(ui::contrastRatio(section->preview()->accent(), base) >= 4.5);
        // plugin.cpp refreshes it when dark mode is on or off, and again once the chat's colours settled (its sample).
        const QString plugin = sourceText(a("src/plugin.cpp"));
        QVERIFY(plugin.contains(a("QObject::connect(g_dark, &DarkMode::enabledChanged, section, &ChatTextSection::refreshColors)")));
        QVERIFY(plugin.contains(a("QObject::connect(g_chat, &ChatIntegration::chatColorsChanged, section, &ChatTextSection::refreshColors)")));
        QVERIFY(sourceText(a("src/chatintegration.cpp")).contains(a("emit chatColorsChanged();")));
    }

    // ---- the CRASH RULE in the sources --------------------------------------------------------------------------------------
    // ---- 2.7.2 (D3): the chat column keeps the skin's width ------------------------------------------------------------------
    // The dark sheet's tab pane had no side insets where the skin's (the native frame) has some: the chat column changed its
    // width with every toggle, so the visible chat rewrapped inside the toggle and a hidden one at its Show, after which
    // TS Media's own-right bubbles, dividers and pictures followed on 180-200 ms timers (live: a horizontal scroll bar for
    // ~200 ms, then a jump). Now the pane keeps the skin's insets in the window colour: the visible chat's column, and the
    // hidden chat's at its Show, are as wide in both looks; the rule names the chat tab widget's class only.
    void chatColumnKept()
    {
        installSkin();
        TsWindow t = shownWindow(300, 300);
        t.tabs->setCurrentIndex(1); // the hidden chat laid out once, as TeamSpeak's tabs are after a while
        pump(30);
        t.tabs->setCurrentIndex(0);
        pump(30);
        const int visibleWidth = t.visible->viewport()->width();
        const int hiddenWidth  = t.hidden->viewport()->width();
        QVERIFY(visibleWidth > 300);
        auto dark = makeDark();
        dark->setEnabled(true);
        const darkpalette::Metrics m = dark->metrics();
        qInfo("skin pane: %s left %d right %d; column %d px", qPrintable(m.paneSelector), m.paneLeft, m.paneRight, visibleWidth);
        QCOMPARE(m.paneSelector, a("DarkTestChatTabs"));
        QVERIFY(qApp->styleSheet().contains(a("DarkTestChatTabs::pane { border-left: %1px solid ").arg(m.paneLeft)));
        QVERIFY(!qApp->styleSheet().contains(a("QTabWidget::pane { border-left:")));
        QCOMPARE(t.visible->viewport()->width(), visibleWidth); // no rewrap of the visible chat inside the toggle
        pump(30);
        t.tabs->setCurrentIndex(1); // the hidden chat's Show: the size it gets now
        QCOMPARE(t.hidden->viewport()->width(), hiddenWidth);
        pump(30);
        t.tabs->setCurrentIndex(0);
        pump(30);
        dark->setEnabled(false);
        QCOMPARE(t.visible->viewport()->width(), visibleWidth);
        pump(30);
        t.tabs->setCurrentIndex(1);
        QCOMPARE(t.hidden->viewport()->width(), hiddenWidth);
        t.tabs->setCurrentIndex(0);
        pump(30);
        // A tab widget that isn't shown yet (TeamSpeak's start with dark mode on): its style's own answer is the same.
        auto* lone = new DarkTestChatTabs;
        m_windows << lone;
        auto* chat = new QTextBrowser;
        chat->setObjectName(a("ChatTab"));
        lone->addTab(chat, a("Lobby"));
        lone->resize(t.tabs->size());
        t.window->hide();
        auto late = makeDark();
        late->setEnabled(true);
        QCOMPARE(late->metrics().paneSelector, a("DarkTestChatTabs"));
        QCOMPARE(late->metrics().paneLeft, m.paneLeft);
        QCOMPARE(late->metrics().paneRight, m.paneRight);
        late->setEnabled(false);
    }

    // 2.7.2 live test: under a dark application palette (dark mode's) TeamSpeak adds its own dark-mode files, after a "\n",
    // to the sheet of every chat it makes (<style>_chat_darkmode.qss: a new server tab or private chat while dark mode is on,
    // every chat of a start with dark mode on) and to the application sheet when it puts a skin in place
    // (<style>_darkmode.qss: a style picked in Options -> Design meanwhile). 2.7.0-2.7.1 gave those sheets back at off and at
    // the unload: such a chat kept TeamSpeak's dark chat colours for good (light blue times and lines on white, its own
    // name colour and weight), and the chat tab names were white on the light tab bar. Now the skin's own sheets go back,
    // and the chat's lines get the skin's look.
    void teamSpeakDarkFilesGoBack_data()
    {
        QTest::addColumn<bool>("unload");
        QTest::addColumn<bool>("late");
        QTest::newRow("off") << false << false;
        QTest::newRow("unload") << true << false;
        // 2.7.3: dark mode sees the new chat before TeamSpeak gives it its sheet (then its first lines).
        QTest::newRow("sheet after dark mode saw the chat, off") << false << true;
        QTest::newRow("sheet after dark mode saw the chat, unload") << true << true;
    }
    void teamSpeakDarkFilesGoBack()
    {
        QFETCH(bool, unload);
        QFETCH(bool, late);
        // TeamSpeak's styles folder (the config folder's here) with its two dark-mode files, written the way TeamSpeak's are.
        const QString chatDark = a(".TextMessage_UserLink, .TextMessage_ChannelLink { color: #006ad2; font-weight: normal; }\n"
                                   ".TextMessage_Welcome { color: #11de4d; }\n"
                                   ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Time { color: #83acd2; }\n");
        const QString appDark  = a("QTabBar::tab { color: white; }\n");
        // 2.7.3 (M7): with the style's own files, which TeamSpeak's loader composes them with.
        QVERIFY(writeStyles({{a("default.qss"), QString::fromLatin1(kSkinSheet)},
                             {a("default_chat.qss"), QString::fromLatin1(kSkinChat)},
                             {a("default_chat_darkmode.qss"), chatDark},
                             {a("default_darkmode.qss"), appDark}}));
        installSkin();
        TsWindow t    = shownWindow(10, 10);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        // A chat TeamSpeak makes while dark mode is on: its sheet with the dark-mode file, its first lines printed under it
        // before dark mode sees the chat.
        const QString skinChat = QString::fromLatin1(kSkinChat);
        auto*         b        = new QTextBrowser;
        b->setObjectName(a("ChatTab"));
        b->setOpenLinks(false);
        const QStringList lines{chatLine(a("20:00:00"), a("TextMessage_Welcome"), a("Welcome to the test server")),
                                chatLine(a("20:00:01"), a("TextMessage_Connected"), a("Connected as ") + user(a("Amy"))),
                                chatLine(a("20:00:05"), a("TextMessage_ClientJoined"), user(a("Zed")) + a(" joined your channel"))};
        if (late) {
            t.tabs->addTab(b, a("New tab"));
            t.tabs->setCurrentWidget(b);
            pump(30);
            QVERIFY(dark->isTracked(b->document()));
        }
        b->document()->setDefaultStyleSheet(skinChat + QLatin1Char('\n') + chatDark);
        appendHtml(b, lines.at(0));
        appendHtml(b, lines.at(1));
        if (!late) {
            t.tabs->addTab(b, a("New tab"));
            t.tabs->setCurrentWidget(b);
        }
        pump(80);
        QCOMPARE(b->document()->defaultStyleSheet(), dark->chatSheetFor(b->document())); // dark mode has it
        appendHtml(b, lines.at(2)); // a line while dark
        pump(80);
        // A style picked in Options -> Design meanwhile (the same one): TeamSpeak puts it in place with its dark-mode file.
        qApp->setStyleSheet(QString::fromLatin1(kSkinSheet) + QLatin1Char('\n') + appDark);
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        QCOMPARE(dark->snapshotSheet(), QString::fromLatin1(kSkinSheet));
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        QVERIFY(!DarkMode::isActive());
        QCOMPARE(qApp->styleSheet(), QString::fromLatin1(kSkinSheet));
        QCOMPARE(b->document()->defaultStyleSheet(), skinChat);
        QCOMPARE(t.visible->document()->defaultStyleSheet(), skinChat); // a chat made before: as it was
        // Every line as in a chat TeamSpeak made with the skin's palette: the colour and the weight of each piece.
        QTextDocument reference;
        reference.setDefaultStyleSheet(skinChat);
        for (const QString& line : lines) {
            QTextCursor c(&reference);
            c.movePosition(QTextCursor::End);
            if (!reference.isEmpty())
                c.insertBlock();
            c.insertHtml(line);
        }
        const auto styleAt = [](QTextDocument* doc, int position) {
            QTextCursor c(doc);
            c.setPosition(position + 1);
            const QTextCharFormat f = c.charFormat();
            return QString::fromLatin1("%1/%2").arg(f.foreground().color().name()).arg(f.fontWeight() > QFont::Normal ? 1 : 0);
        };
        for (const char* piece : {"<20:00:00>", "Welcome to", "<20:00:01>", "Connected as", "Amy", "<20:00:05>", "Zed", "joined your channel"}) {
            const int got  = findText(b->document(), a(piece));
            const int want = findText(&reference, a(piece));
            QVERIFY2(got >= 0 && want >= 0, piece);
            QVERIFY2(styleAt(b->document(), got) == styleAt(&reference, want),
                     qPrintable(a(piece) + a(": ") + styleAt(b->document(), got) + a(" want ") + styleAt(&reference, want)));
        }
        QCOMPARE(colorAt(b->document(), findText(b->document(), a("Welcome to"))), QColor(0x08, 0x7d, 0x22));
        QCOMPARE(darkchat::taggedCount(b->document()), 0);
        // A line after off: the skin's look, as TeamSpeak inserts it with the skin's sheet.
        appendHtml(b, chatLine(a("20:01:00"), a("TextMessage_ClientLeft"), user(a("Kim")) + a(" left your channel")));
        QCOMPARE(colorAt(b->document(), findText(b->document(), a("Kim"))), QColor(0x00, 0x2f, 0x5d));
        QCOMPARE(colorAt(b->document(), findText(b->document(), a("<20:01:00>"))), QColor(0x33, 0x59, 0x7d));
    }

    // ---- live test fix (2.7.3): the first line of a chat made while dark mode is on, in Cozy and Compact -----------------------
    // A private chat that came in while dark mode was on (default style: TeamSpeak made it with its dark-mode file): after off its
    // first line kept that file's colours (live: time #83acd2, name #006ad2 on the white chat), the lines after it were the
    // style's. The chat layout styles that line in one edit (the row's picture with its time and name in place of TeamSpeak's
    // text, with later lines too): the range M8 follows for the lines TeamSpeak printed under its own sheet lost all of it. 2.7.2
    // (and Classic) gave that line the style's look.
    void madeWhileDarkFirstLineLayout_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("unload");
        QTest::addColumn<bool>("together"); // the line while dark styled in the same edit
        QTest::newRow("Cozy, off") << int(layoutart::Mode::Cozy) << false << false;
        QTest::newRow("Cozy, unload") << int(layoutart::Mode::Cozy) << true << false;
        QTest::newRow("Compact, off") << int(layoutart::Mode::Compact) << false << false;
        QTest::newRow("Cozy, both lines in one edit, off") << int(layoutart::Mode::Cozy) << false << true;
        QTest::newRow("Compact, both lines in one edit, unload") << int(layoutart::Mode::Compact) << true << true;
    }
    void madeWhileDarkFirstLineLayout()
    {
        QFETCH(int, mode);
        QFETCH(bool, unload);
        QFETCH(bool, together);
        const QString chatDark = a(".TextMessage_UserLink, .TextMessage_ChannelLink { color: #006ad2; }\n"
                                   ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Time { color: #83acd2; }\n");
        QVERIFY(writeStyles({{a("default.qss"), QString::fromLatin1(kSkinSheet)},
                             {a("default_chat.qss"), QString::fromLatin1(kSkinChat)},
                             {a("default_chat_darkmode.qss"), chatDark}}));
        installSkin();
        TsWindow t    = shownWindow(10, 10);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        // A private message as TeamSpeak prints it (its icon, the time, the quoted name in its link), with its chat sheet's classes.
        const auto message = [](const QString& time, const QString& body) {
            return QString::fromLatin1("<p>%1<span class=\"TextMessage_Time\">&lt;%2&gt;</span> "
                                       "<a class=\"TextMessage_UserLink\" href=\"client://5/x~Amy\">\"Amy\"</a>: %3</p>")
                .arg(tschat::icon("MESSAGE_INCOMING"), time, body);
        };
        const QStringList lines{message(a("20:00:00"), a("Hello from a new private chat")), message(a("20:00:01"), a("A second line"))};
        const QString     skinChat = QString::fromLatin1(kSkinChat);
        auto*             b        = new QTextBrowser;
        b->setObjectName(a("ChatTab"));
        b->setOpenLinks(false);
        b->document()->setDefaultStyleSheet(skinChat + QLatin1Char('\n') + chatDark);
        appendHtml(b, lines.at(0));
        t.tabs->addTab(b, a("Amy"));
        t.tabs->setCurrentWidget(b);
        pump(80);
        QVERIFY(dark->isTracked(b->document()));
        layoutdoc::Env env = layoutEnv(true);
        env.mode           = layoutart::Mode(mode);
        env.tokens         = layoutart::tokensFor(env.mode, QApplication::font(), false);
        const auto style   = [b, &env] {
            layoutformat::LayoutEditScope editing; // as ChatLayout edits
            return layoutdoc::styleAll(b->document(), env);
        };
        if (!together)
            QVERIFY(style() > 0); // the row's picture with the time and the name at the start of the first line
        appendHtml(b, lines.at(1)); // a line while dark
        pump(80);
        if (together)
            QCOMPARE(style(), 2);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        QCOMPARE(b->document()->defaultStyleSheet(), skinChat);
        {
            layoutformat::LayoutEditScope editing; // Classic (or the chat layer's restore after an unload): the lines back
            layoutdoc::restoreAll(b->document());
        }
        QTextDocument reference;
        reference.setDefaultStyleSheet(skinChat);
        for (const QString& line : lines) {
            QTextCursor c(&reference);
            c.movePosition(QTextCursor::End);
            if (!reference.isEmpty())
                c.insertBlock();
            c.insertHtml(line);
        }
        const auto styleAt = [](QTextDocument* doc, int position) {
            QTextCursor c(doc);
            c.setPosition(position + 1);
            const QTextCharFormat f = c.charFormat();
            return QString::fromLatin1("%1/%2").arg(f.hasProperty(QTextFormat::ForegroundBrush) ? f.foreground().color().name() : a("-")).arg(f.fontWeight());
        };
        for (const char* piece : {"<20:00:00>", "\"Amy\": Hello", "Hello from", "<20:00:01>", "\"Amy\": A second", "A second"}) {
            const int got  = findText(b->document(), a(piece));
            const int want = findText(&reference, a(piece));
            QVERIFY2(got >= 0 && want >= 0, piece);
            QVERIFY2(styleAt(b->document(), got) == styleAt(&reference, want),
                     qPrintable(a(piece) + a(": ") + styleAt(b->document(), got) + a(" want ") + styleAt(&reference, want)));
        }
        QCOMPARE(colorAt(b->document(), findText(b->document(), a("<20:00:00>"))), QColor(0x33, 0x59, 0x7d));
        QCOMPARE(colorAt(b->document(), findText(b->document(), a("\"Amy\": Hello"))), QColor(0x00, 0x2f, 0x5d));
        QCOMPARE(darkchat::taggedCount(b->document()), 0);
    }

    // ---- 2.7.3 (M7): only what TeamSpeak added because of dark mode's palette ------------------------------------------------
    // TeamSpeak 3.6.2 adds its dark-mode files when the palette's window text is lighter than its window (its binary).
    void teamSpeakDarkTest()
    {
        installSkin();
        QVERIFY(!DarkMode::teamSpeakDark(QApplication::palette())); // the test skin's
        QVERIFY(DarkMode::teamSpeakDark(darkpalette::palette(QApplication::palette()))); // dark mode's
        QPalette own = QApplication::palette(); // a style with dark colours of its own (its header's palette roles)
        own.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));
        own.setColor(QPalette::WindowText, QColor(0xe0, 0xe0, 0xe0));
        QVERIFY(DarkMode::teamSpeakDark(own));
        own.setColor(QPalette::WindowText, QColor(0x20, 0x20, 0x20)); // as light as the window: not dark ("lighter than")
        QVERIFY(!DarkMode::teamSpeakDark(own));
    }

    // 2.7.2 took every text of a *_darkmode.qss off the application sheet (at on and at a skin taken over) and off every
    // document's sheet at off, whatever palette TeamSpeak had set them under: a style whose own files contain such a text, or
    // a style with dark colours of its own (TeamSpeak adds its dark-mode files to it itself), lost them, and its chats' lines
    // were mapped to the colours of the sheet without them. Now dark mode takes off only TeamSpeak's exact composition, of a
    // sheet TeamSpeak set while dark mode's palette was in place, when the style's own palette wouldn't have got the files.
    void teamSpeakDarkFilesOnlyTeamSpeaks_data()
    {
        QTest::addColumn<int>("style");
        QTest::addColumn<bool>("unload");
        // Review fix: the one case dark mode can't tell from TeamSpeak's composition, pinned as the docs state it: a style
        // whose files are another style's files with that style's dark-mode files after them, picked while dark mode is on,
        // gets the other style's own files in every chat and in TeamSpeak's window when dark mode goes off.
        const char* const names[] = {"own files end with a dark file's text", "a style with its own dark palette",
                                     "a dark-palette style picked while on over a light one",
                                     "a copy of another style's files with its dark files, picked while on"};
        for (int style = 0; style < 4; ++style) {
            for (const bool unload : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1("%1, %2").arg(QString::fromLatin1(names[style]), unload ? a("unload") : a("off")))) << style << unload;
        }
    }
    void teamSpeakDarkFilesOnlyTeamSpeaks()
    {
        QFETCH(int, style);
        QFETCH(bool, unload);
        // TeamSpeak's own style files and dark-mode files (as in teamSpeakDarkFilesGoBack), and the row's style.
        const QString chatDark   = a(".TextMessage_UserLink, .TextMessage_ChannelLink { color: #006ad2; font-weight: normal; }\n"
                                     ".TextMessage_Welcome { color: #11de4d; }\n"
                                     ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Time { color: #83acd2; }\n");
        const QString appDark    = a("QTabBar::tab { color: white; }\n");
        const QString ownAppDark = a("QTabBar::tab { color: #e0e0e0; }\n");
        const QString skinChat   = QString::fromLatin1(kSkinChat);
        const QString made       = skinChat + QLatin1Char('\n') + chatDark; // TeamSpeak's chat composition under a dark palette
        const QString r1Sheet    = QString::fromLatin1(kOtherSheet) + QLatin1Char('\n') + appDark;
        const QString r1Chat     = QString::fromLatin1(kOtherChat) + QLatin1Char('\n') + chatDark;
        const QString r2Sheet    = QString::fromLatin1(kOtherSheet);
        QVERIFY(writeStyles({{a("default.qss"), QString::fromLatin1(kSkinSheet)},
                             {a("default_chat.qss"), skinChat},
                             {a("default_chat_darkmode.qss"), chatDark},
                             {a("default_darkmode.qss"), appDark},
                             {a("r1.qss"), r1Sheet}, // a style made from a style file and a dark-mode file's text
                             {a("r1_chat.qss"), r1Chat},
                             {a("r2.qss"), r2Sheet}, // a style with its own dark-mode file and no chat file (default's pair)
                             {a("r2_darkmode.qss"), ownAppDark},
                             {a("r4.qss"), QString::fromLatin1(kSkinSheet) + QLatin1Char('\n') + appDark}, // default's files + its dark files
                             {a("r4_chat.qss"), made}}));
        const auto darkRoles = [](QPalette p) {
            p.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));
            p.setColor(QPalette::WindowText, QColor(0xe0, 0xe0, 0xe0));
            return p;
        };
        installSkin();
        // The sheets TeamSpeak sets before dark mode (under the style's own palette).
        QString appSheet = QString::fromLatin1(kSkinSheet), chatSheet = skinChat;
        if (style == 0) {
            appSheet  = r1Sheet; // no r1_darkmode.qss: TeamSpeak adds nothing
            chatSheet = r1Chat;
        } else if (style == 1) {
            QApplication::setPalette(darkRoles(QApplication::palette()));
            appSheet  = r2Sheet + QLatin1Char('\n') + ownAppDark; // TeamSpeak's own dark test is true for the style's palette
            chatSheet = made;
        }
        qApp->setStyleSheet(appSheet);
        TsWindow t = shownWindow(10, 10);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(chatSheet);
        appendHtml(t.visible, chatLine(a("19:00:00"), a("TextMessage_ClientJoined"), user(a("Ada")) + a(" joined your channel")));
        const QString oldLines = formatsOf(t.visible->document());
        auto          dark     = makeDark();
        dark->setEnabled(true);
        pump(50);
        // A chat TeamSpeak makes while dark mode is on (its composition under dark mode's palette), lines before dark mode
        // sees it and after.
        const QString     newChat = style == 0 ? r1Chat : made;
        const QStringList lines{chatLine(a("20:00:00"), a("TextMessage_Welcome"), a("Welcome to the test server")),
                                chatLine(a("20:00:01"), a("TextMessage_Connected"), a("Connected as ") + user(a("Amy"))),
                                chatLine(a("20:00:05"), a("TextMessage_ClientJoined"), user(a("Zed")) + a(" joined your channel"))};
        auto*             b = new QTextBrowser;
        b->setObjectName(a("ChatTab"));
        b->setOpenLinks(false);
        b->document()->setDefaultStyleSheet(newChat);
        appendHtml(b, lines.at(0));
        appendHtml(b, lines.at(1));
        t.tabs->addTab(b, a("New tab"));
        t.tabs->setCurrentWidget(b);
        pump(80);
        appendHtml(b, lines.at(2)); // a line while dark
        pump(80);
        // A style picked in Options -> Design while dark mode is on, as TeamSpeak's pick sets it (the application sheet with
        // its dark test on the palette in place, dark mode's; then every chat's with its test on the new palette).
        QString picked = appSheet, pickedChat = newChat;
        if (style == 0) {
            picked = r1Sheet;
        } else if (style == 3) {
            picked     = QString::fromLatin1(kSkinSheet) + QLatin1Char('\n') + appDark; // r4 has no dark-mode files of its own
            pickedChat = made;
        } else {
            picked     = r2Sheet + QLatin1Char('\n') + ownAppDark;
            pickedChat = made;
        }
        qApp->setStyleSheet(QString());
        if (style == 2)
            QApplication::setPalette(darkRoles(QApplication::palette()));
        qApp->setStyleSheet(picked);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info, static_cast<QTextEdit*>(b)})
            e->document()->setDefaultStyleSheet(pickedChat);
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        QVERIFY(!DarkMode::isActive());
        // What the style shows without dark mode: TeamSpeak's own compositions for its own palette.
        QString wantApp  = style == 2 ? r2Sheet : picked; // (row 3: the application test ran on the light style's palette)
        QString wantChat = pickedChat;                   // (row 3: the chats' test runs on the new, dark palette)
        if (style == 3) { // (the documented exception: default's own files, not r4's)
            wantApp  = QString::fromLatin1(kSkinSheet);
            wantChat = skinChat;
        }
        QCOMPARE(qApp->styleSheet(), wantApp);
        QCOMPARE(dark->snapshotSheet(), wantApp);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info, static_cast<QTextEdit*>(b)})
            QVERIFY2(e->document()->defaultStyleSheet() == wantChat, qPrintable(e->objectName()));
        if (style < 2) {
            // The lines before dark mode got exactly what they had; the new chat's lines look as in a chat TeamSpeak made
            // with that sheet.
            QVERIFY2(formatsOf(t.visible->document()).startsWith(oldLines), qPrintable(firstDiff(formatsOf(t.visible->document()), oldLines)));
            QTextDocument reference;
            reference.setDefaultStyleSheet(newChat);
            for (const QString& line : lines)
                appendTo(&reference, line);
            for (const char* piece : {"<20:00:00>", "Welcome to", "<20:00:01>", "Connected as", "Amy", "<20:00:05>", "Zed", "joined your channel"}) {
                const QString got  = describe(formatAt(b->document(), findText(b->document(), a(piece))));
                const QString want = describe(formatAt(&reference, findText(&reference, a(piece))));
                QVERIFY2(got == want, qPrintable(a(piece) + a(": ") + got + a(" want ") + want));
            }
        }
        QCOMPARE(darkchat::taggedCount(b->document()), 0);
    }

    // ---- 2.7.3 (M8): a chat made while dark mode was on, then another style picked while it is on --------------------------
    // The lines TeamSpeak printed in a new chat before dark mode saw it were mapped back from the sheet the chat had at off,
    // which after a style picked while on is the new style's: with a new style without a dark-mode file they kept the made
    // sheet's colours and weights (TeamSpeak's dark chat file's). Now they get the style shown after off, like the lines that
    // came in meanwhile.
    void darkBornChatAfterStyleChange_data()
    {
        QTest::addColumn<QString>("picked");
        QTest::addColumn<QString>("chatDark");
        QTest::addColumn<bool>("unload");
        const QString chatDark = a(".TextMessage_UserLink, .TextMessage_ChannelLink { color: #006ad2; font-weight: normal; }\n"
                                   ".TextMessage_Welcome { color: #11de4d; }\n"
                                   ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Time { color: #83acd2; }\n"
                                   ".TextMessage_ServerError { color: #ff6060; font-weight: normal; }\n");
        // Review fix: a dark chat file written like TeamSpeak's (colours only, every class of the lines coloured), picked as a
        // style of its own (the 2.7.2 live test's "darktest"): its classes have the made sheet's colours, but not its weights,
        // italics and underlines (TeamSpeak's chat file under it gives those).
        const QString colorsOnly = a(".TextMessage_UserLink, .TextMessage_OfflineLink, .TextMessage_ChannelLink, .TextMessage_ServerLink { color: #006ad2; }\n"
                                     ".TextMessage_Welcome, .TextMessage_Poke { color: #11de4d; }\n"
                                     ".TextMessage_HostName, .TextMessage_Normal, .TextMessage_Connected, .TextMessage_ClientJoined, .TextMessage_ClientLeft, "
                                     ".TextMessage_Time { color: #83acd2; }\n"
                                     ".TextMessage_BeginHistory, .TextMessage_EndHistory { color: #bcbcbc; }\n"
                                     ".TextMessage_ServerError { color: #f73c3f; }\n");
        const QVector<std::tuple<const char*, QString, QString>> styles{
            {"to a style without a dark file", QString::fromLatin1(kCommunityChat), chatDark},
            {"to a colours-only style", QString::fromLatin1(kBareChat), chatDark},
            {"the same style again", QString(), chatDark},
            {"to the dark chat file alone, the same colours", colorsOnly, colorsOnly}};
        for (const auto& style : styles) {
            for (const bool unload : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1("%1, %2").arg(QString::fromLatin1(std::get<0>(style)), unload ? a("unload") : a("off"))))
                    << std::get<1>(style) << std::get<2>(style) << unload;
        }
    }
    void darkBornChatAfterStyleChange()
    {
        QFETCH(QString, picked);
        QFETCH(QString, chatDark);
        QFETCH(bool, unload);
        const QString appDark  = a("QTabBar::tab { color: white; }\n");
        const QString skinChat = teamSpeakLikeChat(); // (every class of the lines coloured, as TeamSpeak's own chat sheet)
        const QString made     = skinChat + QLatin1Char('\n') + chatDark;
        QVERIFY(writeStyles({{a("default.qss"), QString::fromLatin1(kSkinSheet)},
                             {a("default_chat.qss"), skinChat},
                             {a("default_chat_darkmode.qss"), chatDark},
                             {a("default_darkmode.qss"), appDark}}));
        const bool    same     = picked.isEmpty();
        const QString shown    = same ? skinChat : picked;                                      // the style after off
        const QString pickedTs = same ? made : picked;                                          // what TeamSpeak sets while on
        const QString appTs    = same ? QString::fromLatin1(kSkinSheet) + QLatin1Char('\n') + appDark : QString::fromLatin1(kOtherSheet);
        installSkin();
        TsWindow t    = shownWindow(5, 0, false);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        QStringList lines = styledLines();
        // Merge check (2.7.3): a host name the community skin doesn't colour (X1) among the lines printed before dark mode
        // saw the chat (M8's way back from the sheet it was made with) and among those printed while it was on.
        lines.insert(1, lines.at(3));
        lines << chatLine(a("20:00:01"), a("TextMessage_Connected"), a("Connected as ") + user(a("Amy")))
              << chatLine(a("20:00:05"), a("TextMessage_ClientJoined"), user(a("Zed")) + a(" joined your channel"))
              << chatLine(a("20:00:09"), a("TextMessage_Normal"), a("You are now talking in channel: \"<a class=\"TextMessage_ChannelLink\" href=\"channelid://2\">Games</a>\""));
        QCOMPARE(lines.size(), 10);
        // A chat TeamSpeak makes while dark mode is on (its sheet with TeamSpeak's dark chat file), 3 lines before dark mode
        // sees it.
        auto* b = new QTextBrowser;
        b->setObjectName(a("ChatTab"));
        b->setOpenLinks(false);
        b->document()->setDefaultStyleSheet(made);
        for (int i = 0; i < 3; ++i)
            appendHtml(b, lines.at(i));
        t.tabs->addTab(b, a("New tab"));
        t.tabs->setCurrentWidget(b);
        pump(80);
        QVERIFY(dark->isTracked(b->document()));
        for (int i = 3; i < 5; ++i)
            appendHtml(b, lines.at(i));
        pump(30);
        // Another style picked in Options -> Design meanwhile: TeamSpeak's application sheet, then every chat's.
        qApp->setStyleSheet(appTs);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info, static_cast<QTextEdit*>(b)})
            e->document()->setDefaultStyleSheet(pickedTs);
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        for (int i = 5; i < 7; ++i)
            appendHtml(b, lines.at(i));
        pump(30);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        for (int i = 7; i < lines.size(); ++i)
            appendHtml(b, lines.at(i)); // after off
        QCOMPARE(b->document()->defaultStyleSheet(), shown);
        // Every piece as in a chat made with the style shown after off: colour, weight, italic, underline, size.
        const auto looks = [](QTextDocument* doc) {
            QStringList out;
            for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat f = it.fragment().charFormat();
                    out << QString::fromLatin1("%1 => %2 w%3 i%4 u%5 s%6/%7")
                               .arg(it.fragment().text(), f.foreground().color().name())
                               .arg(f.fontWeight())
                               .arg(f.fontItalic() ? 1 : 0)
                               .arg(f.fontUnderline() ? 1 : 0)
                               .arg(f.hasProperty(QTextFormat::FontPointSize) ? f.fontPointSize() : 0.0)
                               .arg(f.property(QTextFormat::FontSizeAdjustment).toInt());
                }
            }
            return out;
        };
        QTextDocument reference;
        reference.setDefaultStyleSheet(shown);
        for (const QString& line : lines)
            appendTo(&reference, line);
        const QStringList got = looks(b->document()), want = looks(&reference);
        QVERIFY2(got == want, qPrintable(firstListDiff(got, want)));
        QVERIFY(darkLeftovers(b->document()).isEmpty());
    }

    // Review fix (2.7.3): a style picked while dark mode is on makes TeamSpeak set every chat's sheet again (with its dark chat
    // file under dark mode's palette), and a line may come in during that same turn, before dark mode takes the style over:
    // it is printed under TeamSpeak's sheet. When dark mode goes off it looks like the style's own lines (2.7.2's pass over the
    // whole chat did that; the first 2.7.3 build left it in TeamSpeak's dark chat colours). Rows: a chat dark mode themed at
    // on, and one TeamSpeak made while dark mode was on.
    void lineInStylePickTurn_data()
    {
        QTest::addColumn<bool>("madeWhileOn");
        QTest::addColumn<bool>("unload");
        for (const bool made : {false, true}) {
            for (const bool unload : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1("%1, %2").arg(made ? a("a chat made while on") : a("a chat from before"), unload ? a("unload") : a("off"))))
                    << made << unload;
        }
    }
    void lineInStylePickTurn()
    {
        QFETCH(bool, madeWhileOn);
        QFETCH(bool, unload);
        const QString chatDark = a(".TextMessage_UserLink, .TextMessage_ChannelLink { color: #006ad2; font-weight: normal; }\n"
                                   ".TextMessage_Welcome { color: #11de4d; }\n"
                                   ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Time { color: #83acd2; }\n");
        const QString appDark  = a("QTabBar::tab { color: white; }\n");
        const QString skinChat = QString::fromLatin1(kSkinChat);
        const QString made     = skinChat + QLatin1Char('\n') + chatDark;
        QVERIFY(writeStyles({{a("default.qss"), QString::fromLatin1(kSkinSheet)},
                             {a("default_chat.qss"), skinChat},
                             {a("default_chat_darkmode.qss"), chatDark},
                             {a("default_darkmode.qss"), appDark}}));
        installSkin();
        TsWindow t    = shownWindow(5, 0, false);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        QTextBrowser* chat = t.visible;
        if (madeWhileOn) {
            chat = new QTextBrowser;
            chat->setObjectName(a("ChatTab"));
            chat->setOpenLinks(false);
            chat->document()->setDefaultStyleSheet(made);
            appendHtml(chat, chatLine(a("20:00:00"), a("TextMessage_Welcome"), a("Welcome to the test server")));
            t.tabs->addTab(chat, a("New tab"));
            t.tabs->setCurrentWidget(chat);
            pump(80);
            QVERIFY(dark->isTracked(chat->document()));
        }
        const QStringList lines{chatLine(a("20:00:05"), a("TextMessage_ClientJoined"), user(a("Zed")) + a(" joined your channel")),
                                chatLine(a("20:00:09"), a("TextMessage_Normal"), a("You are now talking in channel: \"<a class=\"TextMessage_ChannelLink\" href=\"channelid://2\">Games</a>\""))};
        appendHtml(chat, lines.at(0)); // a line while dark
        pump(30);
        // The same style picked again in Options -> Design: TeamSpeak's application sheet and every chat's (with the dark
        // files), and a line in that same turn.
        qApp->setStyleSheet(QString::fromLatin1(kSkinSheet) + QLatin1Char('\n') + appDark);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info, static_cast<QTextEdit*>(chat)})
            e->document()->setDefaultStyleSheet(made);
        appendHtml(chat, lines.at(1));
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        QCOMPARE(chat->document()->defaultStyleSheet(), skinChat);
        QTextDocument reference;
        reference.setDefaultStyleSheet(skinChat);
        for (const QString& line : lines)
            appendTo(&reference, line);
        for (const char* piece : {"<20:00:05>", "Zed", "joined your channel", "<20:00:09>", "You are now", "Games"}) {
            const QString got  = describe(formatAt(chat->document(), findText(chat->document(), a(piece))));
            const QString want = describe(formatAt(&reference, findText(&reference, a(piece))));
            QVERIFY2(got == want, qPrintable(a(piece) + a(": ") + got + a(" want ") + want));
        }
        QVERIFY(darkLeftovers(chat->document()).isEmpty());
    }

    // Review fix (2.7.3, M9): the dark sheet carries the style's text sizes and fonts, so a line printed while dark mode was on
    // has the size and font of the style in use then. After another style is picked while dark mode is on, such a line gets
    // the picked style's size and font when dark mode goes off (or TS Media is turned off), as 2.7.2 gave them, like its colours.
    // The info panel rendered before the pick too.
    void textSizeAfterStylePick_data()
    {
        QTest::addColumn<bool>("unload");
        QTest::newRow("off") << false;
        QTest::newRow("unload") << true;
    }
    void textSizeAfterStylePick()
    {
        QFETCH(bool, unload);
        const QString styleA = QString::fromLatin1(kSkinChat) + a("p { font-size: 7pt; font-family: \"Courier New\"; }\n");
        const QString styleB = teamSpeakLikeChat() + a("p { font-size: 14pt; font-family: \"Times New Roman\"; }\n"
                                                       ".InfoFrame_Title { font-size: 20pt; }\n");
        installSkin();
        TsWindow t = shownWindow(3, 0, false);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(styleA);
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(50);
        QStringList lines = styledLines(); // (merge check: the host name line too, X1 with M8 and M9)
        const int firstDark = t.visible->document()->characterCount() - 1;
        for (int i = 0; i < 3; ++i)
            t.visible->append(lines.at(i)); // lines while dark, before the pick
        t.info->setHtml(infoPanelHtml()); // a selection while dark
        pump(30);
        // Another style picked in Options -> Design while dark mode is on.
        qApp->setStyleSheet(QString::fromLatin1(kOtherSheet));
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(styleB);
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        for (int i = 3; i < lines.size(); ++i)
            t.visible->append(lines.at(i)); // lines while dark, after the pick
        pump(30);
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        const auto looks = [](QTextDocument* doc, int from) {
            QStringList out;
            for (QTextBlock block = doc->findBlock(from); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat f = it.fragment().charFormat();
                    if (it.fragment().position() < from)
                        continue;
                    out << QString::fromLatin1("%1 => %2 w%3 i%4 u%5 s%6/%7 f[%8]")
                               .arg(it.fragment().text(), f.hasProperty(QTextFormat::ForegroundBrush) ? f.foreground().color().name() : a("-"))
                               .arg(f.fontWeight())
                               .arg(f.fontItalic() ? 1 : 0)
                               .arg(f.fontUnderline() ? 1 : 0)
                               .arg(f.hasProperty(QTextFormat::FontPointSize) ? f.fontPointSize() : 0.0)
                               .arg(f.property(QTextFormat::FontSizeAdjustment).toInt())
                               .arg(f.fontFamilies().toStringList().join(QLatin1Char(',')) + QLatin1Char('|') + f.fontFamily());
                }
            }
            return out;
        };
        // The lines printed while dark mode was on, before and after the pick: as in a chat made with the picked style.
        QTextDocument reference;
        reference.setDefaultStyleSheet(darkchat::resolvePaletteRefs(styleB, QApplication::palette()));
        for (const QString& line : lines)
            appendTo(&reference, line);
        const QStringList got = looks(t.visible->document(), firstDark + 1), want = looks(&reference, 0);
        QVERIFY2(got == want, qPrintable(firstListDiff(got, want)));
        // The info panel rendered before the pick: the picked style's sizes and fonts too.
        QTextDocument info;
        info.setDefaultStyleSheet(darkchat::resolvePaletteRefs(styleB, QApplication::palette()));
        info.setHtml(infoPanelHtml());
        const QStringList infoGot = looks(t.info->document(), 0), infoWant = looks(&info, 0);
        QVERIFY2(infoGot == infoWant, qPrintable(a("info panel: ") + firstListDiff(infoGot, infoWant)));
    }

    // Review fix: TeamSpeak starts with dark mode on before it has built any chat (the live logs: "2 widgets"), so there was
    // nothing to measure: the dark sheet kept a default chat frame and scroll bar and no pane rule until dark mode went off
    // and on again, and the first off of the session (or a Disable or Reload) changed every chat's column (the D3 rewrap).
    // Now probes under the skin's sheet measure all three at once; the pane rule names TeamSpeak's chat tab widget class
    // until the first chat shows its own (another class: the sheet is set again once, with that one).
    void chatColumnKeptFromStart()
    {
        // A skin whose chat frame and scroll bar aren't the defaults (frame 1, the style's scroll bar).
        const QString sheet = QString::fromLatin1(kSkinSheet).replace(a("border: 1px solid #a0a0a0;"), a("border: 3px solid #a0a0a0;"))
                              + a("QTextEdit#ChatTab QScrollBar:vertical { width: 11px; }\n");
        qApp->setStyleSheet(sheet);
        QPalette p = QApplication::palette();
        p.setColor(QPalette::Link, QColor(0x1c, 0x82, 0xcc));
        QApplication::setPalette(p);
        // The skin's column and chat geometry, measured on a chat (as a toggle does).
        darkpalette::Metrics skin;
        int                  visibleWidth = 0, hiddenWidth = 0;
        {
            TsWindow ref = shownWindow(300, 300);
            ref.tabs->setCurrentIndex(1);
            pump(30);
            ref.tabs->setCurrentIndex(0);
            pump(30);
            visibleWidth  = ref.visible->viewport()->width();
            hiddenWidth   = ref.hidden->viewport()->width();
            auto measured = makeDark();
            measured->setEnabled(true);
            skin = measured->metrics();
            measured->setEnabled(false);
            delete ref.window;
            pump(30);
        }
        qInfo("skin: frame %d, scroll bar %d, pane %s left %d right %d; column %d px", skin.chatFrame, skin.chatBar, qPrintable(skin.paneSelector), skin.paneLeft,
              skin.paneRight, visibleWidth);
        QCOMPARE(skin.chatFrame, 3);
        QCOMPARE(skin.chatBar, 11);
        QCOMPARE(skin.paneSelector, a("DarkTestChatTabs"));
        QVERIFY(visibleWidth > 300 && hiddenWidth > 300);

        // TeamSpeak's start: dark mode on before any chat exists.
        auto dark = makeDark();
        dark->setEnabled(true);
        darkpalette::Metrics m = dark->metrics();
        QCOMPARE(m.chatFrame, skin.chatFrame);
        QCOMPARE(m.chatBar, skin.chatBar);
        QCOMPARE(m.paneLeft, skin.paneLeft);
        QCOMPARE(m.paneRight, skin.paneRight);
        QCOMPARE(m.paneSelector, a("ChatTabWidget")); // TeamSpeak's chat tab widget
        QVERIFY(qApp->styleSheet().contains(a("ChatTabWidget::pane { border-left: %1px solid ").arg(skin.paneLeft)));
        // TeamSpeak builds its window. Its tab widget is another class here: the sheet is set again once, naming it.
        TsWindow t = shownWindow(300, 300);
        pump(30);
        m = dark->metrics();
        QCOMPARE(m.paneSelector, a("DarkTestChatTabs"));
        QVERIFY(qApp->styleSheet().contains(a("DarkTestChatTabs::pane { border-left: %1px solid ").arg(skin.paneLeft)));
        QVERIFY(!qApp->styleSheet().contains(a("\nChatTabWidget::pane")));
        QCOMPARE(t.visible->viewport()->width(), visibleWidth); // the skin's column in dark mode
        t.tabs->setCurrentIndex(1);
        QCOMPARE(t.hidden->viewport()->width(), hiddenWidth);
        pump(30);
        t.tabs->setCurrentIndex(0);
        pump(30);
        // The session's first off: no chat changes its column (the visible one inside the off, the hidden one at its Show).
        dark->setEnabled(false);
        QCOMPARE(t.visible->viewport()->width(), visibleWidth);
        pump(30);
        t.tabs->setCurrentIndex(1);
        QCOMPARE(t.hidden->viewport()->width(), hiddenWidth);
        QCOMPARE(t.hidden->horizontalScrollBar()->maximum(), 0);
        t.tabs->setCurrentIndex(0);
        pump(30);
        delete t.window;
        pump(30);

        // A skin with a pane rule of its own for TeamSpeak's chat tab widget class: the start's probe reads that rule too.
        qApp->setStyleSheet(sheet + a("ChatTabWidget::pane { border-left: 5px solid #808080; border-right: 7px solid #808080; }\n"));
        auto own = makeDark();
        own->setEnabled(true);
        QCOMPARE(own->metrics().paneSelector, a("ChatTabWidget"));
        QCOMPARE(own->metrics().paneLeft, 5);
        QCOMPARE(own->metrics().paneRight, 7);
        own->setEnabled(false);
    }

    // ---- 2.7.3 (M6): the chat keeps the skin's height ----------------------------------------------------------------------
    // The dark sheet's bars had vertical paddings scaled with the font (design_dark 2.4) where TeamSpeak's grow with the font
    // only: dark mode made the chat 18 px shorter at 100 % and 60-73 px at 200 % (live 2.7.2: 556 -> 538, 507 -> 434), so
    // every toggle changed the chat's height. Now the menu bar, the tool bar, both tab bars, the input row, the splitter and
    // the status bar keep the skin's heights, and the chat's pages the skin's distance from the top and the tabs: the
    // visible chat, and the hidden one at its Show, are as tall (and as wide, D3) in both looks; a skin picked while dark
    // mode is on gives its own heights.
    void chatHeightKept()
    {
        // Another skin with taller bars (Options -> Design while dark mode is on): its heights without dark mode first.
        const QString taller = QString::fromLatin1(kSkinSheet) + a("QTabBar::tab { padding: 7px 12px; }\nQToolBar QToolButton { padding: 6px; }\n"
                                                                   "QMenuBar { padding: 3px 6px; }\nQMenuBar::item { padding: 5px 8px; }\nQStatusBar { min-height: 30px; }\n"
                                                                   "QToolBar { padding: 3px; }\n");
        installSkin();
        TsBars        t = shownBarsWindow(300);
        const QString skin = barGeometry(t);
        const QSize   hidden = t.hidden->viewport()->size();
        qApp->setStyleSheet(taller);
        pump(30);
        const QString tallerSkin = barGeometry(t);
        installSkin();
        pump(30);
        QCOMPARE(barGeometry(t), skin);
        qInfo("skin: %s", qPrintable(skin));
        qInfo("taller skin: %s", qPrintable(tallerSkin));
        QVERIFY(tallerSkin != skin);
        // The reader in the middle of the visible chat.
        QScrollBar* bar = t.visible->verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        pump(30);
        const chatscroll::ReaderPlace reader = chatscroll::captureReader(t.visible);

        auto dark = makeDark();
        dark->setEnabled(true);
        const darkpalette::Metrics m = dark->metrics();
        qInfo("measured: menu item %d (text %d), tool bar %d (button %d, contents %d), server tab %d, chat tab %d, input button %d (contents %d), "
              "splitters %d/%d, status %d, pane top %d bottom %d",
              m.menuItem, m.menuText, m.toolBar, m.toolButton, m.toolContent, m.serverTab, m.chatTab, m.inputButton, m.inputContent, m.splitterV,
              m.splitterH, m.status, m.paneTop, m.paneBottom);
        QVERIFY(!m.barsProbed);
        QCOMPARE(barGeometry(t), skin);
        QCOMPARE(drift(t.visible, reader), 0.0);
        pump(30);
        QCOMPARE(barGeometry(t), skin);
        t.tabs->setCurrentIndex(1); // the hidden chat's Show
        QCOMPARE(t.hidden->viewport()->size(), hidden);
        pump(30);
        t.tabs->setCurrentIndex(0);
        pump(30);
        dark->setEnabled(false);
        QCOMPARE(barGeometry(t), skin);
        QCOMPARE(drift(t.visible, reader), 0.0);
        pump(30);
        t.tabs->setCurrentIndex(1);
        QCOMPARE(t.hidden->viewport()->size(), hidden);
        t.tabs->setCurrentIndex(0);
        pump(30);

        // A skin picked while dark mode is on: its heights (measured at the adoption).
        dark->setEnabled(true);
        pump(30);
        qApp->setStyleSheet(taller);
        pump(80);
        QCOMPARE(dark->stats().adoptions, 1);
        QCOMPARE(barGeometry(t), tallerSkin);
        dark->setEnabled(false);
        pump(30);
        QCOMPARE(barGeometry(t), tallerSkin);
        installSkin();
        pump(30);
    }

    // TeamSpeak's start with dark mode on: no main window yet, the bars measured on a probe under the skin's sheet; the
    // window TeamSpeak builds then has the skin's heights in dark mode, and the session's first off changes nothing.
    // Review fix: TeamSpeak's main window exists at the plugin's load, still empty (the 2.7.2 live logs: "2 widgets; main
    // window yes"), and gets its bars afterwards: measured on the probe too (it was measured empty, nothing kept).
    // Live test fix: the probe can't know what TeamSpeak puts into its bars (the status bar's icons, the server tabs'
    // contents: live, 3-5 px and 9-15 px taller than the probe's, so the chat was 2-3 px taller until the first off): the
    // window's own bars are measured in the skin at its first Show.
    void chatHeightKeptFromStart_data()
    {
        QTest::addColumn<bool>("emptyWindow");
        QTest::newRow("no main window") << false;
        QTest::newRow("empty main window") << true;
    }

    void chatHeightKeptFromStart()
    {
        QFETCH(bool, emptyWindow);
        installSkin();
        QString    skin;
        QSize      hidden;
        QByteArray split; // the chat's height TeamSpeak saved (its main splitter's state), put back at the start
        {
            TsBars ref = shownBarsWindow(300);
            skin       = barGeometry(ref);
            hidden     = ref.hidden->viewport()->size();
            split      = ref.split->saveState();
            delete ref.window;
            pump(30);
        }
        QMainWindow* teamSpeaks = nullptr;
        if (emptyWindow) {
            teamSpeaks = new QMainWindow;
            teamSpeaks->setObjectName(a("MainWindow"));
            m_windows << teamSpeaks;
        }
        auto dark = makeDark();
        dark->setEnabled(true);
        const darkpalette::Metrics m = dark->metrics();
        qInfo("probed: menu item %d (text %d), tool bar %d (button %d, contents %d), server tab %d, chat tab %d, input button %d (contents %d), "
              "splitters %d/%d, status %d, pane top %d bottom %d",
              m.menuItem, m.menuText, m.toolBar, m.toolButton, m.toolContent, m.serverTab, m.chatTab, m.inputButton, m.inputContent, m.splitterV,
              m.splitterH, m.status, m.paneTop, m.paneBottom);
        QVERIFY(m.barsProbed);
        QVERIFY(m.menuItem > 0 && m.toolButton > 0 && m.serverTab > 0 && m.chatTab > 0 && m.inputButton > 0 && m.status > 0 && m.paneTop >= 0);
        TsBars t = shownBarsWindow(300, teamSpeaks, false, split);
        pump(50);
        QVERIFY(!dark->metrics().barsProbed);
        qInfo("skin: %s", qPrintable(skin));
        qInfo("probe's status %d, server tab %d; window's status %d, server tab %d", m.status, m.serverTab, dark->metrics().status, dark->metrics().serverTab);
        QVERIFY(m.status < dark->metrics().status && m.serverTab < dark->metrics().serverTab); // what the probe couldn't know
        QCOMPARE(barGeometry(t), skin);
        t.tabs->setCurrentIndex(1);
        QCOMPARE(t.hidden->viewport()->size(), hidden);
        t.tabs->setCurrentIndex(0);
        pump(30);
        dark->setEnabled(false);
        QCOMPARE(barGeometry(t), skin);
        pump(30);
        t.tabs->setCurrentIndex(1);
        QCOMPARE(t.hidden->viewport()->size(), hidden);
        t.tabs->setCurrentIndex(0);
        pump(30);
    }

    // Live test fix (2.7.3): TeamSpeak's window at its first Show holds one chat tab (the channel's comes with the
    // connection), and the community dark skin gives a chat tab that isn't selected a thicker border below it (5 px, the
    // selected one 3 px). Measured then, the chat tabs were the selected tab's height in dark mode: 2 px lower than the
    // skin's with the second tab, the chat 2 px taller until the first off (live: 31 against 33 px at 100 %).
    void chatTabsKeptFromStartWithOneTab()
    {
        installSkin();
        qApp->setStyleSheet(QString::fromLatin1(kSkinSheet)
                            + a("\nQTabBar::tab { border: none; padding: 6px; }\n"
                                "QTabBar::tab:selected:bottom { border-bottom: 3px solid #7289da; }\n"
                                "QTabBar::tab:!selected:bottom { border-bottom: 5px solid #36393e; }\n"));
        QString    skin;
        QByteArray split;
        {
            TsBars ref = shownBarsWindow(60);
            skin       = barGeometry(ref);
            split      = ref.split->saveState();
            delete ref.window;
            pump(30);
        }
        auto dark = makeDark();
        dark->setEnabled(true); // the start with dark mode on: the bars probed
        TsBars        t      = buildBarsWindow(60);
        const QString second = t.tabs->tabText(1);
        const QIcon   icon   = t.tabs->tabIcon(1);
        t.tabs->removeTab(1); // the first Show with one chat tab
        t.split->restoreState(split);
        m_windows << t.window;
        t.window->show();
        QApplication::setActiveWindow(t.window);
        QVERIFY(QTest::qWaitForWindowExposed(t.window));
        pump(50);
        QVERIFY(!dark->metrics().barsProbed);
        t.tabs->addTab(t.hidden, icon, second); // the channel's tab
        t.tabs->setCurrentIndex(1);
        pump(30);
        t.tabs->setCurrentIndex(0);
        pump(30);
        qInfo("skin: %s", qPrintable(skin));
        QCOMPARE(barGeometry(t), skin);
        dark->setEnabled(false);
        pump(30);
        QCOMPARE(barGeometry(t), skin);
    }

    // Review fix: the main splitter dragged to an end, so the server area (the channel tree and the info panel side by side)
    // or the chat area sits at its smallest height. Dark mode's scroll bars and the tree's frame gave those areas other
    // smallest heights (taller at 96 dpi: 106 -> 140 px for the server area), and the splitter moved the chat at every
    // switch. The splitter's areas keep the skin's smallest heights in dark mode.
    void chatHeightKeptAtSplitterEnd_data()
    {
        QTest::addColumn<bool>("serverSmallest");
        QTest::newRow("server area smallest") << true;
        QTest::newRow("chat area smallest") << false;
    }

    void chatHeightKeptAtSplitterEnd()
    {
        QFETCH(bool, serverSmallest);
        installSkin();
        TsBars t = shownBarsWindow(300, nullptr, true);
        t.split->setChildrenCollapsible(false);
        t.split->setSizes(serverSmallest ? QList<int>{1, 100000} : QList<int>{100000, 1});
        pump(30);
        const auto sizes = [&] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            return QString::fromLatin1("%1 | areas %2/%3").arg(barGeometry(t)).arg(t.split->sizes().value(0)).arg(t.split->sizes().value(1));
        };
        const QString skin   = sizes();
        const int     minimum = (serverSmallest ? t.split->widget(0) : t.split->widget(1))->minimumSizeHint().height();
        QCOMPARE(t.split->sizes().value(serverSmallest ? 0 : 1), minimum); // the area at its smallest
        QScrollBar* bar = t.visible->verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        pump(30);
        const chatscroll::ReaderPlace reader = chatscroll::captureReader(t.visible);
        qInfo("skin: %s", qPrintable(skin));
        auto dark = makeDark();
        for (int round = 0; round < 2; ++round) {
            dark->setEnabled(true);
            pump(30);
            qInfo("dark: %s", qPrintable(sizes()));
            QCOMPARE(sizes(), skin);
            QCOMPARE(drift(t.visible, reader), 0.0);
            dark->setEnabled(false);
            pump(30);
            QCOMPARE(sizes(), skin);
            QCOMPARE(drift(t.visible, reader), 0.0);
        }
        // TeamSpeak's own minimum on an area stays its own after dark mode.
        t.split->widget(1)->setMinimumHeight(120);
        dark->setEnabled(true);
        pump(30);
        dark->setEnabled(false);
        pump(30);
        QCOMPARE(t.split->widget(1)->minimumHeight(), 120);
        QCOMPARE(t.split->widget(0)->minimumHeight(), 0);
    }

    // Review fix: a start with dark mode on measures the splitter's areas only in TeamSpeak's own window (the probe has none
    // of its tree, info panel and chat). Live test fix: at that window's first Show, in the skin (it measured them only at
    // the next on: until then the splitter stopped a little earlier at its ends, and at an end the first off moved it on to
    // the skin's end, 30 px at 144 dpi live). From the start the splitter's ends are the skin's, and no switch moves it.
    void splitterEndAfterDarkStart_data()
    {
        QTest::addColumn<bool>("serverSmallest");
        QTest::newRow("server area smallest") << true;
        QTest::newRow("chat area smallest") << false;
    }

    void splitterEndAfterDarkStart()
    {
        QFETCH(bool, serverSmallest);
        installSkin();
        auto* teamSpeaks = new QMainWindow; // TeamSpeak's, still empty at the plugin's load
        teamSpeaks->setObjectName(a("MainWindow"));
        m_windows << teamSpeaks;
        auto dark = makeDark();
        dark->setEnabled(true);
        QVERIFY(dark->metrics().areas.isEmpty());
        TsBars t = shownBarsWindow(300, teamSpeaks, true);
        pump(50);
        QVERIFY(!dark->metrics().areas.isEmpty()); // measured in TeamSpeak's window at its first Show
        const int at = serverSmallest ? 0 : 1;
        t.split->setChildrenCollapsible(false);
        t.split->setSizes(serverSmallest ? QList<int>{1, 100000} : QList<int>{100000, 1});
        pump(30);
        const auto sizes = [&] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            return QString::fromLatin1("%1 | areas %2/%3").arg(barGeometry(t)).arg(t.split->sizes().value(0)).arg(t.split->sizes().value(1));
        };
        const QString start        = sizes();
        const int     darkSmallest = t.split->sizes().value(at);
        dark->setEnabled(false);
        pump(30);
        const QString off          = sizes();
        const int     skinSmallest = t.split->widget(at)->minimumSizeHint().height();
        qInfo("dark start: %s; off: %s; the area's smallest height %d in dark mode, %d in the skin", qPrintable(start), qPrintable(off), darkSmallest,
              skinSmallest);
        QCOMPARE(t.split->sizes().value(at), skinSmallest); // at the skin's end
        QCOMPARE(darkSmallest, skinSmallest);                // in dark mode too, from the start
        QCOMPARE(off, start);                                // the first off moves nothing
        for (int round = 0; round < 2; ++round) {
            dark->setEnabled(true);
            pump(30);
            QVERIFY(!dark->metrics().areas.isEmpty()); // measured in TeamSpeak's window now
            QCOMPARE(sizes(), off);
            dark->setEnabled(false);
            pump(30);
            QCOMPARE(sizes(), off);
        }
    }

    // Both at 150 % and 200 % (font DPI), each in a child process (the scale is per process).
    void chatHeightKeptScaled_data()
    {
        QTest::addColumn<QString>("dpi");
        QTest::newRow("QT_FONT_DPI=144") << a("144");
        QTest::newRow("QT_FONT_DPI=192") << a("192");
    }

    void chatHeightKeptScaled()
    {
        QFETCH(QString, dpi);
        QProcess            child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(a("QT_SCALE_FACTOR"));
        env.insert(a("QT_FONT_DPI"), dpi);
        env.insert(a("QT_QPA_FONTDIR"), QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR", a("C:/Windows"))) + a("/Fonts"));
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        const QString results = m_dir->filePath(a("heights.txt"));
        child.start(QCoreApplication::applicationFilePath(),
                    {a("-platform"), a("offscreen"), a("-o"), results + a(",txt"), a("chatHeightKept"), a("chatHeightKeptFromStart"),
                     a("chatHeightKeptAtSplitterEnd"), a("splitterEndAfterDarkStart")});
        QVERIFY(child.waitForFinished(120000));
        QFile         file(results);
        const QString out = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString::fromLocal8Bit(child.readAll());
        QVERIFY2(child.exitCode() == 0, qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::chatHeightKept()")), qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::chatHeightKeptFromStart(no main window)")), qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::chatHeightKeptFromStart(empty main window)")), qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::chatHeightKeptAtSplitterEnd(server area smallest)")), qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::chatHeightKeptAtSplitterEnd(chat area smallest)")), qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::splitterEndAfterDarkStart(server area smallest)")), qPrintable(out.right(3000)));
        QVERIFY2(out.contains(a("PASS   : TestDarkMode::splitterEndAfterDarkStart(chat area smallest)")), qPrintable(out.right(3000)));
        const QRegularExpressionMatch m = QRegularExpression(a("chatHeightKept\\(\\) skin: (.*)")).match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(m.captured(1)));
    }

    // ---- 2.7.2 (D7): the Settings window open during a toggle --------------------------------------------------------------
    // The window as plugin.cpp builds it (every section), open, dark mode on and off twice.
    // Each switch sets the window's own sheet once, after the application sheet is the new one, to exactly what a re-theme
    // gives afterwards, and lays each page out at most once. 2.7.1 re-themed at every step of the switch (on the palette with
    // the skin's sheet still in place, then on the sheet; off a third time with a button still polished by the dark sheet)
    // and each of the 8 pages laid itself out 6-8 times: the switch took 400-580 ms with the window open (live).
    void settingsWindowThemedOnce()
    {
        installSkin();
        TsWindow        t      = shownWindow(200, 100);
        SettingsDialog* dialog = settingsWindow();
        QStackedWidget* stack  = dialog->pageStack();
        QVERIFY(stack && stack->count() == 8);
        const QString skin = qApp->styleSheet();
        auto          dark = makeDark();
        for (int round = 0; round < 2; ++round) {
            for (const bool on : {true, false}) {
                const int sets  = dialog->sheetSets();
                const int pages = dialog->pageStyleChanges();
                dark->setEnabled(on);
                const QString why = QStringLiteral("round %1, dark mode %2").arg(round).arg(QString::fromLatin1(on ? "on" : "off"));
                QVERIFY2(dialog->sheetSets() - sets == 1, qPrintable(why + QStringLiteral(": %1 sets").arg(dialog->sheetSets() - sets)));
                QVERIFY2(dialog->sheetSetUnder() == qApp->styleSheet(), qPrintable(why + QStringLiteral(": set under the other sheet")));
                QCOMPARE(qApp->styleSheet() == skin, !on);
                const int laidOut = dialog->pageStyleChanges() - pages;
                QVERIFY2(laidOut <= stack->count(), qPrintable(why + QStringLiteral(": the pages laid out %1 times").arg(laidOut)));
                // Exact: the tokens it read are the final ones (a re-theme now changes nothing).
                const settingstheme::Tokens tokens = dialog->themeTokens();
                QEvent change(QEvent::StyleChange);
                QCoreApplication::sendEvent(dialog, &change);
                QCOMPARE(dialog->sheetSets() - sets, 1);
                QCOMPARE(dialog->themeTokens().window, tokens.window);
                QCOMPARE(dialog->themeTokens().card, tokens.card);
                QCOMPARE(dialog->themeTokens().accent, tokens.accent);
                QCOMPARE(dialog->themeTokens().dark, tokens.dark);
                QCOMPARE(tokens.dark, on);
                pump(20);
            }
        }
        Q_UNUSED(t);
    }

    // ---- 2.7.3 (M2): a Settings page is polished when it is first shown ------------------------------------------------------
    // Qt's repolish for a new application sheet polishes a widget once per styled ancestor. The Settings window is deep and
    // polished every page at its construction, its hidden pages too: a dark mode switch restyled its 423 widgets 5-13 times
    // each (live 2.7.2: off 423-520 ms with the window open, 197-254 closed). Now a page waits outside the window, not
    // polished (but for the spin boxes and combo boxes whose size hints a row asks for: Qt polishes them for it), until it is
    // first shown; its scroll area is made then; every control is still the window's (controls, control).
    void settingsPagesPolishedWhenShown()
    {
        installSkin();
        SettingsDialog* dialog = settingsWindow();
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::Chat);
        QStringList names; // the polished widgets polished() counted last
        const auto  polished = [&names](QWidget* page, bool all) {
            names.clear();
            int n = page->testAttribute(Qt::WA_WState_Polished) ? 1 : 0;
            for (QWidget* w : page->findChildren<QWidget*>()) {
                bool sized = false; // a spin box or combo box, or inside one (its popup too)
                for (QObject* o = w; o && o != page && !sized; o = o->parent())
                    sized = qobject_cast<QAbstractSpinBox*>(o) || qobject_cast<QComboBox*>(o);
                if (w->testAttribute(Qt::WA_WState_Polished) && (all || !sized)) {
                    ++n;
                    names << QString::fromLatin1(w->metaObject()->className()) + QLatin1Char('#') + w->objectName() + QLatin1Char('<')
                                 + QString::fromLatin1(w->parentWidget()->metaObject()->className());
                }
            }
            return n;
        };
        int unshown = 0, unshownPolished = 0;
        for (int i = 0; i < 8; ++i) {
            const auto tab  = static_cast<SettingsDialog::Tab>(i);
            QWidget*   page = dialog->page(tab);
            QVERIFY(page);
            const int n = polished(page, false);
            if (tab == SettingsDialog::Tab::Chat) {
                QVERIFY(dialog->pageAttached(tab));
                QVERIFY(n > 20);
            } else {
                QVERIFY(!dialog->pageAttached(tab));
                QVERIFY(!dialog->isAncestorOf(page));
                if (n)
                    qInfo("page %d: %d widgets polished by the page's own code (showing a widget polishes it): %s", i, n, qPrintable(names.join(QLatin1String(", "))));
                const QStringList own = names;
                unshown += 1 + page->findChildren<QWidget*>().size();
                unshownPolished += polished(page, true);
                names = own;
                // A widget shown by its section's code (Qt polishes what is shown) is the only kind polished outside spin and
                // combo boxes: Server access's rows.
                if (tab != SettingsDialog::Tab::Servers)
                    QVERIFY2(n == 0, qPrintable(QStringLiteral("page %1: %2 polished widgets: %3").arg(i).arg(n).arg(names.join(QLatin1String(", ")))));
            }
        }
        qInfo("the pages not shown yet: %d of their %d widgets polished", unshownPolished, unshown);
        QVERIFY(unshownPolished * 5 <= unshown);
        QCOMPARE(dialog->pageStack()->count(), 8);
        // Every control is the window's: the dropping switch the send settings follow (Sending), every check box.
        QCheckBox* drop = dialog->control<QCheckBox>(a("dropSendsFiles"));
        QVERIFY(drop);
        QVERIFY(dialog->page(SettingsDialog::Tab::Sending)->isAncestorOf(drop));
        QVERIFY(dialog->controls<QCheckBox>().size() > dialog->findChildren<QCheckBox*>().size());
        // Another page: attached and polished when shown, its fields as wide as in a window that opens on it.
        dialog->showTab(SettingsDialog::Tab::ReceivingPlayback);
        pump(30);
        QWidget* receiving = dialog->page(SettingsDialog::Tab::ReceivingPlayback);
        QVERIFY(dialog->pageAttached(SettingsDialog::Tab::ReceivingPlayback));
        QVERIFY(dialog->isAncestorOf(receiving));
        QVERIFY(receiving->isVisible());
        QVERIFY(polished(receiving, true) > 20);
        QCOMPARE(dialog->pageStack()->currentWidget(), static_cast<QWidget*>(dialog->pageArea(SettingsDialog::Tab::ReceivingPlayback)));
        const auto widths = [](QWidget* page) {
            QStringList out;
            for (QComboBox* combo : page->findChildren<QComboBox*>())
                out << QStringLiteral("combo %1/%2").arg(combo->minimumWidth()).arg(combo->width());
            for (QSpinBox* box : page->findChildren<QSpinBox*>())
                out << QStringLiteral("spin %1/%2").arg(box->minimumWidth()).arg(box->width());
            for (QLabel* label : page->findChildren<QLabel*>())
                out << QStringLiteral("label %1").arg(label->width());
            return out.join(QLatin1Char(' '));
        };
        SettingsDialog* first = settingsWindow(SettingsDialog::Tab::ReceivingPlayback);
        QCOMPARE(first->size(), dialog->size());
        QVERIFY(!widths(receiving).isEmpty());
        QCOMPARE(widths(receiving), widths(first->page(SettingsDialog::Tab::ReceivingPlayback)));
        // The Tab order takes in a page when it is shown.
        QList<QWidget*> chain;
        for (QWidget* w = dialog->pageList()->nextInFocusChain(); w && w != dialog->pageList() && chain.size() < 2000; w = w->nextInFocusChain())
            chain << w;
        const auto inChain = [&](SettingsDialog::Tab tab) {
            for (QWidget* w : qAsConst(chain)) {
                if (dialog->page(tab)->isAncestorOf(w))
                    return true;
            }
            return false;
        };
        QVERIFY(inChain(SettingsDialog::Tab::Chat));
        QVERIFY(inChain(SettingsDialog::Tab::ReceivingPlayback));
        QVERIFY(!inChain(SettingsDialog::Tab::Sending));
        dialog->attachAllPages();
        for (int i = 0; i < 8; ++i)
            QVERIFY(dialog->pageAttached(static_cast<SettingsDialog::Tab>(i)));
    }

    // The work of a switch with the window open on its Chat page: the style changes its widgets get (each one a polish by
    // Qt's repolish), on and off. 2.7.2: 3011 each way offscreen (every page, its hidden ones too, 5-13 times each); 2.7.3:
    // 1072-1260 (the frame, the shown page, and in the pages not shown yet the parts Qt's own size queries made styled). A
    // page shown after the switches looks as in a window opened on it in that look.
    void settingsWindowToggleWork()
    {
        installSkin();
        TsWindow        t      = shownWindow(200, 100);
        SettingsDialog* dialog = settingsWindow();
        struct Counter : QObject {
            SettingsDialog* dialog = nullptr;
            int             styles = 0;
            bool            eventFilter(QObject* o, QEvent* e) override
            {
                if (e->type() == QEvent::StyleChange && o->isWidgetType()) {
                    auto* w    = static_cast<QWidget*>(o);
                    bool  ours = w->window() == dialog;
                    for (int i = 0; i < 8 && !ours; ++i) {
                        QWidget* page = dialog->page(static_cast<SettingsDialog::Tab>(i));
                        ours          = page == w || page->isAncestorOf(w);
                    }
                    styles += ours ? 1 : 0;
                }
                return false;
            }
        } counter;
        counter.dialog = dialog;
        qApp->installEventFilter(&counter);
        auto dark = makeDark();
        for (const bool on : {true, false, true, false, true}) {
            counter.styles = 0;
            dark->setEnabled(on);
            qInfo("dark mode %s with the Settings window open (%d widgets, %d in the window): %d style changes", on ? "on" : "off",
                  dialog->controls<QWidget>().size(), dialog->findChildren<QWidget*>().size(), counter.styles);
            QVERIFY2(counter.styles <= 1500, qPrintable(QStringLiteral("%1 style changes").arg(counter.styles)));
            pump(20);
        }
        qApp->removeEventFilter(&counter);
        // Dark mode is on: a page shown now looks as in a window opened on it (the look of its texts, fields and cards).
        dialog->showTab(SettingsDialog::Tab::Sending);
        pump(30);
        SettingsDialog* fresh = settingsWindow(SettingsDialog::Tab::Sending);
        QCOMPARE(fresh->size(), dialog->size());
        const auto look = [](QWidget* page) {
            QStringList out;
            for (QWidget* w : page->findChildren<QWidget*>()) {
                if (!w->isVisible())
                    continue;
                const QPalette p = w->palette();
                out << QStringLiteral("%1 %2 %3 %4x%5").arg(QString::fromLatin1(w->metaObject()->className()), p.color(w->foregroundRole()).name(),
                                                           p.color(w->backgroundRole()).name())
                           .arg(w->width())
                           .arg(w->height());
            }
            return out;
        };
        const QStringList shown = look(dialog->page(SettingsDialog::Tab::Sending));
        QVERIFY(shown.size() > 20);
        QCOMPARE(shown, look(fresh->page(SettingsDialog::Tab::Sending)));
        dark->setEnabled(false);
        Q_UNUSED(t);
    }

    // Review fix: the same after every page has been shown once (the user looked through the window before switching). The
    // pages shown before stayed in the window, and Qt's repolish polished them again at every switch (3564 style changes,
    // as in 2.7.2): a switch takes the pages not on screen out of the window (they come back when shown, where they were
    // scrolled to, in the look of a window opened on them).
    void settingsWindowToggleWorkEveryPage()
    {
        installSkin();
        TsWindow        t      = shownWindow(200, 100);
        SettingsDialog* dialog = settingsWindow();
        for (int i = 0; i < 8; ++i) {
            dialog->showTab(static_cast<SettingsDialog::Tab>(i));
            pump(10);
        }
        QScrollArea* sending = dialog->pageArea(SettingsDialog::Tab::Sending);
        QVERIFY(sending->verticalScrollBar()->maximum() > 40);
        dialog->showTab(SettingsDialog::Tab::Sending);
        pump(10);
        sending->verticalScrollBar()->setValue(40);
        dialog->showTab(SettingsDialog::Tab::Chat);
        pump(30);
        for (int i = 0; i < 8; ++i)
            QVERIFY(dialog->pageAttached(static_cast<SettingsDialog::Tab>(i)));
        struct Counter : QObject {
            SettingsDialog* dialog = nullptr;
            int             styles = 0; // in the window
            int             outside = 0; // in the pages a switch took out
            bool            eventFilter(QObject* o, QEvent* e) override
            {
                if (e->type() == QEvent::StyleChange && o->isWidgetType()) {
                    auto* w = static_cast<QWidget*>(o);
                    if (w->window() == dialog) {
                        ++styles;
                    } else {
                        for (int i = 0; i < 8; ++i) {
                            QWidget* page = dialog->page(static_cast<SettingsDialog::Tab>(i));
                            if (page == w || page->isAncestorOf(w)) {
                                ++outside;
                                break;
                            }
                        }
                    }
                }
                return false;
            }
        } counter;
        counter.dialog = dialog;
        qApp->installEventFilter(&counter);
        auto dark  = makeDark();
        int  round = 0;
        for (const bool on : {true, false, true, false, true}) {
            counter.styles  = 0;
            counter.outside = 0;
            QElapsedTimer took;
            took.start();
            dark->setEnabled(on);
            const qint64 ms      = took.elapsed();
            const int    widgets = dialog->controls<QWidget>().size();
            const int    inside  = dialog->findChildren<QWidget*>().size();
            qInfo("dark mode %s with the Settings window open after every page was shown (%d widgets, %d in the window): %d style changes in the "
                  "window, %d in the pages taken out, %lld ms",
                  on ? "on" : "off", widgets, inside, counter.styles, counter.outside, ms);
            // The window: its frame and the shown page only (717 offscreen).
            QVERIFY2(inside < 140, qPrintable(QStringLiteral("%1 widgets in the window").arg(inside)));
            QVERIFY2(counter.styles <= 1000, qPrintable(QStringLiteral("%1 style changes in the window").arg(counter.styles)));
            // The pages taken out: a few styled ancestors each (1118 for 334 widgets offscreen; the whole window had 3011
            // before), restyled under the application sheet alone; the first switch also moves them out (Qt restyles what it
            // moves: 1356).
            QVERIFY2(counter.outside <= (round == 0 ? 5 : 4) * (widgets - inside),
                     qPrintable(QStringLiteral("%1 style changes in the pages taken out").arg(counter.outside)));
            if (round++ > 0)
                QVERIFY2(counter.styles + counter.outside <= 2100, qPrintable(QStringLiteral("%1 in all").arg(counter.styles + counter.outside)));
            pump(20);
        }
        qApp->removeEventFilter(&counter);
        QVERIFY(dialog->pageAttached(SettingsDialog::Tab::Chat));
        QVERIFY(!dialog->pageAttached(SettingsDialog::Tab::Sending));
        // Shown again: where it was read, and as in a window opened on it in this look.
        dialog->showTab(SettingsDialog::Tab::Sending);
        pump(30);
        QVERIFY(dialog->pageAttached(SettingsDialog::Tab::Sending));
        QCOMPARE(dialog->pageArea(SettingsDialog::Tab::Sending)->verticalScrollBar()->value(), 40);
        SettingsDialog* fresh = settingsWindow(SettingsDialog::Tab::Sending);
        QCOMPARE(fresh->size(), dialog->size());
        const auto look = [](QWidget* page) {
            QStringList out;
            for (QWidget* w : page->findChildren<QWidget*>()) {
                if (!w->isVisible())
                    continue;
                const QPalette p = w->palette();
                out << QStringLiteral("%1 %2 %3 %4 %5x%6")
                           .arg(QString::fromLatin1(w->metaObject()->className()), p.color(w->foregroundRole()).name(), p.color(w->backgroundRole()).name(),
                                w->font().toString())
                           .arg(w->width())
                           .arg(w->height());
            }
            return out;
        };
        const QStringList shown = look(dialog->page(SettingsDialog::Tab::Sending));
        QVERIFY(shown.size() > 20);
        QCOMPARE(shown, look(fresh->page(SettingsDialog::Tab::Sending)));
        // Every control is still the window's (its settings are read and stored from every page).
        QVERIFY(dialog->control<QCheckBox>(a("dropSendsFiles")));
        QVERIFY(dialog->controls<QCheckBox>().size() > dialog->findChildren<QCheckBox*>().size());
        dark->setEnabled(false);
        pump(20);
        Q_UNUSED(t);
    }

    // Merge check (2.7.3): the Settings pages a switch took out or never showed (M2), after TeamSpeak's style was picked while
    // dark mode was on (the skin taken over, M7). A page shown while dark mode is still on, and one shown after off, look as in
    // a window opened on it then: the picked style's palette and buttons, the window's own re-theme.
    void settingsPagesAfterStylePick()
    {
        installSkin();
        TsWindow        t      = shownWindow(200, 100);
        SettingsDialog* dialog       = settingsWindow();
        const QColor    accentBefore = dialog->themeTokens().accent; // the test skin's (the picked style gives another)
        dialog->showTab(SettingsDialog::Tab::Sending); // shown once: the switch takes it out again
        pump(20);
        dialog->showTab(SettingsDialog::Tab::Chat);
        pump(20);
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(20);
        QVERIFY(!dialog->pageAttached(SettingsDialog::Tab::Sending));
        // TeamSpeak's pick while on: the palette with the new style's header roles, then its sheet.
        const QString picked  = QString::fromLatin1(kOtherSheet) + a("QPushButton { background-color: #c8d8e8; color: #102030; }\n");
        QPalette      palette = QApplication::palette();
        palette.setColor(QPalette::Button, QColor(0xc8, 0xd8, 0xe8));
        palette.setColor(QPalette::Highlight, QColor(0x30, 0x70, 0xb0));
        QApplication::setPalette(palette);
        qApp->setStyleSheet(picked);
        pump(50);
        QVERIFY(dark->stats().adoptions >= 1);
        QVERIFY(DarkMode::isActive());
        const auto look = [](QWidget* page) {
            QStringList out;
            for (QWidget* w : page->findChildren<QWidget*>()) {
                if (!w->isVisible())
                    continue;
                const QPalette p = w->palette();
                out << QStringLiteral("%1 %2 %3 %4 %5x%6")
                           .arg(QString::fromLatin1(w->metaObject()->className()), p.color(w->foregroundRole()).name(), p.color(w->backgroundRole()).name(),
                                w->font().toString())
                           .arg(w->width())
                           .arg(w->height());
            }
            return out;
        };
        const auto sameAsFresh = [&](SettingsDialog::Tab tab, const char* when) {
            dialog->showTab(tab);
            pump(30);
            SettingsDialog* fresh = settingsWindow(tab);
            const QString   why   = QStringLiteral("%1, page %2").arg(QString::fromLatin1(when)).arg(static_cast<int>(tab));
            QVERIFY2(fresh->size() == dialog->size(), qPrintable(why));
            QVERIFY2(fresh->themeTokens().window == dialog->themeTokens().window && fresh->themeTokens().card == dialog->themeTokens().card
                         && fresh->themeTokens().text == dialog->themeTokens().text && fresh->themeTokens().accent == dialog->themeTokens().accent
                         && fresh->themeTokens().dark == dialog->themeTokens().dark,
                     qPrintable(why + QStringLiteral(": the window's tokens")));
            const QStringList shown = look(dialog->page(tab)), want = look(fresh->page(tab));
            QVERIFY2(shown.size() > 10, qPrintable(why));
            QStringList differ;
            for (int i = 0; i < qMax(shown.size(), want.size()); ++i) {
                if (shown.value(i) != want.value(i))
                    differ << shown.value(i) + QStringLiteral(" | fresh: ") + want.value(i);
            }
            QVERIFY2(differ.isEmpty(), qPrintable(why + QStringLiteral(": ") + differ.mid(0, 6).join(QStringLiteral("; "))));
            delete fresh;
            pump(10);
        };
        sameAsFresh(SettingsDialog::Tab::ReceivingPlayback, "dark mode on, never shown");
        if (QTest::currentTestFailed())
            return;
        sameAsFresh(SettingsDialog::Tab::Sending, "dark mode on, taken out by the switch");
        if (QTest::currentTestFailed())
            return;
        dialog->showTab(SettingsDialog::Tab::Chat);
        pump(20);
        dark->setEnabled(false);
        pump(30);
        QCOMPARE(qApp->styleSheet(), picked);
        QVERIFY(!dialog->themeTokens().dark);
        QVERIFY2(dialog->themeTokens().accent != accentBefore, qPrintable(dialog->themeTokens().accent.name())); // the picked style's
        QVERIFY(!dialog->pageAttached(SettingsDialog::Tab::ReceivingPlayback));
        sameAsFresh(SettingsDialog::Tab::Privacy, "after off, never shown");
        if (QTest::currentTestFailed())
            return;
        sameAsFresh(SettingsDialog::Tab::ReceivingPlayback, "after off, taken out by the switch");
        if (QTest::currentTestFailed())
            return;
        sameAsFresh(SettingsDialog::Tab::Chat, "after off, the page shown during the switches");
        Q_UNUSED(t);
    }

    // The other windows that re-theme once per switch (2.7.2 D7): the poll window and the voice panel open during switches.
    // Each widget restyled a bounded number of times (Qt polishes once per styled ancestor; these windows are shallow).
    void otherWindowsToggleWork()
    {
        installSkin();
        TsWindow t    = shownWindow(50, 20);
        auto*    poll = new PollDialog(a("the channel Lobby"));
        auto*    voice = new VoicePanel;
        for (QWidget* w : {static_cast<QWidget*>(poll), static_cast<QWidget*>(voice)}) {
            m_windows << w;
            w->show();
            QVERIFY(QTest::qWaitForWindowExposed(w));
        }
        pump(30);
        struct Counter : QObject {
            QHash<QWidget*, int> styles; // by window
            bool                 eventFilter(QObject* o, QEvent* e) override
            {
                if (e->type() == QEvent::StyleChange && o->isWidgetType())
                    ++styles[static_cast<QWidget*>(o)->window()];
                return false;
            }
        } counter;
        qApp->installEventFilter(&counter);
        auto dark = makeDark();
        for (const bool on : {true, false, true, false}) {
            counter.styles.clear();
            dark->setEnabled(on);
            for (QWidget* w : {static_cast<QWidget*>(poll), static_cast<QWidget*>(voice)}) {
                const int widgets = 1 + w->findChildren<QWidget*>().size();
                const int n       = counter.styles.value(w);
                qInfo("dark mode %s: %s, %d widgets, %d style changes", on ? "on" : "off", w->metaObject()->className(), widgets, n);
                QVERIFY2(n <= 6 * widgets, w->metaObject()->className());
            }
            pump(20);
        }
        qApp->removeEventFilter(&counter);
        Q_UNUSED(t);
    }

    // The toggle's time with the Settings window open and closed (logged; live 2.7.1: on 402 ms, off 577 ms open on a long
    // chat, 154 and 233 closed). Open, it costs one repolish of the window by the new application sheet over closed. 2.7.3
    // (M2): its shown page and its frame only (offscreen 2.7.2: +104..+126 ms over closed); the best of 5 (load only adds).
    void timingSettingsOpen()
    {
        installSkin();
        TsWindow t    = shownWindow(600, 400);
        auto     dark = makeDark();
        dark->setDeferHiddenPlaces(true);
        const auto measure = [&](qint64* on, qint64* off) {
            QVector<qint64> a, b;
            for (int i = 0; i < 5; ++i) {
                QElapsedTimer clock;
                clock.start();
                dark->setEnabled(true);
                a << clock.elapsed();
                pump(20);
                clock.restart();
                dark->setEnabled(false);
                b << clock.elapsed();
                pump(20);
            }
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            *on  = a.at(0);
            *off = b.at(0);
        };
        qint64 closedOn = 0, closedOff = 0, openOn = 0, openOff = 0;
        measure(&closedOn, &closedOff);
        SettingsDialog* dialog = settingsWindow();
        measure(&openOn, &openOff);
        qInfo("dark mode with the Settings window (%d widgets of %d) closed: on %lld ms, off %lld ms; open: on %lld ms, off %lld ms (best of 5)",
              dialog->findChildren<QWidget*>().size(), dialog->controls<QWidget>().size(), static_cast<long long>(closedOn), static_cast<long long>(closedOff),
              static_cast<long long>(openOn), static_cast<long long>(openOff));
        QVERIFY2(openOn <= 900 && openOff <= 900, "the toggle with the Settings window open");
        QVERIFY2(openOn - closedOn <= 60 && openOff - closedOff <= 60, "the Settings window's share of the toggle");
        Q_UNUSED(t);
    }

    // ---- 2.7.3 (M9): dark mode keeps the style's spacing ---------------------------------------------------------------------
    // Dark mode's chat sheet brought its own spacing (no paragraph margins, its table and cell spacing, the info title's size),
    // and what TeamSpeak inserted while it was on kept it: an info panel shown while dark until something else was selected,
    // and a chat that was empty when dark mode saw it (a new private chat, every chat of a start with dark mode on) for good,
    // because QTextEdit::append gives every new line its cursor block's format. Now the dark sheet takes the style's own
    // spacing and layout rules (review fix: borders, indents, letter spacing too): the block, table and cell formats of what
    // TeamSpeak prints while dark are the style's.
    void chatSheetKeepsSpacing()
    {
        const darkpalette::Tokens& t = darkpalette::tokens();
        QStringList                unknown;
        const QString              skin = darkpalette::chatSheet(t, QString::fromLatin1(kSkinChat), &unknown);
        QVERIFY2(unknown.isEmpty(), qPrintable(unknown.join(a(","))));
        QVERIFY(!skin.contains(QLatin1Char('@')));
        QVERIFY2(skin.contains(a("\np { padding: 0px; margin: 0px; }\n")), qPrintable(skin));
        QVERIFY2(skin.contains(a("\n.InfoFrame_Title { font-size: x-large; }\n")), qPrintable(skin));
        // None of the template's own spacing (the test skin has no table or cell rule).
        for (const char* gone : {"border-collapse", "border-spacing", "vertical-align", "td.List", "td.Badge", "Space-Top", "Space-Bot", "td { padding"})
            QVERIFY2(!skin.contains(a(gone)), gone);
        QCOMPARE(skin.count(a("font-size")), 1);
        // The template's colours and text styles stay.
        QVERIFY(skin.contains(a(".TextMessage_UserLink { color: #f5f4ee; font-weight: bold; text-decoration: none; }")));
        QVERIFY(skin.contains(a(".InfoFrame_Table td.Label { font-weight: bold; color: #c2c0b6; }")));
        QVERIFY(skin.contains(a(".TextMessage_BeginHistory, .TextMessage_EndHistory { color: #a3a198; font-weight: bold; font-style: italic; }")));
        // A colours-only style: no spacing at all, Qt's own paragraph margins.
        const QString bare = darkpalette::chatSheet(t, QString::fromLatin1(kBareChat));
        for (const char* gone : {"margin", "padding", "border-collapse", "border-spacing", "vertical-align", "font-size", "\np {"})
            QVERIFY2(!bare.contains(a(gone)), gone);
        // The community skin's spacing rules with their selectors (and a font family in quotes, a size in a shorthand).
        const QString own = QString::fromLatin1(kCommunityChat) + a(".TextMessage_Text { font-family: \"Segoe UI; Light\", Arial; }\n"
                                                                   "td.Badge { font: bold 8pt Arial; padding: 5px 5px 0px 5px; color: #101010; }\n");
        const QString community = darkpalette::chatSheet(t, own);
        QVERIFY(community.contains(a(".TextMessage_Text { font-family: \"Segoe UI; Light\", Arial; }")));
        QVERIFY(community.contains(a("td.Badge { font: bold 8pt Arial; padding: 5px 5px 0px 5px; }")));
        QVERIFY(!community.contains(a("#101010")));
        // Review fix: borders (with the style's colour: what a cell or table shows after off is exactly the style's), indents,
        // list indents, empty paragraphs, floats, widths and heights, letter and word spacing; never a text or background colour.
        const QString layoutOwn = QString::fromLatin1(kSkinChat)
                                  + a("td.X { border-bottom: 2px solid #c0c0c0; border-right-width: 3px; background-color: #ffffff; color: #101010; }\n"
                                      "p.Y { -qt-block-indent: 1; -qt-list-indent: 2; -qt-paragraph-type: empty; letter-spacing: 1px; word-spacing: 4px; }\n"
                                      "img.Z { float: right; width: 20px; height: 10px; }\n");
        const QString layout = darkpalette::chatSheet(t, layoutOwn);
        QVERIFY2(layout.contains(a("td.X { border-bottom: 2px solid #c0c0c0; border-right-width: 3px; }")), qPrintable(layout));
        QVERIFY2(layout.contains(a("p.Y { -qt-block-indent: 1; -qt-list-indent: 2; -qt-paragraph-type: empty; letter-spacing: 1px; word-spacing: 4px; }")), qPrintable(layout));
        QVERIFY2(layout.contains(a("img.Z { float: right; width: 20px; height: 10px; }")), qPrintable(layout));
        QVERIFY(!layout.contains(a("#ffffff")) && !layout.contains(a("#101010")));
        // Every sheet parses without warnings.
        g_messages.clear();
        const QtMessageHandler previous = qInstallMessageHandler(collectMessages);
        for (const QString& sheet : {skin, bare, community, layout}) {
            QTextDocument doc;
            doc.setDefaultStyleSheet(sheet);
            doc.setHtml(someLines(10, false).join(QString()) + infoPanelHtml());
        }
        qInstallMessageHandler(previous);
        for (const QString& m : qAsConst(g_messages)) {
            QVERIFY2(!m.contains(a("Could not parse"), Qt::CaseInsensitive), qPrintable(m));
            QVERIFY2(!m.contains(a("Unknown property"), Qt::CaseInsensitive), qPrintable(m));
        }
    }

    void spacingKeptInDarkMode_data()
    {
        QTest::addColumn<QString>("sheet");
        QTest::addColumn<bool>("unload");
        // Review fix: a style that draws borders around the info panel's cells and spaces its text in other ways too.
        const QString layout = teamSpeakLikeChat()
                               + a(".InfoFrame_Table td { border-bottom: 2px solid #c0c0c0; }\n"
                                   ".InfoFrame_Table td.Label { border-right-width: 3px; border-right-style: dotted; }\n"
                                   "p.TextMessage_ClientJoined { -qt-block-indent: 1; }\n"
                                   ".TextMessage_Text { letter-spacing: 1px; word-spacing: 4px; }\n");
        const QVector<QPair<const char*, QString>> skins{{"TeamSpeak's skin", teamSpeakLikeChat()},
                                                         {"community dark", QString::fromLatin1(kCommunityChat)},
                                                         {"colours only, no spacing", QString::fromLatin1(kBareChat)},
                                                         {"borders, indents and letter spacing", layout}};
        for (const auto& skin : skins) {
            for (const bool unload : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1("%1, %2").arg(QString::fromLatin1(skin.first), unload ? a("unload") : a("off"))))
                    << skin.second << unload;
        }
    }
    void spacingKeptInDarkMode()
    {
        QFETCH(QString, sheet);
        QFETCH(bool, unload);
        installSkin();
        TsWindow t = shownWindow(3, 0, false);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(t.hidden), t.info})
            e->document()->setDefaultStyleSheet(sheet);
        const QString resolved = darkchat::resolvePaletteRefs(sheet, QApplication::palette());
        // The info panel as TeamSpeak renders it under the style, and a chat made with the style (TeamSpeak's append).
        QTextDocument infoRef;
        infoRef.setDefaultFont(t.info->document()->defaultFont());
        infoRef.setDefaultStyleSheet(resolved);
        infoRef.setHtml(infoPanelHtml());
        QTextEdit         chatRef;
        QStringList       lines = styledLines();
        lines.move(3, 1); // merge check (X1 with M9): a host name the community skin doesn't colour, printed while dark
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        // A chat TeamSpeak makes while dark mode is on (a new private chat), empty when dark mode sees it.
        auto* b = new QTextBrowser;
        b->setObjectName(a("ChatTab"));
        b->setOpenLinks(false);
        b->document()->setDefaultStyleSheet(sheet);
        t.tabs->addTab(b, a("Private"));
        t.tabs->setCurrentWidget(b);
        pump(30);
        QVERIFY(dark->isTracked(b->document()));
        chatRef.document()->setDefaultFont(b->document()->defaultFont());
        chatRef.document()->setDefaultStyleSheet(resolved);
        for (int i = 0; i < 3; ++i) {
            b->append(lines.at(i));
            chatRef.append(lines.at(i));
        }
        t.info->setHtml(infoPanelHtml()); // a selection while dark
        pump(30);
        // While dark: placed as the style places them.
        QCOMPARE(heightAt(t.info->document(), 420), heightAt(&infoRef, 420));
        QCOMPARE(heightAt(b->document(), 500), heightAt(chatRef.document(), 500));
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        for (int i = 3; i < 5; ++i) {
            b->append(lines.at(i));
            chatRef.append(lines.at(i));
        }
        const QStringList info = structureOf(t.info->document()), infoWant = structureOf(&infoRef); // (2.7.3, M5: cells and separators too)
        QVERIFY2(info == infoWant, qPrintable(a("info panel: ") + firstListDiff(info, infoWant)));
        const QStringList chat = structureOf(b->document()), chatWant = structureOf(chatRef.document());
        QVERIFY2(chat == chatWant, qPrintable(a("chat: ") + firstListDiff(chat, chatWant)));
        QCOMPARE(heightAt(t.info->document(), 420), heightAt(&infoRef, 420));
        QCOMPARE(heightAt(b->document(), 500), heightAt(chatRef.document(), 500));
        const QString infoPixels = imageDiff(drawnAt(t.info->document(), 420), drawnAt(&infoRef, 420));
        QVERIFY2(infoPixels.isEmpty(), qPrintable(a("info panel: ") + infoPixels));
        const QString chatPixels = imageDiff(drawnAt(b->document(), 500), drawnAt(chatRef.document(), 500));
        QVERIFY2(chatPixels.isEmpty(), qPrintable(a("chat: ") + chatPixels));
    }

    // ---- 2.7.3 (M5): no dark look left on any format a user-visible path can read --------------------------------------------
    // QTextEdit::append gives each new block its cursor's character format and keeps that as the cursor's own; after a click
    // on a name printed while dark mode was on (TeamSpeak's chat cursor goes there) that is the name's dark look, without
    // kOriginal, which 2.7.2 mapped back only when it held kOriginal: every later separator copied it, also after off, and a
    // plain-text line appended (or text typed into a new block) then came out in dark mode's light, bold link text on the
    // light chat. TeamSpeak's own pictures and the info panel's cell formats kept dark mode's colours too. Now every block
    // character format, each view's own format, TeamSpeak's pictures and the cells are mapped back like text.
    void noDarkLookLeft_data()
    {
        QTest::addColumn<int>("way");
        QTest::newRow("off") << 0;
        QTest::newRow("unload") << 1;
        QTest::newRow("Cozy, off, then TeamSpeak classic") << 2;
    }
    void noDarkLookLeft()
    {
        QFETCH(int, way);
        const darkpalette::Tokens& k = darkpalette::tokens();
        QSet<QRgb>                 darkColours;
        for (const QColor& c : {k.text, k.secondary, k.muted, k.link, k.success, k.error, k.warning, k.accent})
            darkColours.insert(c.rgba());
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        const QUrl iconUrl(a("iconpath:MESSAGE_INFO?size=13x13"));
        // TeamSpeak's status line with its icon, a time and a name.
        const auto line = [](const char* time, const char* name) {
            return a("<p class=\"TextMessage_ClientSwitched\">") + tschat::icon("MESSAGE_INFO") + QString::fromLatin1("<span class=\"TextMessage_Time\">&lt;%1&gt;</span> ").arg(a(time))
                   + user(a(name)) + a(" switched to channel <a class=\"TextMessage_ChannelLink\" href=\"channelid://1\">\"Lobby\"</a></p>");
        };
        installSkin();
        TsWindow       t   = shownWindow(5, 0, false);
        QTextDocument* doc = t.visible->document();
        doc->addResource(QTextDocument::ImageResource, iconUrl, icon);
        // The same chat with the style only: the reference.
        QTextBrowser ref;
        ref.document()->setDefaultStyleSheet(QString::fromLatin1(kSkinChat));
        ref.document()->setDefaultFont(doc->defaultFont());
        appendLines(&ref, someLines(5, false));
        ref.document()->addResource(QTextDocument::ImageResource, iconUrl, icon);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(&ref)}) {
            e->append(line("19:00:00", "Ada"));
            e->append(line("19:00:01", "Ben"));
        }
        const int   oldBlocks = doc->blockCount();
        QStringList oldSeparators;
        for (QTextBlock b = doc->begin(); b.isValid() && b.blockNumber() < oldBlocks; b = b.next())
            oldSeparators << describe(b.charFormat());
        auto dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        const int firstDark = doc->characterCount() - 1;
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(&ref)}) {
            e->append(line("20:00:00", "Kim"));
            e->append(line("20:00:01", "Lea"));
            e->append(line("20:00:02", "Max"));
        }
        pump(30);
        // A click on a name printed while dark: TeamSpeak's chat cursor goes there.
        const int kim = findText(doc, a("Kim"), firstDark);
        QVERIFY(kim > 0);
        QCOMPARE(findText(ref.document(), a("Kim"), firstDark), kim);
        for (QTextEdit* e : {static_cast<QTextEdit*>(t.visible), static_cast<QTextEdit*>(&ref)}) {
            QTextCursor click(e->document());
            click.setPosition(kim + 1);
            e->setTextCursor(click);
            for (const char* name : {"Uma", "Ivo", "Zoe"})
                e->append(line("20:01:00", name));
        }
        t.info->setHtml(infoPanelHtml()); // the info panel rendered while dark
        pump(30);
        QVERIFY(darkColours.contains(t.visible->currentCharFormat().foreground().color().rgba())); // the name's dark look
        if (way == 2) {
            layoutformat::LayoutEditScope editing;
            QVERIFY(layoutdoc::styleAll(doc, layoutEnv(true)) >= 3);
        }
        if (way == 1)
            dark->shutdown();
        else
            dark->setEnabled(false);
        if (way == 2) {
            layoutformat::LayoutEditScope editing;
            layoutdoc::restoreAll(doc);
        }
        pump(30);
        // No block character format (a table cell's first block: the cell's format), no cell format, no view's own format and
        // no picture of TeamSpeak's has a dark look; the exports carry none.
        const auto darkIn = [&darkColours](const QTextFormat& f) {
            return f.hasProperty(QTextFormat::ForegroundBrush) && darkColours.contains(f.brushProperty(QTextFormat::ForegroundBrush).color().rgba());
        };
        QStringList left;
        for (QTextDocument* d : {doc, t.info->document()}) {
            const QString which = d == doc ? a("chat") : a("info panel");
            for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) {
                if (darkIn(b.charFormat()))
                    left << which + QString::fromLatin1(" block %1 char format %2").arg(b.blockNumber()).arg(describe(b.charFormat()));
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat f = it.fragment().charFormat();
                    if (f.isImageFormat() && !darkchat::isModuleFormat(f) && darkIn(f))
                        left << which + QString::fromLatin1(" picture at %1 %2").arg(it.fragment().position()).arg(describe(f));
                }
            }
            for (QTextFrame* frame : d->rootFrame()->childFrames()) {
                if (auto* table = qobject_cast<QTextTable*>(frame)) {
                    for (int r = 0; r < table->rows(); ++r) {
                        for (int c = 0; c < table->columns(); ++c) {
                            if (darkIn(table->cellAt(r, c).format()))
                                left << which + QString::fromLatin1(" cell %1,%2 %3").arg(r).arg(c).arg(describe(table->cellAt(r, c).format()));
                        }
                    }
                }
            }
            QTextCursor all(d);
            all.select(QTextCursor::Document);
            for (const QString& html : {d->toHtml(), all.selection().toHtml()}) {
                for (const QRgb rgb : darkColours) {
                    if (html.contains(QColor::fromRgba(rgb).name(), Qt::CaseInsensitive))
                        left << which + a(" exported with ") + QColor::fromRgba(rgb).name();
                }
            }
        }
        for (QTextEdit* view : {static_cast<QTextEdit*>(t.visible), t.info}) {
            if (darkIn(view->currentCharFormat()))
                left << view->objectName() + a(" own format ") + describe(view->currentCharFormat());
        }
        QVERIFY2(left.isEmpty(), qPrintable(left.mid(0, 6).join(a("\n"))));
        QVERIFY(darkLeftovers(doc).isEmpty());
        if (way == 2)
            return; // (the layout's restore gives the lines back in their own formats: layoutAcrossToggle compares them)
        QStringList separators;
        for (QTextBlock b = doc->begin(); b.isValid() && b.blockNumber() < oldBlocks; b = b.next())
            separators << describe(b.charFormat());
        QVERIFY2(separators == oldSeparators, qPrintable(firstListDiff(separators, oldSeparators))); // old separators byte-exact
        // A plain-text line appended after off (another plugin's, text without tags) looks as in the chat made with the style.
        t.visible->append(a("a plain line"));
        ref.append(a("a plain line"));
        const int got = findText(doc, a("a plain line")), want = findText(ref.document(), a("a plain line"));
        QCOMPARE(got, want);
        QCOMPARE(describe(formatAt(doc, got)), describe(formatAt(ref.document(), want)));
        QCOMPARE(describe(doc->findBlock(got).charFormat()), describe(ref.document()->findBlock(want).charFormat()));
        QCOMPARE(describe(t.visible->currentCharFormat()), describe(ref.currentCharFormat()));
    }

    // ---- 2.7.3 (M10): an info panel TeamSpeak made while dark mode was on ----------------------------------------------------
    // TeamSpeak makes a server tab's info panel with its chat files (its dark chat file at the end under dark mode's palette,
    // a style without a chat file of its own gets default's pair) and fills it before dark mode sees it; it may render it
    // again while dark. After off and at the unload it shows exactly what TeamSpeak renders under the style: its sheet, every
    // fragment, its frames, tables, cells and blocks, its pixels, and nothing of dark mode's bookkeeping.
    void infoPanelMadeWhileDark_data()
    {
        QTest::addColumn<int>("style");
        QTest::addColumn<bool>("again");
        QTest::addColumn<bool>("unload");
        const char* const names[] = {"TeamSpeak-like with its dark file", "community-like, no dark file", "bare, no dark file",
                                     "a style without its own chat sheet"};
        for (int style = 0; style < 4; ++style) {
            for (const bool again : {false, true}) {
                for (const bool unload : {false, true})
                    QTest::newRow(qPrintable(QString::fromLatin1("%1, %2, %3")
                                                 .arg(QString::fromLatin1(names[style]), again ? a("rendered again while dark") : a("filled before"),
                                                      unload ? a("unload") : a("off"))))
                        << style << again << unload;
            }
        }
    }
    void infoPanelMadeWhileDark()
    {
        QFETCH(int, style);
        QFETCH(bool, again);
        QFETCH(bool, unload);
        // A style's own dark chat file (synthesized) that colours the info panel's value classes only in a table cell
        // (.InfoFrame_Table td.X): the case the cell probes are for. TeamSpeak 3.6.2's own dark chat file colours both forms
        // (.Active, .InfoFrame_Table td.Active), which 2.7.2 already gave back.
        const QString chatDark = a(".TextMessage_UserLink, .TextMessage_ChannelLink { color: #006ad2; font-weight: normal; }\n"
                                   ".TextMessage_Normal, .TextMessage_ClientJoined, .TextMessage_ClientLeft, .TextMessage_Time { color: #83acd2; }\n"
                                   ".InfoFrame_Title .TextMessage_UserLink, .InfoFrame_Title .TextMessage_ServerLink { color: #d6e4f0; }\n"
                                   ".InfoFrame_Title .InfoFrame_CustomNick { color: #8cc4ff; }\n"
                                   ".InfoFrame_Table td.Highlight { color: #8cc4ff; }\n"
                                   ".InfoFrame_Table td.Important { color: #ff7a7a; }\n"
                                   ".InfoFrame_Table td.Active { color: #5ee08a; }\n"
                                   ".InfoFrame_Table td.Inactive { color: #9a9a9a; }\n");
        const QString defaultChat = teamSpeakLikeChat();
        QVERIFY(writeStyles({{a("default.qss"), QString::fromLatin1(kSkinSheet)},
                             {a("default_chat.qss"), defaultChat},
                             {a("default_chat_darkmode.qss"), chatDark},
                             {a("default_darkmode.qss"), a("QTabBar::tab { color: white; }\n")},
                             {a("community.qss"), QString::fromLatin1(kOtherSheet)},
                             {a("community_chat.qss"), QString::fromLatin1(kCommunityChat)},
                             {a("bare.qss"), QString::fromLatin1(kOtherSheet)},
                             {a("bare_chat.qss"), QString::fromLatin1(kBareChat)},
                             {a("r3.qss"), QString::fromLatin1(kOtherSheet)}}));
        // The panel's sheet as TeamSpeak makes it under dark mode's palette, and the style's own.
        QString made = defaultChat + QLatin1Char('\n') + chatDark, own = defaultChat;
        if (style == 1)
            made = own = QString::fromLatin1(kCommunityChat);
        else if (style == 2)
            made = own = QString::fromLatin1(kBareChat);
        installSkin();
        if (style == 3)
            qApp->setStyleSheet(QString::fromLatin1(kOtherSheet)); // r3: its own window style, default's chat files
        TsWindow t    = shownWindow(3, 0, false);
        auto     dark = makeDark();
        dark->setEnabled(true);
        pump(30);
        // A second server tab's info panel, made and filled while dark mode is on, before dark mode sees it.
        auto* info = new QTextEdit;
        info->setObjectName(a("InfoFrame"));
        info->setReadOnly(true);
        info->document()->setDefaultStyleSheet(made);
        info->setHtml(infoPanelHtml());
        t.tabs->addTab(info, a("Server 2"));
        t.tabs->setCurrentWidget(info);
        pump(80);
        QVERIFY(dark->isTracked(info->document()));
        if (again) {
            info->setHtml(infoPanelHtml()); // a selection while dark
            pump(30);
        }
        if (unload)
            dark->shutdown();
        else
            dark->setEnabled(false);
        pump(30);
        QCOMPARE(info->document()->defaultStyleSheet(), own);
        QTextDocument ref;
        ref.setDefaultFont(info->document()->defaultFont());
        ref.setDefaultStyleSheet(darkchat::resolvePaletteRefs(own, QApplication::palette()));
        ref.setHtml(infoPanelHtml());
        const QStringList got = fragmentsFrom(info->document(), 0), want = fragmentsFrom(&ref, 0);
        QVERIFY2(got == want, qPrintable(firstListDiff(got, want)));
        const QStringList structure = structureOf(info->document()), structureWant = structureOf(&ref);
        QVERIFY2(structure == structureWant, qPrintable(firstListDiff(structure, structureWant)));
        const QString pixels = imageDiff(drawnAt(info->document(), 420), drawnAt(&ref, 420));
        QVERIFY2(pixels.isEmpty(), qPrintable(pixels));
        QVERIFY(darkLeftovers(info->document()).isEmpty());
    }

    // The way back maps an untagged block character format with a dark look like text and leaves a skin-coloured one as it is.
    void recolorMapsSeparators()
    {
        const darkpalette::Tokens& k       = darkpalette::tokens();
        const QString              skin    = QString::fromLatin1(kSkinChat);
        const QString              darkCss = darkpalette::chatSheet(k); // (the template: the same colours and weights)
        const auto                 back    = darkchat::buildMap(darkchat::classFormats(darkCss, k.link), darkchat::classFormats(skin, QColor(0x1c, 0x82, 0xcc)), {}, QColor());
        const auto                 nameIn  = [](const QString& css) {
            QTextDocument probe;
            probe.setDefaultStyleSheet(css);
            probe.setHtml(QString::fromLatin1("<p class=\"TextMessage_Message\">%1</p>").arg(user(a("Kim"))));
            return formatAt(&probe, findText(&probe, a("Kim")));
        };
        const QTextCharFormat skinName = nameIn(skin), darkName = nameIn(darkCss);
        QTextDocument         doc;
        QTextCursor           c(&doc);
        c.insertText(a("first"));
        c.insertBlock(QTextBlockFormat(), skinName); // a separator copied from a name in the style's look
        c.insertText(a("second"), QTextCharFormat());
        c.insertBlock(QTextBlockFormat(), darkName); // ... and from one printed while dark mode was on
        c.insertText(a("third"), QTextCharFormat());
        const QString before = describe(doc.findBlockByNumber(1).charFormat());
        QVERIFY(!darkName.hasProperty(darkchat::kOriginal));
        QCOMPARE(darkchat::recolor(&doc, back, false), 1);
        QCOMPARE(describe(doc.findBlockByNumber(1).charFormat()), before);
        QCOMPARE(describe(doc.findBlockByNumber(2).charFormat()), describe(skinName));
    }

    void sourceRules()
    {
        for (const char* file : {"src/darkmode.cpp", "src/darkpalette.cpp", "src/darkchat.cpp", "src/darkglyphs.cpp", "src/darktitlebar.cpp",
                                 "src/appearancesection.cpp", "src/darkmode.h", "src/darkpalette.h", "src/darkchat.h", "src/appearancesection.h"}) {
            const QString text = sourceText(a(file));
            QVERIFY2(!text.isEmpty(), file);
            QVERIFY2(!text.contains(QRegularExpression(a("QStringLiteral\\s*\\("))), file);
            QVERIFY2(!text.contains(QRegularExpression(a("QTimer::singleShot\\s*\\("))), file); // owned timers only
            QVERIFY2(!text.contains(QRegularExpression(a("Q_DECLARE_METATYPE|qRegisterMetaType"))), file);
        }
        // The templates are read through fromLatin1 at run time.
        const QString palette = sourceText(a("src/darkpalette.cpp"));
        QVERIFY(palette.contains(a("QString::fromLatin1(kAppSheet)")));
        QVERIFY(palette.contains(a("QString::fromLatin1(kChatSheet)")));
        // The viewer and the send window keep their own caption.
        QVERIFY(sourceText(a("src/mediaviewer.cpp")).contains(a("setProperty(darktitlebar::kOwnCaption, true)")));
        QVERIFY(sourceText(a("src/composedialog.cpp")).contains(a("setProperty(darktitlebar::kOwnCaption, true)")));
    }
};

TSMEDIA_REGISTER_TEST(TestDarkMode)
#include "tst_darkmode.moc"
