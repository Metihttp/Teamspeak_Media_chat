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
#include <QImage>
#include <QLabel>
#include <QLibrary>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtTest>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#include "appearancesection.h"
#include "chatcolors.h"
#include "chatscale.h"
#include "chatscroll.h"
#include "chattextsection.h"
#include "composewidgets.h"
#include "darkchat.h"
#include "darkglyphs.h"
#include "darkmode.h"
#include "darkpalette.h"
#include "darktitlebar.h"
#include "emojiformat.h"
#include "fakets3.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "settings.h"
#include "settingsdialog.h"
#include "settingstheme.h"
#include "settingswidgets.h"
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
                         ".TextMessage_Time { color: #33597d; }\n"
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

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position + 1);
    return c.charFormat();
}

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
    t.tabs   = new QTabWidget;
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
            doc.setDefaultStyleSheet(dark->chatSheetText());
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
        QCOMPARE(t.visible->document()->defaultStyleSheet(), dark->chatSheetText());
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
        QCOMPARE(t.visible->document()->defaultStyleSheet(), dark->chatSheetText());
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
        QCOMPARE(t.visible->document()->defaultStyleSheet(), dark->chatSheetText());
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
            // (Same as the chat text: its bold prefix has the nickname's look in dark mode, and gets the nickname's colour.)
            if (*choice && QLatin1String(choice) == QLatin1String("text"))
                continue;
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
        QVERIFY(ci.contains(a("v->layoutBubbles = bubbles;\n        v->layoutAccent  = style.accent;")));
        QVERIFY(ci.contains(a("bubbles == v->layoutBubbles\n            && style.accent == v->layoutAccent)")));
        QVERIFY(ci.contains(a("surfacesKey(style) != view.layoutBubbles || style.accent != view.layoutAccent)")));
        QVERIFY(sourceText(a("src/chatintegration.h")).contains(a("QColor                  layoutAccent;")));
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
        QCOMPARE(reused->defaultStyleSheet(), dark->chatSheetText());
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
