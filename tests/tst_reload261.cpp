// R-DEF-1 (2.7.0): Options > Addons > Reload. The real plugin (this build's tsmedia DLL) is loaded into a fake
// TeamSpeak window, unloaded (ts3plugin_shutdown, then the DLL freed) and loaded again, the way TeamSpeak's Reload
// does it, with a long chat like a real one (TS Media pictures and albums, HD emoji, replies, polls, web links, own
// messages, system and day lines; or ServerQuery lines with picture links; or ServerQuery page links whose web cards an
// earlier session left in cache/links, with or without one album, like the live L14 chat; made-up people only). A chat
// at its end must be at its end again, and a reader scrolled up must see their line at the same place.
//
// Each case runs in a child process (this executable, one test function, its own TeamSpeak config folder), so the
// plugin's statics and the scaling (QT_FONT_DPI, QT_SCALE_FACTOR: per process, like the live instances) never touch
// the other tests. The child prints one line per step ("reload261: ..."), the parent checks the "measure" lines.
// The child runs with Windows' fonts (QT_QPA_FONTDIR): the offscreen platform has none of its own, and without them
// the HD emoji and the replies' reply lines never take part.

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibrary>
#include <QMainWindow>
#include <QMenuBar>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <memory>

#include "fakets3.h"
#include "blurhash.h"
#include "chatscroll.h"
#include "darkpalette.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "linkpreview/linkurl.h"
#include "medialink.h"
#include "polls.h"
#include "proxyfakes.h"
#include "testmain.h"
#include "ts3api.h"
#include "tschat.h"

#include "teamspeak/public_errors.h"

#include <windows.h> // 2.7.4 (S4): the plugin DLL's address range (PluginDll::reserveAtUnload)
#undef near // windows.h's empty near and far: this file has variables with those names
#undef far

// TeamSpeak's chat tabs (ChatIntegration attaches to a QTextBrowser below one of these class names).
class ChatTabWidget : public QTabWidget
{
    Q_OBJECT
  public:
    using QTabWidget::QTabWidget;
};

namespace {

const QString kAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");
const QString kMe    = QStringLiteral("q0Xn6Mine000000000000000000=");

// The plugin DLL as TeamSpeak sees it: its exports, loaded and freed again.
struct PluginDll {
    std::unique_ptr<QLibrary> lib;
    void (*setFunctionPointers)(const TS3Functions)                         = nullptr;
    int (*init)()                                                           = nullptr;
    void (*shutdown)()                                                      = nullptr;
    void (*registerPluginID)(const char*)                                   = nullptr;
    void (*transferStatus)(anyID, unsigned int, const char*, uint64, uint64) = nullptr;
    void (*menuItem)(uint64, enum PluginMenuType, int, uint64)               = nullptr; // 2.7.1: the Plugins menu

    QString load(const QString& path)
    {
        lib = std::make_unique<QLibrary>(path);
        if (!lib->load())
            return lib->errorString();
        setFunctionPointers = reinterpret_cast<decltype(setFunctionPointers)>(lib->resolve("ts3plugin_setFunctionPointers"));
        init                = reinterpret_cast<decltype(init)>(lib->resolve("ts3plugin_init"));
        shutdown            = reinterpret_cast<decltype(shutdown)>(lib->resolve("ts3plugin_shutdown"));
        registerPluginID    = reinterpret_cast<decltype(registerPluginID)>(lib->resolve("ts3plugin_registerPluginID"));
        transferStatus      = reinterpret_cast<decltype(transferStatus)>(lib->resolve("ts3plugin_onFileTransferStatusEvent"));
        menuItem            = reinterpret_cast<decltype(menuItem)>(lib->resolve("ts3plugin_onMenuItemEvent"));
        if (!setFunctionPointers || !init || !shutdown || !transferStatus)
            return QStringLiteral("exports missing");
        setFunctionPointers(ts3::funcs);
        HMODULE module = nullptr; // where the DLL is (its image: SizeOfImage bytes)
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(init), &module)
            && module) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(module) + dos->e_lfanew);
            base            = reinterpret_cast<quintptr>(module);
            size            = nt->OptionalHeader.SizeOfImage;
        }
        if (init() != 0)
            return QStringLiteral("init failed");
        if (registerPluginID)
            registerPluginID("tsmedia_reload261");
        return QString();
    }
    void unload()
    {
        if (shutdown)
            shutdown();
        shutdown = nullptr;
        init     = nullptr;
        if (lib)
            lib->unload();
        lib.reset();
        transferStatus = nullptr;
        menuItem       = nullptr;
        reserved       = reserveAtUnload && base && size && VirtualAlloc(reinterpret_cast<void*>(base), size, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
        base           = 0;
    }
    bool loaded() const { return lib != nullptr; }

    // 2.7.4 (S4): the loaded DLL's address and size. With reserveAtUnload (TSMEDIA_RELOAD261_MOVEDLL=1 in the environment, which
    // every child gets), unload() keeps that address range reserved, without access, for the rest of the process (`reserved`
    // says whether it could). The next load then goes to another address, so anything of this load that runs or is used after its unload (a
    // posted call, an object or a vtable that a later load or Qt reaches) faults at once. Without it Windows mostly loads the
    // DLL at the same address again, where such a leftover runs the new load's identical code and goes unseen.
    quintptr base            = 0;
    quintptr size            = 0;
    bool     reserveAtUnload = !qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_MOVEDLL");
    bool     reserved        = false;
};

QString pluginPath()
{
    const QByteArray over = qgetenv("TSMEDIA_RELOAD_DLL"); // another build (e.g. a scratch one that logs "[r261]" steps)
    if (!over.isEmpty())
        return QString::fromLocal8Bit(over);
#ifdef TSMEDIA_PLUGIN_DLL
    return QString::fromUtf8(TSMEDIA_PLUGIN_DLL);
#else
    return QString();
#endif
}

QByteArray picture(int w, int h, int seed, const char* format = "PNG")
{
    QImage image(w, h, QImage::Format_RGB32);
    QPainter p(&image);
    QLinearGradient g(0, 0, w, h);
    g.setColorAt(0, QColor::fromHsv((seed * 37) % 360, 120, 200));
    g.setColorAt(1, QColor::fromHsv((seed * 37 + 120) % 360, 160, 90));
    p.fillRect(image.rect(), g);
    p.setPen(Qt::white);
    p.drawText(image.rect(), Qt::AlignCenter, QString::number(seed));
    p.end();
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

// 2.7.3 (M3): the blurhash a sender's TS Media puts in a picture's link (made from the picture's own colours).
QString blurHashOf(int w, int h, int seed)
{
    QImage small(32, qMax(1, 32 * h / w), QImage::Format_RGB32);
    QPainter p(&small);
    QLinearGradient g(0, 0, small.width(), small.height());
    g.setColorAt(0, QColor::fromHsv((seed * 37) % 360, 120, 200));
    g.setColorAt(1, QColor::fromHsv((seed * 37 + 120) % 360, 160, 90));
    p.fillRect(small.rect(), g);
    p.end();
    return blurhash::encode(small, 4, 3);
}

// 2.7.2: a sticker-like PNG: a disc on a transparent background (the chat's placeholder shows under its clear pixels).
QByteArray transparentPicture(int w, int h, int seed)
{
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor::fromHsv((seed * 53) % 360, 200, 220));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(w * 0.15, h * 0.15, w * 0.7, h * 0.7));
    p.end();
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// A TS Media picture on the fake server, and its link as a sender's TS Media writes it.
MediaLink mediaLink(int seed, int w, int h, quint32 album = 0, int index = 0, int count = 0, bool transparent = false, bool jpeg = false)
{
    const QByteArray bytes = transparent ? transparentPicture(w, h, seed) : picture(w, h, seed, jpeg ? "JPG" : "PNG");
    MediaLink        link;
    link.host       = QString::fromLatin1(fakets3::kHost);
    link.port       = fakets3::kPort;
    link.serverUid  = QString::fromLatin1(fakets3::kServerUid);
    link.channelId  = fakets3::kChannel;
    link.path       = QStringLiteral("/tsmedia");
    link.fileName   = (jpeg ? QStringLiteral("photo%1.jpg") : QStringLiteral("photo%1.png")).arg(seed);
    link.size       = static_cast<quint64>(bytes.size());
    link.dateTime   = 1700000000 + seed;
    link.protocol   = MediaLink::kProtocol;
    link.width      = w;
    link.height     = h;
    link.sha256     = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    link.albumId    = album;
    link.albumIndex = index;
    link.albumCount = count;
    fakets3::putFile(link.remoteFile(), bytes);
    return link;
}

QString quoteHtml(const QString& nick, int clid, const QString& uid, const QString& time, const QString& snippet)
{
    return QStringLiteral("<i>↪ <a href=\"client://%1/%2~%3\">%3</a> · %4: “%5”</i>").arg(clid).arg(uid, nick, time, snippet);
}

QString pollHtml(const tschat::Skin& s, int n)
{
    polls::Poll p;
    p.key       = QStringLiteral("3f9a1c2e5b6d7e%1").arg(n, 2, 10, QLatin1Char('0'));
    p.question  = QStringLiteral("Which map tonight, round %1?").arg(n);
    p.options   = QStringList{QStringLiteral("Harbor"), QStringLiteral("Desert Town"), QStringLiteral("Night Market")};
    p.channelId = fakets3::kChannel;
    QString body = QStringLiteral("\U0001F4CA Poll: ") + tschat::escaped(p.question);
    for (int i = 0; i < p.options.size(); ++i)
        body += QStringLiteral("<br>%1. %2").arg(i + 1).arg(tschat::escaped(p.options.at(i)));
    return body + QStringLiteral("<br>")
           + tschat::linkHtml(s, QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat#") + polls::markerFragment(p), QStringLiteral("Vote with TS Media chat"));
}

QString note()
{
    return QStringLiteral(" <span style=\"color:#72767d\">— TS Media chat plugin required to view this in chat</span>");
}

QString cardUrl(int i)
{
    return QStringLiteral("https://news.example.com/perf/story-%1?r=261").arg(i);
}

// The "cards" chat's web cards as an earlier session left them in cache/links (the plugin shows them from there with
// no request): a title, a description and a picture, large or as a thumbnail, like the live L14 chat's loaded cards.
bool seedCards(const QString& linksDir, int messages)
{
    if (!QDir().mkpath(linksDir))
        return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 1; i <= messages; ++i) {
        if (i % 10 == 0)
            continue;
        const linkpreview::Target target = linkpreview::classify(cardUrl(i));
        if (!target.isNewClass() || target.key.isEmpty())
            return false;
        const bool    large = i % 3 != 0;
        const QSize   pic   = large ? QSize(1200, 630) : QSize(330, 596);
        const QString thumb = target.key + QStringLiteral("-thumb.jpg");
        {
            QImage image(pic, QImage::Format_RGB32);
            image.fill(QColor::fromHsv((i * 29) % 360, 110, 170));
            if (!image.save(linksDir + QLatin1Char('/') + thumb, "JPG", 80))
                return false;
        }
        QJsonObject card;
        card.insert(QStringLiteral("site"), QStringLiteral("Example News"));
        card.insert(QStringLiteral("author"), QString());
        card.insert(QStringLiteral("title"), QStringLiteral("Story %1: a made-up page about Alice and Bob").arg(i));
        card.insert(QStringLiteral("desc"), QStringLiteral("A short description of story %1, long enough to take two lines of the card in a chat of "
                                                           "ordinary width, as most pages' descriptions do.")
                                                .arg(i));
        card.insert(QStringLiteral("pic"), thumb);
        card.insert(QStringLiteral("pw"), pic.width());
        card.insert(QStringLiteral("ph"), pic.height());
        card.insert(QStringLiteral("layout"), large ? QStringLiteral("large") : QStringLiteral("thumb"));
        card.insert(QStringLiteral("sens"), false);
        card.insert(QStringLiteral("play"), false);
        card.insert(QStringLiteral("d"), 0.0);
        QJsonObject meta;
        meta.insert(QStringLiteral("v"), 2);
        meta.insert(QStringLiteral("cls"), QString::fromLatin1(linkpreview::className(target.cls)));
        meta.insert(QStringLiteral("at"), static_cast<double>(now - 60));
        meta.insert(QStringLiteral("kind"), QStringLiteral("card"));
        meta.insert(QStringLiteral("mime"), QStringLiteral("image/jpeg"));
        meta.insert(QStringLiteral("thumb"), thumb);
        meta.insert(QStringLiteral("card"), card);
        QFile file(linksDir + QLatin1Char('/') + target.key + QStringLiteral(".json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact)) <= 0)
            return false;
    }
    return true;
}

// A long chat as TeamSpeak shows it after a while: history, day lines, every kind of message ("mixed"); ServerQuery
// lines with a picture link each, shown as a "Load preview" strip, and a few plain lines ("text"); or the live L14
// chat's kind: ServerQuery page links whose cards show from cache/links and a few plain lines ("cards"), plus one TS
// Media album of four pictures early in it ("album").
void fillChat(QTextDocument* doc, int messages, const QByteArray& kind)
{
    const tschat::Skin s = tschat::darkSkin();
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
    tschat::append(doc, tschat::historyHtml(s, QStringLiteral("2026-11-13 20:10:00")));
    tschat::append(doc, tschat::dayHtml(s, QStringLiteral("11/13/2026")));
    tschat::append(doc, tschat::systemHtml(s, QStringLiteral("20:10:00"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Fake channel"))));
    const QStringList emoji = {QStringLiteral("\U0001F600"), QStringLiteral("\U0001F44D"), QStringLiteral("\U0001F389"), QStringLiteral("❤️"),
                               QStringLiteral("\U0001F602")};
    int seed = 1;
    for (int i = 1; i <= messages; ++i) {
        if (i == messages / 2)
            tschat::append(doc, tschat::dayHtml(s, QStringLiteral("11/14/2026")));
        const int     minute = 600 + i;
        const QString time   = QStringLiteral("%1:%2:%3").arg(minute / 60 % 24, 2, 10, QLatin1Char('0')).arg(minute % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
        if (kind == "cards" || kind == "album") { // the live L14 chat: ServerQuery page links whose cards were loaded before
            if (kind == "album" && i == 5) { // "album": one TS Media album of four pictures early in it (three collapsed links)
                QString body;
                for (int k = 1; k <= 4; ++k) {
                    const MediaLink link = mediaLink(seed++, 640, 480, 0x1a2b3d05u, k, 4);
                    body += (k > 1 ? QStringLiteral("<br>") : QString()) + tschat::linkHtml(s, link.toUrl(), link.fileName);
                }
                tschat::append(doc, tschat::messageHtml(s, time, 19, kBob, QStringLiteral("Bob"), body + note()));
            } else if (i % 10 == 0) {
                tschat::append(doc, tschat::messageHtml(s, time, 17, kAlice, QStringLiteral("Alice"), QStringLiteral("r261 %1 ok, reading").arg(i)));
            } else {
                tschat::append(doc, tschat::messageHtml(s, time, 1, QStringLiteral("serveradmin"), QStringLiteral("serveradmin"),
                                                        QStringLiteral("r261 %1 SQ perf ").arg(i) + tschat::linkHtml(s, cardUrl(i))));
            }
            continue;
        }
        if (kind == "text") {
            if (i % 10 == 0) {
                tschat::append(doc, tschat::messageHtml(s, time, 17, kAlice, QStringLiteral("Alice"), QStringLiteral("r261 %1 ok, reading").arg(i)));
            } else {
                const QString url = QStringLiteral("https://cdn.example.com/perf/%1.png?r=261").arg(i);
                tschat::append(doc, tschat::messageHtml(s, time, 1, QStringLiteral("serveradmin"), QStringLiteral("serveradmin"),
                                                        QStringLiteral("r261 %1 SQ perf ").arg(i) + tschat::linkHtml(s, url)));
            }
            continue;
        }
        const bool    own    = i % 5 == 0;
        const bool    bob    = !own && i % 3 == 0;
        const int     clid   = own ? fakets3::kOwnClient : bob ? 19 : 17;
        const QString uid    = own ? kMe : bob ? kBob : kAlice;
        const QString nick   = own ? QStringLiteral("Sam") : bob ? QStringLiteral("Bob") : QStringLiteral("Alice");
        QString       body;
        if (i % 17 == 0) { // an album of three pictures
            const quint32 album = 0x1a2b3c00u + static_cast<quint32>(i);
            for (int k = 1; k <= 3; ++k) {
                const MediaLink link = mediaLink(seed++, 640, 480, album, k, 3);
                body += (k > 1 ? QStringLiteral("<br>") : QString()) + tschat::linkHtml(s, link.toUrl(), link.fileName);
            }
            body += note();
        } else if (i % 9 == 0) { // a picture
            const MediaLink link = i % 2 ? mediaLink(seed++, 800, 450) : mediaLink(seed++, 360, 640);
            body = QStringLiteral("look r261 %1<br>").arg(i) + tschat::linkHtml(s, link.toUrl(), link.fileName) + note();
        } else if (i % 11 == 0) { // a web page (no slot while it waits; automatic loading is off: no request)
            const QString url = QStringLiteral("https://news.example.com/story-%1").arg(i);
            body = QStringLiteral("r261 %1 read this ").arg(i) + tschat::linkHtml(s, url);
        } else if (i % 7 == 0) { // a reply
            body = quoteHtml(QStringLiteral("Bob"), 19, kBob, QStringLiteral("10∶%1").arg(i % 60, 2, 10, QLatin1Char('0')), QStringLiteral("earlier words"))
                   + QStringLiteral("<br>r261 %1 sure, as always %2").arg(i).arg(emoji.at(i % emoji.size()));
        } else if (i == 40 || i == 130) {
            body = pollHtml(s, i);
        } else if (i % 4 == 0) {
            body = QStringLiteral("r261 %1 %2%2 that was great %3").arg(i).arg(emoji.at(i % emoji.size()), emoji.at((i + 1) % emoji.size()));
        } else if (i % 6 == 1) {
            body = QStringLiteral("r261 %1 a longer message that wraps over more than one line in the chat, so its bubble is taller than the others "
                                  "and the reader's place falls inside it")
                       .arg(i);
        } else {
            body = QStringLiteral("r261 %1 ok").arg(i);
        }
        if (i % 23 == 0)
            tschat::append(doc, tschat::systemHtml(s, time, tschat::clientHtml(s, 21, QStringLiteral("q0Xn6Dex0000000000000000000="), QStringLiteral("Dex"))
                                                                 + QStringLiteral(" connected")));
        tschat::append(doc, tschat::messageHtml(s, time, clid, uid, nick, body, own));
    }
}

struct Where {
    int     value   = 0;
    int     maximum = 0;
    int     block   = -1;
    qreal   offset  = 0;
    QString text;
};

Where whereOf(QTextBrowser* b)
{
    Where             w;
    QScrollBar*       bar = b->verticalScrollBar();
    w.value               = bar->value();
    w.maximum             = bar->maximum();
    const layoutdoc::Anchor a = layoutdoc::anchorAt(b->document(), bar->value());
    w.block                   = a.block;
    w.offset                  = a.offset;
    w.text                    = b->document()->findBlockByNumber(a.block).text().left(40).replace(QChar(0xfffc), QLatin1Char('#'));
    return w;
}

// The end as the laid-out chat has it (lays the whole chat out; only where nothing else runs).
int trueMaximum(QTextBrowser* b)
{
    return qMax(0, qRound(b->document()->documentLayout()->documentSize().height()) - b->viewport()->height());
}

// The reader's line: the newest block holding `marker`, its top relative to the view's top (y on screen), or a huge
// value when it is gone or hidden.
qreal lineY(QTextBrowser* b, const QString& marker)
{
    QTextBlock tb = b->document()->lastBlock();
    while (tb.isValid() && !tb.text().contains(marker))
        tb = tb.previous();
    if (!tb.isValid() || !tb.isVisible())
        return 1e9;
    return b->document()->documentLayout()->blockBoundingRect(tb).top() - b->verticalScrollBar()->value();
}

QString describe(const Where& w)
{
    return QStringLiteral("scroll %1/%2 gap %3 top block %4 offset %5 \"%6\"")
        .arg(w.value)
        .arg(w.maximum)
        .arg(w.maximum - w.value)
        .arg(w.block)
        .arg(w.offset, 0, 'f', 1)
        .arg(w.text);
}

// ---- 2.7.1: the cards TS Media draws, and dark mode ---------------------------------------------------------------------

// Any file of a TS Media message on the fake server, and its link (the kind's own fields set by the caller).
MediaLink serverLink(const QString& fileName, const QByteArray& bytes, int seed)
{
    MediaLink link;
    link.host      = QString::fromLatin1(fakets3::kHost);
    link.port      = fakets3::kPort;
    link.serverUid = QString::fromLatin1(fakets3::kServerUid);
    link.channelId = fakets3::kChannel;
    link.path      = QStringLiteral("/tsmedia");
    link.fileName  = fileName;
    link.size      = static_cast<quint64>(bytes.size());
    link.dateTime  = 1700000000 + seed;
    link.protocol  = MediaLink::kProtocol;
    link.sha256    = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    fakets3::putFile(link.remoteFile(), bytes);
    return link;
}

QByteArray someBytes(int size, int seed)
{
    QByteArray bytes(size, '\0');
    for (int i = 0; i < size; ++i)
        bytes[i] = static_cast<char>((i * 31 + seed * 7) & 0xff);
    return bytes;
}

// The kind of each card's picture by its key (the child's lines name them).
QHash<QString, QString>& cardKinds()
{
    static QHash<QString, QString> kinds;
    return kinds;
}

// A chat with every kind of card TS Media draws as a picture of its own: a picture, an album of three, a hidden spoiler
// picture, a file card, an audio card, a voice message and a video (its poster on the server), among `messages` lines
// of text (some wrap); the cards come after message `cardsAt` (and `sets` - 1 more times, every 10 messages: a timing run,
// TSMEDIA_DARKCARDS_SETS). Made-up people only. TSMEDIA_DARKCARDS_BIG=1 (2.7.1 review): the picture and the spoiler are
// 2400 x 1800 PNGs (over 4 MP: Core decodes their stills on a worker), so a long chat at 200 % holds more stills than
// Core's cache keeps (the album's stay small: a grid off screen isn't drawn again when its tiles arrive later).
// TSMEDIA_DARKCARDS_HUGE=1 (2.7.2): the picture and the spoiler are 4000 x 3000 (12 MP, never decoded on the GUI thread at
// the unload), and each set has a transparent 1600 x 1600 sticker too. 2.7.3: pictures "huge" or "cheap" (1600 x 1200 JPEGs:
// under every limit of a decode on the GUI thread) for this chat only, whatever the environment says; with
// TSMEDIA_DARKCARDS_BLURHASH=1 the picture's and the spoiler's links carry a blurhash (their stand-in while a worker reads
// the file).
void fillCardChat(QTextDocument* doc, int messages, int cardsAt, int* seed, int sets = 1, const QByteArray& pictures = QByteArray())
{
    const tschat::Skin s     = tschat::lightSkin();
    const bool         cheap = pictures == "cheap";
    const bool         huge  = pictures == "huge" || (pictures.isEmpty() && qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_HUGE") == 1);
    const bool         big   = huge || cheap || (pictures.isEmpty() && qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_BIG") == 1);
    const bool         hash  = qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_BLURHASH") == 1;
    const int          w     = huge ? 4000 : cheap ? 1600 : big ? 2400 : 640, h = huge ? 3000 : cheap ? 1200 : big ? 1800 : 480;
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
    tschat::append(doc, tschat::systemHtml(s, QStringLiteral("20:10:00"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Fake channel"))));
    const auto post = [doc, &s](int i, const QString& body) {
        const QString time = QStringLiteral("21:%1:%2").arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
        const bool    bob  = i % 2 == 0;
        tschat::append(doc, tschat::messageHtml(s, time, bob ? 19 : 17, bob ? kBob : kAlice, bob ? QStringLiteral("Bob") : QStringLiteral("Alice"), body));
    };
    for (int i = 1; i <= messages; ++i) {
        post(i, i % 4 == 1 ? QStringLiteral("dark %1 a longer message that wraps over more than one line in the chat, so the reader's place can fall "
                                              "inside it, and a few more words")
                                 .arg(i)
                           : QStringLiteral("dark %1 ok").arg(i));
        if (i < cardsAt || (i - cardsAt) % 10 != 0 || (i - cardsAt) / 10 >= sets)
            continue;
        const auto card = [&](const MediaLink& link, const char* kind) {
            cardKinds().insert(link.key(), QString::fromLatin1(kind));
            return tschat::linkHtml(s, link.toUrl(), link.fileName) + note();
        };
        {
            MediaLink photo = mediaLink(*seed, w, h, 0, 0, 0, false, cheap);
            if (hash)
                photo.blurHash = blurHashOf(w, h, *seed);
            ++*seed;
            post(1000, card(photo, "picture"));
        }
        if (huge)
            post(1000, card(mediaLink((*seed)++, 1600, 1600, 0, 0, 0, true), "sticker"));
        QString album;
        const quint32 id = 0x5d4c3b00u + static_cast<quint32>(*seed);
        for (int k = 1; k <= 3; ++k) {
            const MediaLink link = mediaLink((*seed)++, 640, 480, id, k, 3);
            cardKinds().insert(link.key(), QStringLiteral("albumitem"));
            album += (k > 1 ? QStringLiteral("<br>") : QString()) + tschat::linkHtml(s, link.toUrl(), link.fileName);
        }
        post(1001, album + note());
        {
            const int        pictureSeed = (*seed)++;
            const QByteArray bytes       = big ? picture(w, h, pictureSeed, cheap ? "JPG" : "PNG") : picture(480, 360, pictureSeed);
            MediaLink hidden = serverLink((cheap ? QStringLiteral("secret_%1.jpg") : QStringLiteral("secret_%1.png")).arg(*seed), bytes, *seed);
            hidden.width     = big ? w : 480;
            hidden.height    = big ? h : 360;
            hidden.spoiler   = true;
            if (hash)
                hidden.blurHash = blurHashOf(hidden.width, hidden.height, pictureSeed);
            post(1002, card(hidden, "spoiler"));
        }
        post(1003, card(serverLink(QStringLiteral("notes_%1.pdf").arg(*seed), someBytes(9000, *seed), *seed), "file"));
        {
            MediaLink song  = serverLink(QStringLiteral("song_%1.mp3").arg(*seed), someBytes(12000, *seed), *seed);
            song.durationMs = 185000;
            post(1004, card(song, "audio"));
            ++*seed;
        }
        {
            MediaLink voice  = serverLink(QStringLiteral("voice_%1.m4a").arg(*seed), someBytes(6000, *seed), *seed);
            voice.voice      = true;
            voice.durationMs = 4200;
            for (int k = 0; k < MediaLink::kWaveformLevels; ++k)
                voice.waveform.append(static_cast<char>((k * 5 + *seed) % (MediaLink::kWaveformMax + 1)));
            post(1005, card(voice, "voice"));
            ++*seed;
        }
        {
            const QByteArray poster = picture(640, 360, *seed);
            const QString    pv     = QStringLiteral("/tsmedia/previews/clip_%1.png").arg(*seed);
            fakets3::putFile(pv, poster);
            MediaLink clip   = serverLink(QStringLiteral("clip_%1.mp4").arg(*seed), someBytes(20000, *seed), *seed);
            clip.width       = 640;
            clip.height      = 360;
            clip.durationMs  = 12000;
            clip.previewFile = pv;
            post(1006, card(clip, "video"));
            ++*seed;
        }
    }
}

// Every picture of ours in a chat (an image named "tsmedia:..."): its name, size, the SHA-1 of its pixels, and whether it
// has pixels of a card drawn for a dark chat (#2b2d31, #232428: previewrenderer's dark card and placeholder) or of dark
// mode's accent (within 6 of #d97757). Blank stand-ins (1 x 1) are marked.
QStringList cardLines(QTextBrowser* b)
{
    QSet<QString> names;
    for (QTextBlock block = b->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (f.isImageFormat() && f.toImageFormat().name().startsWith(QLatin1String("tsmedia:")))
                names.insert(f.toImageFormat().name());
        }
    }
    QStringList sorted(names.begin(), names.end());
    std::sort(sorted.begin(), sorted.end());
    QStringList out;
    for (const QString& name : qAsConst(sorted)) {
        const QVariant v = b->document()->resource(QTextDocument::ImageResource, QUrl(name));
        QImage         img;
        if (v.userType() == QMetaType::QPixmap)
            img = qvariant_cast<QPixmap>(v).toImage();
        else if (v.userType() == QMetaType::QImage)
            img = qvariant_cast<QImage>(v);
        const QString id = cardKinds().value(name.mid(8), QStringLiteral("grid")) + QLatin1Char('-')
                           + QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
        if (img.isNull() || img.width() <= 1 || img.height() <= 1) {
            out << QStringLiteral("%1 blank").arg(id);
            continue;
        }
        img = img.convertToFormat(QImage::Format_ARGB32);
        bool dark = false, accent = false;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QRgb px = line[x] & 0xffffffu;
                dark          = dark || px == 0x2b2d31u || px == 0x232428u;
                accent = accent || (qAlpha(line[x]) == 255 && qAbs(qRed(px) - 0xd9) <= 6 && qAbs(qGreen(px) - 0x77) <= 6 && qAbs(qBlue(px) - 0x57) <= 6);
            }
        }
        const QByteArray bits(reinterpret_cast<const char*>(img.constBits()), img.sizeInBytes());
        out << QStringLiteral("%1 %2x%3 %4 dark=%5 accent=%6")
                   .arg(id)
                   .arg(img.width())
                   .arg(img.height())
                   .arg(QString::fromLatin1(QCryptographicHash::hash(bits, QCryptographicHash::Sha1).toHex().left(16)))
                   .arg(dark ? 1 : 0)
                   .arg(accent ? 1 : 0);
    }
    return out;
}

// 2.7.1 review: the pixmap each picture of ours has now (QPixmap::cacheKey): a picture put again gets another one, even
// with the same pixels. "ID KEY" lines.
QStringList cardKeys(QTextBrowser* b)
{
    QSet<QString> names;
    for (QTextBlock block = b->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (f.isImageFormat() && f.toImageFormat().name().startsWith(QLatin1String("tsmedia:")))
                names.insert(f.toImageFormat().name());
        }
    }
    QStringList out;
    for (const QString& name : qAsConst(names)) {
        const QVariant v  = b->document()->resource(QTextDocument::ImageResource, QUrl(name));
        const QString  id = cardKinds().value(name.mid(8), QStringLiteral("grid")) + QLatin1Char('-')
                           + QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
        out << QStringLiteral("%1 %2").arg(id).arg(v.userType() == QMetaType::QPixmap ? qvariant_cast<QPixmap>(v).cacheKey() : 0);
    }
    out.sort();
    return out;
}

// "reload261: card PHASE CHAT ID WxH SHA dark=D accent=A" (or "ID blank") -> per phase and chat: id -> "WxH SHA dark=D accent=A".
using CardMap = QHash<QString, QHash<int, QMap<QString, QString>>>;
CardMap cardsOf(const QString& out)
{
    static const QRegularExpression re(QStringLiteral("^reload261: card (\\S+) (\\d) (\\S+) (.*)$"), QRegularExpression::MultilineOption);
    CardMap all;
    auto    it = re.globalMatch(out);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        all[m.captured(1)][m.captured(2).toInt()].insert(m.captured(3), m.captured(4).trimmed());
    }
    return all;
}

// "reload261: key PHASE CHAT ID KEY" -> per phase and chat: id -> key.
CardMap keysOf(const QString& out)
{
    static const QRegularExpression re(QStringLiteral("^reload261: key (\\S+) (\\d) (\\S+) (\\S+)\\s*$"), QRegularExpression::MultilineOption);
    CardMap all;
    auto    it = re.globalMatch(out);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        all[m.captured(1)][m.captured(2).toInt()].insert(m.captured(3), m.captured(4).trimmed());
    }
    return all;
}

// What the rows guard (R-DEF-1, fixed in 2.7.0; before the fix every row but "text" and "cards" at the end lost the
// place, by up to 10540 px). The unload takes each chat's place once before the first edit, on TeamSpeak's own text, and
// puts it back once after the last one; the load keeps that place (LoadHold) while the chat is styled again:
// - mixed chat: TS Media albums (the unload gives the collapsed links back above the view, the load collapses them
//   again), polls (cards become text and back), HD emoji, replies and a Cozy head on most messages;
// - text chat: picture links whose "Load preview" strips go at the unload and come back at the load (a place on a strip
//   would jump);
// - cards chat: page links whose web cards come from cache/links, like the live L14 chat (a place on a card would jump;
//   the cards come back in one edit at the load);
// - album chat: the cards chat with one album early in it: the live L14 loss (96 px after the unload, 4821 px after the
//   load at QT_FONT_DPI=192; live 81 and 4849 px);
// - at every scaling (100%, QT_FONT_DPI=192, QT_SCALE_FACTOR=2), for a chat at its end and a reader scrolled up, and
//   with the load right after the unload (no event loop between them, TSMEDIA_RELOAD261_GAPMS=0).

// 2.7.2 (D8): a viewport filter installed before the plugin's (so it runs after them): called at each paint, after
// the plugin drew its pictures for it and before Qt paints.
class PaintTap : public QObject
{
  public:
    std::function<void()> onPaint;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Paint && onPaint)
            onPaint();
        return QObject::eventFilter(watched, event);
    }
};

// 2.7.4 (S2): an application filter that counts the posted calls (QEvent::MetaCall) delivered to the plugin's chat layout:
// it finds the layout by its class name while the plugin is loaded (find), and afterwards compares addresses only.
class LayoutCallSpy : public QObject
{
  public:
    bool     find      = true;
    QObject* layout    = nullptr;
    int      delivered = 0;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::MetaCall) {
            if (find && !layout && qstrcmp(watched->metaObject()->className(), "ChatLayout") == 0)
                layout = watched;
            if (watched == layout)
                ++delivered;
        }
        return QObject::eventFilter(watched, event);
    }
};

// 2.7.2 (D8): each link card's picture in a chat ("tsmedia-link:..." resources): name -> SHA-1 of its pixels ("blank" for
// the 1 x 1 stand-in a slot has before its first paint).
QMap<QString, QString> linkCards(QTextBrowser* b)
{
    QMap<QString, QString> out;
    for (QTextBlock block = b->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (!f.isImageFormat() || !f.toImageFormat().name().startsWith(QLatin1String("tsmedia-link:")))
                continue;
            const QString  name = f.toImageFormat().name();
            const QVariant v    = b->document()->resource(QTextDocument::ImageResource, QUrl(name));
            QImage         img  = v.userType() == QMetaType::QPixmap ? qvariant_cast<QPixmap>(v).toImage() : qvariant_cast<QImage>(v);
            if (img.isNull() || img.width() <= 1) {
                out.insert(name, QStringLiteral("blank"));
                continue;
            }
            img = img.convertToFormat(QImage::Format_ARGB32);
            const QByteArray bits(reinterpret_cast<const char*>(img.constBits()), img.sizeInBytes());
            out.insert(name, QString::fromLatin1(QCryptographicHash::hash(bits, QCryptographicHash::Sha1).toHex().left(12)));
        }
    }
    return out;
}

} // namespace

class TestReload261 : public QObject
{
    Q_OBJECT

  private slots:
    // The parent: one child process per row (this executable, reload261Child, its own scaling and TeamSpeak folder).
    // Each child reloads twice; after every unload (TeamSpeak's events run) and after every load (the plugin settled), a
    // chat at its end must be at its end, and a reader scrolled up must see their line where it was (4 px).
    void reloadKeepsPlace_data()
    {
        QTest::addColumn<QByteArray>("chat");
        QTest::addColumn<QByteArray>("reader");
        QTest::addColumn<QByteArray>("scaling");
        QTest::addColumn<QByteArray>("extra"); // more of the child's environment: "NAME=value;NAME=value"
        for (const char* chat : {"mixed", "text", "cards", "album"}) {
            for (const char* reader : {"end", "up"}) {
                for (const char* scaling : {"100%", "QT_FONT_DPI=192", "QT_SCALE_FACTOR=2"})
                    QTest::newRow(qPrintable(QStringLiteral("%1 chat, %2, %3").arg(QLatin1String(chat), QLatin1String(reader), QLatin1String(scaling))))
                        << QByteArray(chat) << QByteArray(reader) << QByteArray(scaling) << QByteArray();
            }
        }
        // The load right after the unload (no event loop between them: the carry is read on the chat exactly as the unload
        // left it).
        for (const char* reader : {"end", "up"})
            QTest::newRow(qPrintable(QStringLiteral("album chat, %1, QT_FONT_DPI=192, no events between").arg(QLatin1String(reader))))
                << QByteArray("album") << QByteArray(reader) << QByteArray("QT_FONT_DPI=192") << QByteArray("TSMEDIA_RELOAD261_GAPMS=0");
    }
    void reloadKeepsPlace() { keepsPlace(); }

    // 2.7 F5 (design M3): the saved chat zoom (+2 pt). The first load zooms the chat (after the place was taken, before the
    // scale); every reload finds it zoomed and sets no font, so the place keeping is the same as without a zoom. (A
    // function of its own: QtTest's 300 s watchdog counts every row of a data-driven function together.)
    void reloadKeepsPlaceZoomed_data()
    {
        QTest::addColumn<QByteArray>("chat");
        QTest::addColumn<QByteArray>("reader");
        QTest::addColumn<QByteArray>("scaling");
        QTest::addColumn<QByteArray>("extra");
        for (const char* reader : {"end", "up"})
            QTest::newRow(qPrintable(QStringLiteral("mixed chat, %1, QT_FONT_DPI=192, chatZoom=20").arg(QLatin1String(reader))))
                << QByteArray("mixed") << QByteArray(reader) << QByteArray("QT_FONT_DPI=192") << QByteArray("TSMEDIA_RELOAD261_ZOOM=20");
    }
    void reloadKeepsPlaceZoomed() { keepsPlace(); }

  private:
    // One row of reloadKeepsPlace / reloadKeepsPlaceZoomed.
    void keepsPlace()
    {
        QFETCH(QByteArray, chat);
        QFETCH(QByteArray, reader);
        QFETCH(QByteArray, scaling);
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runChild(chat, reader, scaling, extra, 2, &out));

        QStringList seen;
        for (const Measure& m : measures(out)) {
            const bool atEnd = reader == "end";
            seen << m.phase;
            const QString what = QStringLiteral("reload %1 %2: %3")
                                     .arg(m.reload)
                                     .arg(m.phase, atEnd ? QStringLiteral("end gap %1 px").arg(m.gap) : QStringLiteral("the reader's line moved %1 px").arg(m.dy, 0, 'f', 1));
            qInfo("%s", qPrintable(what));
            if (atEnd)
                QVERIFY2(m.gap >= 0 && m.gap <= 4, qPrintable(what));
            else
                QVERIFY2(qAbs(m.dy) <= 4, qPrintable(what));
        }
        QCOMPARE(seen, (QStringList{QStringLiteral("unloaded"), QStringLiteral("loaded"), QStringLiteral("unloaded"), QStringLiteral("loaded")}));
    }

  private slots:
    // Normal use around a reload (mixed chat, 100%, one reload): new messages during the load keep a chat at its end and
    // never move a reader above; the reader's own scrolling during the load is never undone (the hold let go); a chat the
    // reader scrolled while the plugin was off keeps the reader's place (the carry is refused).
    void reloadNormalUse_data()
    {
        QTest::addColumn<QByteArray>("reader");
        QTest::addColumn<QByteArray>("extra");
        QTest::newRow("end + new messages") << QByteArray("end") << QByteArray("TSMEDIA_RELOAD261_APPEND=5");
        QTest::newRow("up + new messages") << QByteArray("up") << QByteArray("TSMEDIA_RELOAD261_APPEND=5");
        QTest::newRow("end + user scrolls") << QByteArray("end") << QByteArray("TSMEDIA_RELOAD261_USERSCROLL=300");
        QTest::newRow("disable, scroll, enable") << QByteArray("up") << QByteArray("TSMEDIA_RELOAD261_MOVE=-400");
    }
    void reloadNormalUse()
    {
        QFETCH(QByteArray, reader);
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runChild("mixed", reader, "100%", extra, 1, &out));
        const QVector<Measure> all = measures(out);
        QCOMPARE(all.size(), 2);
        const Measure& unloaded = all.at(0);
        const Measure& loaded   = all.at(1);
        QCOMPARE(unloaded.phase, QStringLiteral("unloaded"));
        QCOMPARE(loaded.phase, QStringLiteral("loaded"));
        const QString what = QStringLiteral("unloaded gap %1 dy %2 block %3; loaded gap %4 dy %5 block %6 (single step %7)")
                                 .arg(unloaded.gap)
                                 .arg(unloaded.dy, 0, 'f', 1)
                                 .arg(unloaded.block)
                                 .arg(loaded.gap)
                                 .arg(loaded.dy, 0, 'f', 1)
                                 .arg(loaded.block)
                                 .arg(loaded.step);
        qInfo("%s", qPrintable(what));
        if (extra.contains("_MOVE=")) {
            // "reload261: moved R block B": the top block right after the reader scrolled the chat with the plugin off.
            static const QRegularExpression moved(QStringLiteral("^reload261: moved 1 block (\\d+)"), QRegularExpression::MultilineOption);
            const QRegularExpressionMatch   m = moved.match(out);
            QVERIFY2(m.hasMatch(), qPrintable(out.right(2000)));
            const int movedBlock = m.captured(1).toInt();
            QVERIFY2(qAbs(loaded.block - movedBlock) <= 1, qPrintable(what + QStringLiteral(", moved to block %1").arg(movedBlock)));
            QVERIFY2(qAbs(loaded.dy) >= 200, qPrintable(what));
            return;
        }
        if (reader == "end")
            QVERIFY2(unloaded.gap >= 0 && unloaded.gap <= 4, qPrintable(what));
        else
            QVERIFY2(qAbs(unloaded.dy) <= 4, qPrintable(what));
        if (extra.contains("_USERSCROLL="))
            QVERIFY2(loaded.step > 0 && loaded.gap >= 2 * loaded.step, qPrintable(what)); // the hold let go: not pulled back
        else if (reader == "end")
            QVERIFY2(loaded.gap >= 0 && loaded.gap <= 4, qPrintable(what));
        else
            QVERIFY2(qAbs(loaded.dy) <= 4, qPrintable(what));
    }

    // ---- 2.7.4 (S4): Options > Addons > Reload leaves no widget of TS Media behind --------------------------------------
    // The live 2.7.3 runs saw TeamSpeak's widget count grow by 1 with each Reload (dark mode on; 2.7.2 the same). The real
    // plugin in a fake TeamSpeak window (a chat, a menu bar, a status bar), loaded and reloaded 3 times and unloaded, each
    // unload followed by TeamSpeak's event loop: no widget made while the plugin was loaded is left, with dark mode off and
    // on (it measures TeamSpeak's bars, also on a probe window of its own, at its load). So the live one isn't made by
    // what these cover; the live test lists the widgets around a Reload (dark.widgets). Each load is at another address
    // (TSMEDIA_RELOAD261_MOVEDLL, PluginDll::reserveAtUnload), so nothing of an earlier load that a later one or Qt still
    // reaches goes unseen: a plain QWidget made at ts3plugin_init and never deleted fails both rows (tried): it is left after
    // the unload, and the load after the Reload faults.
    void reloadLeavesNoWidget_data()
    {
        QTest::addColumn<QByteArray>("extra");
        QTest::newRow("dark mode off") << QByteArray("TSMEDIA_RELOAD261_WIDGETS=1;TSMEDIA_RELOAD261_MOVEDLL=1");
        QTest::newRow("dark mode on") << QByteArray("TSMEDIA_RELOAD261_WIDGETS=1;TSMEDIA_RELOAD261_DARK=1;TSMEDIA_RELOAD261_MOVEDLL=1");
    }
    void reloadLeavesNoWidget()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runChild("mixed", "end", "100%", extra, 3, &out));
        static const QRegularExpression re(QStringLiteral("^reload261: widgets (\\d+) unloaded: (\\d+) \\((\\d+) before the first load\\) left (\\d+): ([^\\r\\n]*)"),
                                           QRegularExpression::MultilineOption);
        QStringList unloads;
        for (auto it = re.globalMatch(out); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            qInfo("%s", qPrintable(m.captured(0).mid(11)));
            unloads.append(m.captured(1));
            QVERIFY2(m.captured(4).toInt() == 0 && m.captured(2) == m.captured(3), qPrintable(m.captured(0)));
        }
        QCOMPARE(unloads.join(QLatin1Char(' ')), QStringLiteral("1 2 3 4"));
        static const QRegularExpression moved(QStringLiteral("^reload261: moved: [^\\r\\n]*"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   where = moved.match(out);
        QVERIFY2(where.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(where.captured(0).mid(11)));
        QCOMPARE(where.captured(0).mid(18), QStringLiteral("4 loads at 4 addresses, 4 unloads reserved the range they left"));
        static const QRegularExpression sheet(QStringLiteral("^reload261: widgets loaded: application sheet (\\d+) chars"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   applied = sheet.match(out);
        QVERIFY2(applied.hasMatch(), qPrintable(out.right(2000)));
        QVERIFY2(extra.contains("DARK=1") ? applied.captured(1).toInt() > 0 : applied.captured(1).toInt() == 0, qPrintable(applied.captured(0)));
    }

    // The child: one chat, the plugin loaded, settled, the reader placed (end, or scrolled up), then Reload as TeamSpeak
    // does it, `reloads` times. Runs only when the parent starts it (TSMEDIA_RELOAD261_CHILD=end|up).
    void reload261Child()
    {
        const QByteArray scenario = qgetenv("TSMEDIA_RELOAD261_CHILD");
        if (scenario.isEmpty())
            QSKIP("runs as a child of reloadKeepsPlace");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        const int        reloads  = qMax(1, qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_RELOADS"));
        const QByteArray kind     = qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_KIND") ? QByteArray("mixed") : qgetenv("TSMEDIA_RELOAD261_KIND");
        const int        messages = qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_MESSAGES") > 0 ? qMax(40, qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_MESSAGES")) : 160;
        // Between the unload and the load: TeamSpeak's event loop (default 300 ms; 0 loads again at once).
        const int gapMs = qEnvironmentVariableIsSet("TSMEDIA_RELOAD261_GAPMS") ? qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_GAPMS") : 300;
        // A new chat width while the load runs (Qt lays the chat out again): this many px narrower.
        const int narrower = qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_NARROWER");
        // Normal use around the reload (reloadNormalUse): this many messages appended during the load (one per 150 ms from
        // +100 ms); the reader scrolls up three steps this many ms into the load; the reader scrolls the chat by this many
        // px while the plugin is off (after the unload's event loop).
        const int appendCount  = qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_APPEND");
        const int userScrollMs = qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_USERSCROLL");
        const int move         = qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_MOVE");
        // 2.7 F5: the saved chat zoom in settings.ini (tenths of a point).
        const int zoom         = qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_ZOOM");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        {
            QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
            QFile ini(dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=false\nmakeLinkPreviews=false\nsharePresence=false\n");
            if (zoom != 0)
                ini.write(QByteArray("chatZoom=") + QByteArray::number(zoom) + QByteArray("\n"));
            if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_DARK")) // 2.7.4 (S4): dark mode on at every load
                ini.write("darkMode=true\n");
        }
        if (kind == "cards" || kind == "album")
            QVERIFY(seedCards(dir.path() + QStringLiteral("/plugins/tsmedia/cache/links"), messages));

        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        auto* browser = new QTextBrowser;
        browser->setOpenLinks(false);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        browser->document()->setDefaultFont(font);
        tabs->addTab(browser, QStringLiteral("Fake channel"));
        column->addWidget(tabs);
        window.setCentralWidget(central);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_WIDGETS")) { // 2.7.4 (S4): TeamSpeak's bars, which dark mode measures
            window.menuBar()->addMenu(QStringLiteral("&Plugins"))->addAction(QStringLiteral("Fake plugin"));
            window.statusBar()->addPermanentWidget(new QLabel(QStringLiteral("Connected")));
        }
        window.resize(1000, 760);
        fillChat(browser->document(), messages, kind);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QScrollBar* bar = browser->verticalScrollBar();
        bar->setValue(bar->maximum());
        const QScreen* screen = QGuiApplication::primaryScreen();
        report(QStringLiteral("%1 chat, screen dpr %2 logicalDpi %3, viewport %4x%5, blocks %6")
                   .arg(QString::fromLatin1(kind))
                   .arg(screen->devicePixelRatio())
                   .arg(screen->logicalDotsPerInchY())
                   .arg(browser->viewport()->width())
                   .arg(browser->viewport()->height())
                   .arg(browser->document()->blockCount()));

        const qreal basePoints = browser->font().pointSizeF(); // 2.7 F5: TeamSpeak's chat font
        // 2.7.4 (S4): the widgets TeamSpeak had before the first load; what each load added (named while the DLL is loaded),
        // and what is left of them after each unload and TeamSpeak's event loop.
        const bool countWidgets = !qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_WIDGETS");
        if (countWidgets)
            QTest::qWait(gapMs); // what the window's show left to do
        const QWidgetList    widgetsAtStart = QApplication::allWidgets();
        const QSet<QWidget*> widgetsBefore(widgetsAtStart.begin(), widgetsAtStart.end());
        QHash<QWidget*, QString> made;
        auto                     noteMade = [&] {
            for (QWidget* w : QApplication::allWidgets()) {
                if (!widgetsBefore.contains(w)) // again each time: an address can come back with another widget
                    made.insert(w, QStringLiteral("%1#%2 in %3%4").arg(QLatin1String(w->metaObject()->className()), w->objectName(),
                                                                       w->parentWidget() ? QLatin1String(w->parentWidget()->metaObject()->className()) : QLatin1String("(no parent)"),
                                                                       w->isWindow() ? QStringLiteral(" window") : QString()));
            }
        };
        auto widgetsLeft = [&](int r) {
            QStringList left;
            for (QWidget* w : QApplication::allWidgets()) {
                if (!widgetsBefore.contains(w))
                    left.append(made.value(w, QStringLiteral("made at the unload #%1").arg(w->objectName())));
            }
            left.sort();
            report(QStringLiteral("widgets %1 unloaded: %2 (%3 before the first load) left %4: %5")
                       .arg(r)
                       .arg(QApplication::allWidgets().size())
                       .arg(widgetsBefore.size())
                       .arg(left.size())
                       .arg(left.join(QStringLiteral("; "))));
        };
        // 2.7.4 (S4): with TSMEDIA_RELOAD261_MOVEDLL, the addresses of the loads and the unloads that reserved the range they left.
        QSet<quintptr> bases;
        int            loads = 0, reservedRanges = 0;
        PluginDll      plugin;
        QCOMPARE(plugin.load(dll), QString());
        bases.insert(plugin.base);
        ++loads;
        m_plugin = &plugin;
        settle(browser, 30000);
        if (countWidgets) {
            noteMade();
            report(QStringLiteral("widgets loaded: application sheet %1 chars").arg(qApp->styleSheet().size())); // dark mode's, when on
        }
        report(QStringLiteral("loaded: ") + describe(whereOf(browser)) + QStringLiteral(" true max %1").arg(trueMaximum(browser)));
        const qreal zoomedPoints = basePoints + zoom / 10.0;
        if (zoom != 0) // the first load zoomed the chat
            QCOMPARE(browser->font().pointSizeF(), zoomedPoints);

        // The reader's place: the end, or the line of message 100 (or UPAT) 5 px below the view's top.
        const bool    atEnd  = scenario == "end";
        const QString marker = QStringLiteral("r261 %1 ").arg(qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_UPAT") > 0 ? qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_UPAT") : 100);
        if (atEnd) {
            bar->setValue(bar->maximum());
        } else {
            QVERIFY(lineY(browser, marker) < 1e8);
            bar->setValue(bar->value() + qRound(lineY(browser, marker)) - 5);
        }
        pump(browser, 1500);
        if (atEnd)
            bar->setValue(bar->maximum());
        pump(browser, 500);
        const Where placed  = whereOf(browser);
        const qreal placedY = lineY(browser, marker);
        report(QStringLiteral("placed: ") + describe(placed) + QStringLiteral(" true max %1, line y %2").arg(trueMaximum(browser)).arg(placedY, 0, 'f', 1));
        auto measure = [&](int r, const char* phase) {
            const Where w = whereOf(browser);
            report(QStringLiteral("measure %1 %2 gap %3 dy %4 block %5 step %6")
                       .arg(r)
                       .arg(QLatin1String(phase))
                       .arg(w.maximum - w.value)
                       .arg(lineY(browser, marker) - placedY, 0, 'f', 1)
                       .arg(w.block)
                       .arg(bar->singleStep()));
        };

        for (int r = 1; r <= reloads; ++r) {
            const Where before = whereOf(browser);
            report(QStringLiteral("reload %1 before: ").arg(r) + describe(before));
            m_logFrom = fakets3::logLines().size();
            plugin.unload(); // ts3plugin_shutdown, then the DLL freed
            m_plugin = nullptr;
            reservedRanges += plugin.reserved ? 1 : 0;
            printPluginSteps();
            report(QStringLiteral("reload %1 unloaded: ").arg(r) + describe(whereOf(browser)));
            if (gapMs > 0) {
                QTest::qWait(gapMs); // TeamSpeak's event loop runs (the Options dialog) before the DLL is loaded again
                report(QStringLiteral("reload %1 unloaded+events: ").arg(r) + describe(whereOf(browser)) + QStringLiteral(" true max %1").arg(trueMaximum(browser)));
            }
            if (countWidgets)
                widgetsLeft(r);
            if (move != 0) { // the reader scrolls while TS Media is off
                bar->setValue(bar->value() + move);
                report(QStringLiteral("moved %1 block %2: ").arg(r).arg(whereOf(browser).block) + describe(whereOf(browser)));
            }
            measure(r, "unloaded");
            m_logFrom = fakets3::logLines().size();
            QCOMPARE(plugin.load(dll), QString());
            bases.insert(plugin.base);
            ++loads;
            m_plugin = &plugin;
            // Normal use while the load runs: timed from the load (pump runs them).
            m_loadClock.start();
            m_due.clear();
            for (int i = 0; i < appendCount; ++i) {
                m_due.append({100 + 150 * i, [this, browser, i] {
                                  const tschat::Skin s = tschat::darkSkin();
                                  tschat::append(browser->document(), tschat::messageHtml(s, QStringLiteral("23:59:%1").arg(i, 2, 10, QLatin1Char('0')), 17, kAlice,
                                                                                          QStringLiteral("Alice"), QStringLiteral("r261 new %1 just now").arg(i)));
                                  report(QStringLiteral("appended %1: ").arg(i) + describe(whereOf(browser)));
                              }});
            }
            if (userScrollMs > 0) {
                m_due.append({userScrollMs, [this, browser, bar] {
                                  for (int i = 0; i < 3; ++i)
                                      bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
                                  report(QStringLiteral("user scrolled: ") + describe(whereOf(browser)));
                              }});
            }
            if (narrower > 0)
                window.resize(window.width() - narrower, window.height());
            // Samples while the load settles (the first scan, the layout's history pass, the previews from the cache).
            QElapsedTimer clock;
            clock.start();
            Where last;
            int   stable = 0;
            while (clock.elapsed() < 30000) {
                pump(browser, 250);
                printPluginSteps();
                const Where now = whereOf(browser);
                if (now.value != last.value || now.maximum != last.maximum || now.block != last.block) {
                    report(QStringLiteral("reload %1 load +%2 ms: ").arg(r).arg(clock.elapsed()) + describe(now));
                    stable = 0;
                } else if (++stable >= 8 && clock.elapsed() > 2500 && pendingDownloads() == 0) {
                    break;
                }
                last = now;
            }
            const Where after = whereOf(browser);
            report(QStringLiteral("reload %1 loaded: ").arg(r) + describe(after) + QStringLiteral(" true max %1").arg(trueMaximum(browser)));
            if (countWidgets)
                noteMade();
            measure(r, "loaded");
            if (zoom != 0) // 2.7 F5: kept across the unload, found as it is at the load (no font set)
                QCOMPARE(browser->font().pointSizeF(), zoomedPoints);
            if (narrower > 0) {
                window.resize(window.width() + narrower, window.height());
                pump(browser, 1000);
            }
        }
        plugin.unload();
        m_plugin = nullptr;
        reservedRanges += plugin.reserved ? 1 : 0;
        QTest::qWait(countWidgets ? qMax(100, gapMs) : 100);
        if (countWidgets)
            widgetsLeft(reloads + 1);
        if (plugin.reserveAtUnload)
            report(QStringLiteral("moved: %1 loads at %2 addresses, %3 unloads reserved the range they left").arg(loads).arg(bases.size()).arg(reservedRanges));
    }

    // ---- 2.7.1 (dark mode, issue 2): the cards TS Media drew while dark mode was on ------------------------------------
    // T-A: Options -> Addons -> Disable with dark mode on leaves exactly the pictures a Disable with dark mode off leaves,
    // in the visible chat and in a hidden one, in Cozy, Compact and TeamSpeak classic, on a light and a dark skin; the reader
    // scrolled up keeps their line (2 px), a hidden chat at its end stays there. 2.7.0 left every card drawn dark.
    void darkDisableLeavesTeamSpeaksLook_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const char* layout : {"1", "2", "0"}) {
            for (const char* skin : {"light", "dark"}) {
                const QByteArray name = QByteArray(layout[0] == '1' ? "Cozy" : layout[0] == '2' ? "Compact" : "TeamSpeak classic") + ", " + skin + " skin";
                QTest::newRow(name.constData()) << (QByteArray("TSMEDIA_DARKCARDS_LAYOUT=") + layout + ";TSMEDIA_DARKCARDS_SKIN=" + skin);
            }
        }
    }
    void darkDisableLeavesTeamSpeaksLook() { darkDisable(); }

    // The same at 150 % and 200 % (Cozy, light skin; a function of its own: QtTest's watchdog counts a function's rows).
    void darkDisableLeavesTeamSpeaksLookScaled_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const char* dpi : {"144", "192"})
            QTest::newRow(qPrintable(QStringLiteral("Cozy, light skin, QT_FONT_DPI=%1").arg(QLatin1String(dpi))))
                << (QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;QT_FONT_DPI=") + dpi);
    }
    void darkDisableLeavesTeamSpeaksLookScaled() { darkDisable(); }

    // T-B: Reload with dark mode on: while TS Media is gone (TeamSpeak's event loop, the Options dialog) no card is dark in
    // the light chat, and after the load they are dark again exactly as before.
    void darkReloadGapStaysLight()
    {
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("reload", "TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light", &out));
        const CardMap cards = cardsOf(out);
        for (int chat : {1, 2}) {
            const auto& u0 = cards["U0"][chat];
            const auto& g  = cards["G"][chat];
            QVERIFY2(u0.size() >= 6, qPrintable(out.right(3000)));
            QVERIFY2(cardsDiffer(cards["R"][chat], cards["D"][chat]), qPrintable(QStringLiteral("chat %1: dark mode drew nothing differently").arg(chat)));
            QString diff = sameCards(g, u0);
            QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat %1, in the gap: ").arg(chat) + diff));
            diff = sameCards(cards["D2"][chat], cards["D"][chat]);
            QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat %1, after the load: ").arg(chat) + diff));
        }
    }

    // T-C: dark mode on and off while TS Media runs (the Plugins menu, no event loop turn after it): every card of the
    // visible chat in the new look at once, a hidden chat's before its first paint (its Show), in Cozy, Compact and
    // TeamSpeak classic, on a light and a dark skin. 2.7.0 drew them again only from timers.
    void darkToggleDrawsCardsAtOnce_data() { darkDisableLeavesTeamSpeaksLook_data(); }
    void darkToggleDrawsCardsAtOnce()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("toggle", extra, &out));
        const CardMap cards = cardsOf(out);
        for (const QString& line : out.split(QLatin1Char('\n'))) {
            if (line.contains(QLatin1String("[dark] on in")) || line.contains(QLatin1String("[dark] off in")))
                qInfo("%s", qPrintable(line.trimmed()));
        }
        for (int chat : {1, 2}) {
            QVERIFY2(cards["R"][chat].size() >= 6, qPrintable(out.right(3000)));
            QVERIFY2(cardsDiffer(cards["R"][chat], cards["Dsettled"][chat]), qPrintable(QStringLiteral("chat %1: dark mode drew nothing differently").arg(chat)));
            QString diff = sameCards(cards["Rsettled"][chat], cards["R"][chat]);
            QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat %1, off again: ").arg(chat) + diff));
        }
        // On from the first tab: its cards at once; the second tab's at its Show.
        QString diff = sameCards(cards["T1"][1], cards["Dsettled"][1]);
        QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat 1 right after on: ") + diff));
        diff = sameCards(cards["T2"][2], cards["Dsettled"][2]);
        QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat 2 at its Show: ") + diff));
        // Off from the second tab: its cards at once; the first tab's at its Show.
        diff = sameCards(cards["Toff1"][2], cards["Rsettled"][2]);
        QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat 2 right after off: ") + diff));
        diff = sameCards(cards["Toff2"][1], cards["Rsettled"][1]);
        QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat 1 at its Show: ") + diff));
    }

    // ---- 2.7.1 review fixes ------------------------------------------------------------------------------------------------
    // T-F: TeamSpeak's Quit after dark mode went on (and off) from the first tab while the second was hidden, that tab never
    // shown again: nothing of ours is drawn at Quit, in either chat (not one picture put again). Before the fix the hidden
    // chat's pictures were drawn at Quit for a dark sheet without its accent (its pending look, left by the toggle).
    void darkQuitDrawsNothing_data()
    {
        QTest::addColumn<QByteArray>("extra");
        QTest::newRow("Cozy, on, Quit at once") << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_SETTLE=0");
        QTest::newRow("Cozy, on, Quit later") << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light");
        QTest::newRow("Cozy, on and off, Quit at once")
            << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_SETTLE=0;TSMEDIA_DARKCARDS_OFF=1");
        QTest::newRow("TeamSpeak classic, on, Quit later") << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=0;TSMEDIA_DARKCARDS_SKIN=light");
        QTest::newRow("TeamSpeak classic, on and off, Quit later") << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=0;TSMEDIA_DARKCARDS_SKIN=dark;TSMEDIA_DARKCARDS_OFF=1");
    }
    void darkQuitDrawsNothing()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("quit", extra, &out));
        for (const QString& line : out.split(QLatin1Char('\n'))) {
            if (line.contains(QLatin1String("quit unload")))
                qInfo("%s", qPrintable(line.trimmed()));
        }
        const CardMap cards = cardsOf(out), keys = keysOf(out);
        for (int chat : {1, 2}) {
            QVERIFY2(cards["Q0"][chat].size() >= 6, qPrintable(out.right(3000)));
            QCOMPARE(cards["Q1"][chat], cards["Q0"][chat]);
            QCOMPARE(keys["Q0"][chat].size(), cards["Q0"][chat].size());
            QVERIFY2(keys["Q1"][chat] == keys["Q0"][chat], qPrintable(QStringLiteral("chat %1: a picture was put again at Quit").arg(chat)));
        }
    }

    // T-G: a chat hidden at a toggle that the relayout timer drew for the new look meanwhile is not drawn a second time at
    // its Show (review fix: every picture was drawn again then, before the first paint), and its first paint shows the
    // new look. Cozy and Compact (their layout asks the timer for a hidden chat too).
    void darkShowDrawsOnce_data()
    {
        QTest::addColumn<QByteArray>("extra");
        QTest::newRow("Cozy, light skin") << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light");
        QTest::newRow("Compact, dark skin") << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=2;TSMEDIA_DARKCARDS_SKIN=dark");
        QTest::newRow("Cozy, on and off, light skin") << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_OFF=1");
    }
    void darkShowDrawsOnce()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("show", extra, &out));
        const CardMap cards = cardsOf(out), keys = keysOf(out);
        QVERIFY2(cards["S0"][2].size() >= 6, qPrintable(out.right(3000)));
        if (!extra.contains("OFF=1"))
            QVERIFY2(cardsDiffer(cards["R"][2], cards["S0"][2]), "the hidden chat wasn't drawn for dark mode before its Show");
        QString diff = sameCards(cards["S1"][2], cards["S2"][2]);
        QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat 2 at its first paint: ") + diff));
        diff = sameCards(cards["S0"][2], cards["S2"][2]);
        QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat 2 before its Show: ") + diff));
        QCOMPARE(keys["S0"][2].size(), cards["S0"][2].size());
        QVERIFY2(keys["S1"][2] == keys["S0"][2], "chat 2: its pictures were drawn again at its Show");
    }

    // T-H: Disable and dark mode toggles with 2400 x 1800 photos at 300 %, more than Core's still cache keeps (their stills
    // are decoded on a worker): a redraw for the look never puts a stand-in ("Can't preview this image", a blurhash, a
    // smaller copy) where a photo is drawn. Every picture is the one of the look before or of the new look; at the unload
    // a photo whose still isn't at hand keeps the one it has (review fix: a photo of the visible chat became a stand-in in
    // TeamSpeak's chat at the Disable here).
    void darkLooksNeverStandIns_data()
    {
        QTest::addColumn<QByteArray>("scenario");
        QTest::addColumn<QByteArray>("extra");
        const QByteArray big("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_BIG=1;TSMEDIA_DARKCARDS_SETS=8;QT_SCALE_FACTOR=3");
        QTest::newRow("Disable, Cozy, 300 %") << QByteArray("disable") << big;
        QTest::newRow("toggles, Cozy, 300 %") << QByteArray("toggle") << big;
    }
    void darkLooksNeverStandIns()
    {
        QFETCH(QByteArray, scenario);
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString       out;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(runCardsChild(scenario, extra, &out));
        qInfo("child %lld ms", clock.elapsed());
        const CardMap cards = cardsOf(out);
        // Each picture of `got` is the one of `before` or of `after` (blanks aside); the count of those still as before.
        const auto oneOf = [](const QMap<QString, QString>& got, const QMap<QString, QString>& before, const QMap<QString, QString>& after, int* asBefore,
                              int* compared) {
            for (auto it = got.constBegin(); it != got.constEnd(); ++it) {
                const QString b = before.value(it.key()), a = after.value(it.key());
                if (it.value() == QLatin1String("blank") || b.isEmpty() || a.isEmpty() || b == QLatin1String("blank") || a == QLatin1String("blank"))
                    continue;
                ++*compared;
                if (it.value() == a)
                    continue;
                if (it.value() == b) {
                    ++*asBefore;
                    continue;
                }
                return QStringLiteral("%1: %2, neither %3 nor %4").arg(it.key(), it.value(), b, a);
            }
            return QString();
        };
        int kept = 0;
        for (const auto& step : scenario == "disable" ? QVector<QVector<const char*>>{{"U1", "D", "U0"}}
                                                      : QVector<QVector<const char*>>{{"T1", "R", "Dsettled"}, {"T2", "R", "Dsettled"},
                                                                                       {"Toff1", "Dsettled", "Rsettled"}, {"Toff2", "Dsettled", "Rsettled"}}) {
            for (int chat : {1, 2}) {
                int           asBefore = 0, compared = 0;
                const QString diff     = oneOf(cards[step[0]][chat], cards[step[1]][chat], cards[step[2]][chat], &asBefore, &compared);
                QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("%1, chat %2: ").arg(QLatin1String(step[0])).arg(chat) + diff));
                qInfo("%s, chat %d: %d pictures compared, %d as before", step[0], chat, compared, asBefore);
                kept += asBefore;
            }
        }
        // How many kept their picture (a still not at hand): the count depends on what Core's cache still has, so for the
        // toggles it is reported, not checked. (The 2.7.1 code before the review fix put a stand-in for a photo of the visible
        // chat at the Disable of this row.) 2.7.2 (D1): at the unload none is kept: every picture drawn while dark mode was on
        // is drawn again from the still it was drawn from (2.7.1 left one here in its dark drawing).
        qInfo("pictures kept as they were: %d", kept);
        if (scenario == "disable")
            QCOMPARE(kept, 0);
    }

    // ---- 2.7.2 (D1): Options -> Addons -> Disable with dark mode on in long chats of big pictures (more stills than Core's
    // cache keeps, files it never decodes at the unload: over 4 MP, 12 MP PNGs, a transparent sticker), at 200 % and 300 %,
    // on a light and a dark skin: every picture TeamSpeak's chats keep is the one a Disable with dark mode off leaves, none
    // has a dark-drawn pixel or dark mode's accent, the reader keeps their line; the unload's time is reported (U0 off, U1
    // on). 2.7.1 kept a picture whose still wasn't at hand in its dark drawing ----------------------------------------------
    void darkDisableKeepsNoDarkPicture_data()
    {
        QTest::addColumn<QByteArray>("extra");
        QTest::addColumn<int>("minimum");
        QTest::newRow("Cozy, light skin, 300 %, 12 sets")
            << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_BIG=1;TSMEDIA_DARKCARDS_SETS=12;QT_SCALE_FACTOR=3") << 25;
        QTest::newRow("Compact, light skin, QT_FONT_DPI=192, 12 MP and transparent pictures")
            << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=2;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_HUGE=1;TSMEDIA_DARKCARDS_SETS=10;QT_FONT_DPI=192") << 25;
        QTest::newRow("Cozy, dark skin, 200 %, 10 sets")
            << QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=dark;TSMEDIA_DARKCARDS_BIG=1;TSMEDIA_DARKCARDS_SETS=10;QT_SCALE_FACTOR=2") << 25;
    }
    void darkDisableKeepsNoDarkPicture()
    {
        QFETCH(QByteArray, extra);
        QFETCH(int, minimum);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("disable", extra, &out));
        const CardMap cards = cardsOf(out);
        for (int chat : {1, 2}) {
            const auto& u0 = cards["U0"][chat];
            const auto& u1 = cards["U1"][chat];
            QVERIFY2(cardsDiffer(cards["R"][chat], cards["D"][chat]), qPrintable(QStringLiteral("chat %1: dark mode drew nothing differently").arg(chat)));
            if (chat == 1) { // the unload's own times first (reported whatever comes next)
                static const QRegularExpression timeRe(QStringLiteral("^reload261: unload (U[01]) (\\d+) ms"), QRegularExpression::MultilineOption);
                auto                            times = timeRe.globalMatch(out);
                while (times.hasNext()) {
                    const QRegularExpressionMatch m = times.next();
                    qInfo("unload %s (dark mode %s): %s ms", qPrintable(m.captured(1)), m.captured(1) == QLatin1String("U0") ? "off" : "on", qPrintable(m.captured(2)));
                }
            }
            int compared = 0;
            for (auto it = u1.constBegin(); it != u1.constEnd(); ++it) {
                const QString want = u0.value(it.key());
                if (it.value() == QLatin1String("blank"))
                    continue;
                // No picture TeamSpeak's chat keeps has dark mode's accent, nor (on a light skin, whose cards have none) a pixel of
                // the dark card; on a dark skin the cards are dark in TeamSpeak's own look too (compared with U0 below).
                const QString wantFlags = extra.contains("SKIN=dark") ? QStringLiteral("accent=0") : QStringLiteral("dark=0 accent=0");
                QVERIFY2(it.value().endsWith(wantFlags), qPrintable(QStringLiteral("chat %1: %2 %3").arg(chat).arg(it.key(), it.value())));
                if (want.isEmpty() || want == QLatin1String("blank"))
                    continue;
                ++compared;
                QVERIFY2(it.value() == want, qPrintable(QStringLiteral("chat %1: %2 %3, want %4").arg(chat).arg(it.key(), it.value(), want)));
            }
            qInfo("chat %d: %d pictures compared", chat, compared);
            if (chat == 1)
                QVERIFY2(compared >= minimum, qPrintable(QStringLiteral("only %1 pictures to compare").arg(compared)));
        }
        static const QRegularExpression placeRe(QStringLiteral("^reload261: place U1 dy (-?[0-9.]+) hidden gap (-?\\d+) -> (-?\\d+)"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   m = placeRe.match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(m.captured(0)));
        QVERIFY2(qAbs(m.captured(1).toDouble()) <= 2, qPrintable(m.captured(0)));
        if (m.captured(2).toInt() <= 2)
            QVERIFY2(m.captured(3).toInt() >= 0 && m.captured(3).toInt() <= 2, qPrintable(m.captured(0)));
        QVERIFY2(out.contains(QLatin1String("reload261: sheet back 1")), qPrintable(out.right(2000)));
    }

    // ---- 2.7.2 (D1, a guard): dark mode off with the chat at its end after every card was seen in the dark look (Core's
    // cache has lost most of their stills at 200 %), then up a screen at a time: every picture that came near is drawn for
    // TeamSpeak's look (the one it had before dark mode), none keeps its dark drawing --------------------------------------
    void darkOffDrawsNearFromKeptStill()
    {
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("offnear", "TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_BIG=1;TSMEDIA_DARKCARDS_SETS=8;QT_SCALE_FACTOR=2", &out));
        const CardMap cards = cardsOf(out);
        const auto&   up    = cards["Up"][1];
        const auto&   light = cards["R"][1];
        QVERIFY2(cardsDiffer(light, cards["Dsettled"][1]), "dark mode drew nothing differently");
        int compared = 0;
        for (auto it = up.constBegin(); it != up.constEnd(); ++it) {
            const QString want = light.value(it.key());
            if (it.value() == QLatin1String("blank") || want.isEmpty() || want == QLatin1String("blank"))
                continue;
            ++compared;
            QVERIFY2(it.value() == want, qPrintable(QStringLiteral("%1: %2, want %3").arg(it.key(), it.value(), want)));
        }
        qInfo("%d pictures compared", compared);
        QVERIFY(compared >= 20);
    }

    // ---- 2.7.3 (M3): Options -> Addons -> Disable right after a load with dark mode on, while the 12 MP pictures' stills are
    // still being read on a worker (the load drew them from a stand-in: their blurhash, or nothing): no picture TeamSpeak's
    // chats keep has a dark-drawn pixel or dark mode's accent, and each one is the one the same Disable with dark mode off
    // leaves (U0), or (a card the load drew a moment later or sooner) its settled look (R); a picture drawn from a stand-in
    // there (U0 not R) is that same stand-in drawing. 2.7.2 kept the stand-in's dark drawing (no still was kept for it). The
    // unload's time is reported (U0 off, U1 on) ----------------------------------------------------------------------------
    // 2.7.4 (S5): one function per case (they were the three rows of darkStandInsAtUnload), each with QtTest's 300 s of its
    // own (see watchdogTurn): the rows shared the first row's 300 s, and under load the third one ran out of it and aborted
    // the suite.
    void darkStandInsAtUnload_data() { watchdogTurn(); }
    void darkStandInsAtUnload() // Cozy, light skin, 12 MP pictures with a blurhash
    {
        standInsAtUnload("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_HUGE=1;TSMEDIA_DARKCARDS_BLURHASH=1;TSMEDIA_DARKCARDS_SETS=3");
    }
    void darkStandInsAtUnloadNoBlurHash_data() { watchdogTurn(); }
    void darkStandInsAtUnloadNoBlurHash() // Cozy, light skin, 12 MP pictures without a blurhash
    {
        standInsAtUnload("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_HUGE=1;TSMEDIA_DARKCARDS_SETS=3");
    }
    void darkStandInsAtUnloadCompact192_data() { watchdogTurn(); }
    void darkStandInsAtUnloadCompact192() // Compact, light skin, QT_FONT_DPI=192, 12 MP pictures with a blurhash
    {
        standInsAtUnload("TSMEDIA_DARKCARDS_LAYOUT=2;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_HUGE=1;TSMEDIA_DARKCARDS_BLURHASH=1;TSMEDIA_DARKCARDS_SETS=3;"
                         "QT_FONT_DPI=192");
    }

  private:
    // 2.7.4 (S5): darkStandInsBooked's load (hash: with blurhashes), its blank cards kept in m_bookedBlanks.
    void bookedLoad(bool hash)
    {
        QString out;
        QVERIFY(runCardsChild("standin",
                              QByteArray("TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_HUGE=1;TSMEDIA_DARKCARDS_SETS=10;"
                                         "QT_SCALE_FACTOR=3")
                                  + (hash ? QByteArray(";TSMEDIA_DARKCARDS_BLURHASH=1") : QByteArray()),
                              &out));
        const CardMap cards = cardsOf(out);
        int           found[2] = {0, 0};
        for (int phase = 0; phase < 2; ++phase) {
            const auto& chat = cards[phase == 0 ? QStringLiteral("U0") : QStringLiteral("U1")][1];
            QVERIFY2(chat.size() >= 60, qPrintable(out.right(3000)));
            for (const QString& card : chat)
                found[phase] += card == QLatin1String("blank") ? 1 : 0;
        }
        m_bookedBlanks[hash ? 1 : 0][0] = found[0];
        m_bookedBlanks[hash ? 1 : 0][1] = found[1];
        m_bookedLoaded[hash ? 1 : 0]    = true;
        qInfo("%s: blank cards in chat 1 after the Disable: U0 %d, U1 %d of %d", hash ? "with blurhashes" : "without", found[0], found[1],
              cards[QStringLiteral("U1")][1].size());
    }
    // 2.7.4 (S5): QtTest 5.15's watchdog (300 s a row) starts a new deadline only when its thread runs between the end of one
    // row and the start of the next; a row that starts before that thread woke up keeps the deadline it had. Between two
    // rows of a function QtTest does almost nothing, so darkStandInsAtUnload's three rows shared the first row's 300 s
    // (measured with QTEST_FUNCTION_TIMEOUT=35000: its third row was aborted 1 s after it began, 35.0 s after the first row
    // began, each row taking about 17 s alone). Between two functions QtTest does little more. A function's _data() runs in
    // that gap: 100 ms there let the watchdog's thread wake up, so each case gets 300 s of its own (its child has 200 s,
    // finishChild).
    static void watchdogTurn() { QThread::msleep(100); }
    void standInsAtUnload(const QByteArray& extra)
    {
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("standin", extra, &out));
        static const QRegularExpression timeRe(QStringLiteral("^reload261: (unload U[01] .*)$"), QRegularExpression::MultilineOption);
        for (auto times = timeRe.globalMatch(out); times.hasNext();)
            qInfo("%s", qPrintable(times.next().captured(1).trimmed()));
        const CardMap cards    = cardsOf(out);
        int           standIns = 0;
        for (int chat : {1, 2}) {
            const auto& r  = cards["R"][chat];
            const auto& u0 = cards["U0"][chat];
            const auto& u1 = cards["U1"][chat];
            QVERIFY2(u0.size() >= 6, qPrintable(out.right(3000)));
            int compared = 0;
            for (auto it = u1.constBegin(); it != u1.constEnd(); ++it) {
                if (it.value() == QLatin1String("blank"))
                    continue;
                QVERIFY2(it.value().endsWith(QLatin1String("dark=0 accent=0")), qPrintable(QStringLiteral("chat %1: %2 %3").arg(chat).arg(it.key(), it.value())));
                const QString want = u0.value(it.key()), settled = r.value(it.key());
                if (want.isEmpty() || want == QLatin1String("blank") || settled.isEmpty() || settled == QLatin1String("blank"))
                    continue;
                ++compared;
                QVERIFY2(it.value() == want || it.value() == settled,
                         qPrintable(QStringLiteral("chat %1: %2 %3, want %4 (settled: %5)").arg(chat).arg(it.key(), it.value(), want, settled)));
                // A stand-in's drawing where the Disable with dark mode off leaves one (the case this guards).
                standIns += it.value() == want && want != settled ? 1 : 0;
            }
            qInfo("chat %d: %d pictures compared", chat, compared);
            QVERIFY2(compared >= 4, qPrintable(QStringLiteral("chat %1: only %2 pictures to compare").arg(chat).arg(compared)));
        }
        qInfo("pictures drawn back from a stand-in at the Disable: %d", standIns);
        QVERIFY2(standIns > 0, "no picture was drawn from a stand-in: the case wasn't made");
    }

  private slots:
    // ---- 2.7.3 review (M3): a picture drawn in dark mode from its blurhash keeps no image of it (Core makes the blurhash again at
    // once, Core::blurHashStill), so its stand-in books nothing in the chat's picture budget: the dark load swaps no more far
    // cards for blanks with blurhashes in the links than without them (drawn from nothing then). 80 cards with 12 MP pictures
    // at 300 %, the standin child (a Disable as soon as the load drew every picture; U0 dark mode off, U1 on). f69282b kept
    // and booked the blurhash's image: 63 blank cards of 80 with blurhashes against 56 without (2.7.2 and this: 56 both) -----
    // 2.7.4 (S5): the two loads are functions of their own, each with QtTest's 300 s (watchdogTurn): one function took 79 s
    // alone for both, and could share its deadline with the function before it. The second compares (it makes the first's
    // load itself when it runs alone).
    void darkStandInsBooked_data() { watchdogTurn(); }
    void darkStandInsBooked() // without blurhashes
    {
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        bookedLoad(false);
    }
    void darkStandInsBookedBlurHash_data() { watchdogTurn(); }
    void darkStandInsBookedBlurHash() // with blurhashes, and the comparison
    {
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        if (!m_bookedLoaded[0]) {
            bookedLoad(false);
            if (QTest::currentTestFailed())
                return;
        }
        bookedLoad(true);
        if (QTest::currentTestFailed())
            return;
        const int(&blanks)[2][2] = m_bookedBlanks; // [with blurhashes][U0, U1]: chat 1's blank cards
        // The same loads with dark mode off swap the same cards (the stand-ins' cards have one size): the two are comparable.
        QVERIFY2(qAbs(blanks[1][0] - blanks[0][0]) <= 2, "with dark mode off the loads with and without blurhashes swapped other cards");
        QVERIFY2(blanks[0][0] > 0, "the budget swapped nothing: the case wasn't made");
        QVERIFY2(blanks[1][1] <= blanks[0][1] + 2, qPrintable(QStringLiteral("dark mode on: %1 blank cards with blurhashes, %2 without")
                                                                    .arg(blanks[1][1])
                                                                    .arg(blanks[0][1])));
    }

    // ---- 2.7.3 (M4, a regression guard: 2.7.2 passes it too): another style picked in Options -> Design right after dark mode
    // went on, a chat hidden from before then to the Disable, which comes as soon as dark mode took the style over (on a quiet
    // machine before the hidden chat's relayout timer drew anything, under load sometimes after it: reported, either way the
    // same must come out; TSMEDIA_DARKCARDS_SETTLED=1: well after it, the visible chat looked through again): the hidden
    // chat's pictures are drawn for the picked style at the unload, exactly as a Disable with dark mode off
    // leaves them after the same pick (U0, both chats looked through after it). The unload's draw with what Core has lists
    // each picture whose still Core's cache lost in lookOld, and those get the cheap decodes (the second chat's pictures are
    // 1600 x 1200 JPEGs). The unload's time is reported (U0 off, U1 on) ---------------------------------------------------
    void styleChangeWhileDarkAtUnload()
    {
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString       out;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(runCardsChild("style",
                              "TSMEDIA_DARKCARDS_LAYOUT=1;TSMEDIA_DARKCARDS_SKIN=light;TSMEDIA_DARKCARDS_BIG=1;TSMEDIA_DARKCARDS_SETS=6;QT_SCALE_FACTOR=3;"
                              "TSMEDIA_DARKCARDS_CHAT2=cheap",
                              &out));
        qInfo("child %lld ms", clock.elapsed());
        static const QRegularExpression timeRe(QStringLiteral("^reload261: (unload U[01] .*)$"), QRegularExpression::MultilineOption);
        for (auto times = timeRe.globalMatch(out); times.hasNext();)
            qInfo("%s", qPrintable(times.next().captured(1).trimmed())); // U0: dark mode off, U1: on
        static const QRegularExpression hiddenRe(QStringLiteral("^reload261: (hidden chat: .*)$"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   hidden = hiddenRe.match(out);
        if (hidden.hasMatch())
            qInfo("%s", qPrintable(hidden.captured(1).trimmed()));
        QVERIFY2(out.contains(QLatin1String("[dark] adopted the skin")), qPrintable(out.right(3000)));
        const CardMap cards = cardsOf(out);
        QVERIFY2(cardsDiffer(cards["L"][2], cards["U0"][2]), "the picked style draws the hidden chat's pictures no differently");
        for (int chat : {1, 2}) {
            const auto& u0       = cards["U0"][chat];
            const auto& u1       = cards["U1"][chat];
            int         compared = 0;
            for (auto it = u1.constBegin(); it != u1.constEnd(); ++it) {
                const QString want = u0.value(it.key());
                if (it.value() == QLatin1String("blank") || want.isEmpty() || want == QLatin1String("blank"))
                    continue;
                ++compared;
                QVERIFY2(it.value() == want, qPrintable(QStringLiteral("chat %1: %2 %3, want %4 (before the pick: %5)")
                                                            .arg(chat)
                                                            .arg(it.key(), it.value(), want, cards["L"][chat].value(it.key()))));
            }
            qInfo("chat %d: %d pictures compared", chat, compared);
            QVERIFY2(compared >= 5, qPrintable(QStringLiteral("chat %1: only %2 pictures to compare").arg(chat).arg(compared)));
        }
    }

    // The child of the darkCards tests: two chats with cards (the second in a hidden tab), the plugin loaded with dark mode
    // off and on, unloaded, toggled; one line per picture of ours at each step. TSMEDIA_DARKCARDS_CHILD names the scenario
    // (disable, reload, toggle, quit, show, offnear; 2.7.3: standin, style).
    void darkCardsChild()
    {
        const QByteArray scenario = qgetenv("TSMEDIA_DARKCARDS_CHILD");
        if (scenario.isEmpty())
            QSKIP("runs as a child of the darkCards tests");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        const int  layout   = qBound(0, qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_LAYOUT"), 2);
        const bool darkSkin = qgetenv("TSMEDIA_DARKCARDS_SKIN") == "dark";

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
        const QString iniPath = dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini");
        const auto    writeIni = [&](bool dark) {
            QFile ini(iniPath);
            QVERIFY(ini.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=false\nmakeLinkPreviews=false\nsharePresence=false\nchatBubbles=true\n");
            ini.write("chatLayout=" + QByteArray::number(layout) + "\n");
            ini.write(dark ? "darkMode=true\n" : "darkMode=false\n");
        };
        writeIni(false);
        // TeamSpeak's skin: its default paints the chat with palette(base); a dark one sets its own background and text.
        const QString darkSheet  = QStringLiteral("QTextEdit#ChatTab { background-color: #2f3136; color: #dcddde; }\n");
        const QString lightSheet = QStringLiteral("QTextEdit#ChatTab { background-color: palette(base); }\n");
        const QString skin       = darkSkin ? darkSheet : lightSheet;
        qApp->setStyleSheet(skin);

        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        QTextBrowser* chats[2]    = {nullptr, nullptr};
        const int     kCardsAt[2] = {20, 6}; // the cards come after this message
        int           seed        = 1;
        for (int i = 0; i < 2; ++i) {
            chats[i] = new QTextBrowser;
            chats[i]->setObjectName(QStringLiteral("ChatTab")); // dark mode themes TeamSpeak's chats by this name
            chats[i]->setOpenLinks(false);
            chats[i]->document()->setDefaultFont(font);
            // 2.7.3: TSMEDIA_DARKCARDS_CHAT2=cheap|huge: the second chat's pictures of that kind.
            fillCardChat(chats[i]->document(), i == 0 ? 160 : 40, kCardsAt[i], &seed, i == 0 ? qMax(1, qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_SETS")) : 1,
                         i == 1 ? qgetenv("TSMEDIA_DARKCARDS_CHAT2") : QByteArray());
            tabs->addTab(chats[i], i == 0 ? QStringLiteral("Fake channel") : QStringLiteral("Server"));
        }
        column->addWidget(tabs);
        window.setCentralWidget(central);
        window.resize(1000, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        tabs->setCurrentIndex(1); // both laid out once, as TeamSpeak's tabs are after a while
        QTest::qWait(50);
        tabs->setCurrentIndex(0);
        QTest::qWait(50);
        QScrollBar* bar = chats[0]->verticalScrollBar();

        const auto record = [&](const char* phase) {
            for (int i = 0; i < 2; ++i) {
                for (const QString& line : cardLines(chats[i]))
                    report(QStringLiteral("card %1 %2 %3").arg(QLatin1String(phase)).arg(i + 1).arg(line));
                for (const QString& line : cardKeys(chats[i])) // 2.7.1 review: which pictures were put again
                    report(QStringLiteral("key %1 %2 %3").arg(QLatin1String(phase)).arg(i + 1).arg(line));
            }
        };
        const auto settleBoth = [&](int minMs) {
            QElapsedTimer t;
            t.start();
            int    r0 = -1, r1 = -1, value = -1;
            qint64 since = 0;
            while (t.elapsed() < 30000) {
                pump(chats[0], 100);
                const int a = chats[0]->document()->revision(), b = chats[1]->document()->revision();
                if (a != r0 || b != r1 || bar->value() != value || pendingDownloads() > 0) {
                    r0    = a;
                    r1    = b;
                    value = bar->value();
                    since = t.elapsed();
                } else if (t.elapsed() - since > 2000 && t.elapsed() > minMs) {
                    return;
                }
            }
        };
        // Every card of both chats once on screen, so the pictures are drawn with everything they show (a picture off screen
        // is drawn again only when it comes near the view: an album's tiles that arrived later, for example). The hidden
        // chat is shown for it and left at its end again.
        const auto visitCards = [&] {
            for (int i : {1, 0}) {
                tabs->setCurrentIndex(i);
                QScrollBar*   b     = chats[i]->verticalScrollBar();
                const int     sets  = i == 0 ? qMax(1, qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_SETS")) : 1;
                const QString first = QStringLiteral("dark %1 ").arg(kCardsAt[i]);
                const QString after = QStringLiteral("dark %1 ").arg(kCardsAt[i] + 10 * (sets - 1) + 1); // past the last set
                pump(chats[i], 100);
                b->setValue(b->value() + qRound(lineY(chats[i], first)));
                for (int step = 0; step < 20 * sets; ++step) {
                    pump(chats[i], 400);
                    if (lineY(chats[i], after) < chats[i]->viewport()->height() || b->value() >= b->maximum())
                        break;
                    b->setValue(b->value() + chats[i]->viewport()->height() * 3 / 4);
                }
                b->setValue(b->maximum());
                pump(chats[i], 100);
            }
        };
        // The reader: the line of message 100, 5 px below the view's top.
        const QString marker = QStringLiteral("dark 100 ");
        const auto    place  = [&] {
            visitCards();
            QVERIFY(lineY(chats[0], marker) < 1e8);
            bar->setValue(bar->value() + qRound(lineY(chats[0], marker)) - 5);
            pump(chats[0], 300);
        };
        const auto measurePlace = [&](const char* phase, qreal placedY, int hiddenGapBefore) {
            report(QStringLiteral("place %1 dy %2 hidden gap %3 -> %4")
                       .arg(QLatin1String(phase))
                       .arg(lineY(chats[0], marker) - placedY, 0, 'f', 1)
                       .arg(hiddenGapBefore)
                       .arg(trueMaximum(chats[1]) - chats[1]->verticalScrollBar()->value()));
        };
        const auto printDark = [&] {
            const QStringList lines = fakets3::logLines();
            for (int i = m_logFrom; i < lines.size(); ++i) {
                if (lines.at(i).contains(QLatin1String("[dark]")) || lines.at(i).contains(QLatin1String("[test] look:"))
                    || lines.at(i).contains(QLatin1String("[test] shutdown: chat")) || lines.at(i).contains(QLatin1String("[test] shutdown: done")))
                    report(QStringLiteral("  plugin ") + lines.at(i));
            }
            m_logFrom = lines.size();
        };

        PluginDll plugin;
        m_logFrom = fakets3::logLines().size();
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settleBoth(0);
        if (scenario == "standin") {
            // 2.7.3 (M3): every picture downloaded and drawn once (R); then a load with dark mode off (U0) and one with it on
            // (U1), each unloaded (a Disable) as soon as the load drew every picture: the 12 MP files' stills are still being
            // read on a worker then, so those pictures are drawn from a stand-in (their blurhash, or nothing: the placeholder).
            place();
            record("R");
            plugin.unload();
            m_plugin = nullptr;
            const auto quickUnload = [&](bool dark, const char* phase) {
                writeIni(dark);
                const QStringList before = cardKeys(chats[0]) + cardKeys(chats[1]);
                m_logFrom                = fakets3::logLines().size();
                QCOMPARE(plugin.load(dll), QString());
                m_plugin = &plugin;
                QElapsedTimer waited;
                waited.start();
                int left = before.size();
                while (waited.elapsed() < 5000 && left > 0) {
                    QCoreApplication::processEvents(); // no delivery, no sleep: one turn at a time
                    const QStringList now = cardKeys(chats[0]) + cardKeys(chats[1]);
                    left                  = 0;
                    for (const QString& line : now)
                        left += before.contains(line) ? 1 : 0;
                }
                QElapsedTimer unloadClock;
                unloadClock.start();
                plugin.unload();
                report(QStringLiteral("unload %1 %2 ms (the load drew its pictures in %3 ms, %4 not drawn)")
                           .arg(QLatin1String(phase))
                           .arg(unloadClock.elapsed())
                           .arg(waited.elapsed())
                           .arg(left));
                m_plugin = nullptr;
                printDark();
                record(phase);
            };
            quickUnload(false, "U0");
            quickUnload(true, "U1");
            QTest::qWait(100);
            return;
        }
        if (scenario == "style") {
            // 2.7.3 (M4): another style picked in Options -> Design (TeamSpeak's application sheet) while dark mode is on, the
            // second chat hidden from before dark mode went on to the Disable, its stills no longer in Core's cache (the first
            // chat's pictures were drawn after them). The reference (U0): the same pick with dark mode off, both chats looked
            // through once after it (every picture drawn for the picked style), then a Disable.
            const QString picked = darkSkin ? lightSheet : darkSheet;
            place();
            record("R");
            qApp->setStyleSheet(picked);
            settleBoth(2000);
            visitCards();
            tabs->setCurrentIndex(0);
            settleBoth(1000);
            record("P0");
            QElapsedTimer unloadClock;
            unloadClock.start();
            plugin.unload();
            report(QStringLiteral("unload U0 %1 ms").arg(unloadClock.elapsed()));
            m_plugin = nullptr;
            printDark();
            record("U0");
            qApp->setStyleSheet(skin); // the first style again while TS Media is off
            m_logFrom = fakets3::logLines().size();
            QCOMPARE(plugin.load(dll), QString());
            m_plugin = &plugin;
            settleBoth(0);
            place(); // the second chat looked through first, the first chat after it
            record("L");
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 10, 0); // dark mode on, the second chat hidden
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            if (qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_SETTLED") == 1) {
                settleBoth(2000);
                record("D");
                qApp->setStyleSheet(picked); // TeamSpeak's pick while dark mode is on: dark mode takes it over
                settleBoth(2000);
                // The visible chat looked through again (each of its pictures drawn for the new look, its stills the newest).
                for (int step = 0; step < 400 && bar->value() > 0; ++step) {
                    bar->setValue(qMax(0, bar->value() - chats[0]->viewport()->height()));
                    pump(chats[0], 200);
                }
                bar->setValue(bar->maximum());
                settleBoth(1000);
            } else {
                // 2.7.3 review: the pick right after the on, and the Disable as soon as dark mode took it over (its 0 ms timer):
                // on a quiet machine the hidden chat's relayout timer (200 ms after the on) hasn't drawn its pictures for either
                // look then, and Core's cache lost their stills (the first chat's were drawn after them in L). Reported: how
                // many it drew before the Disable (under load the on and the pick can take longer than that timer).
                const QStringList hidden = cardKeys(chats[1]);
                QElapsedTimer     turn;
                turn.start();
                const int from = fakets3::logLines().size();
                qApp->setStyleSheet(picked); // TeamSpeak's pick while dark mode is on: dark mode takes it over
                bool adopted = false;
                while (!adopted && turn.elapsed() < 5000) {
                    QCoreApplication::processEvents();
                    const QStringList lines = fakets3::logLines();
                    for (int i = from; i < lines.size() && !adopted; ++i)
                        adopted = lines.at(i).contains(QLatin1String("[dark] adopted the skin"));
                }
                int drawn = 0;
                for (const QString& line : cardKeys(chats[1]))
                    drawn += hidden.contains(line) ? 0 : 1;
                report(QStringLiteral("hidden chat: %1 of %2 pictures drawn before the Disable (taken over in %3 ms)")
                           .arg(drawn)
                           .arg(hidden.size())
                           .arg(turn.elapsed()));
            }
            record("P1");
            unloadClock.start();
            plugin.unload();
            report(QStringLiteral("unload U1 %1 ms").arg(unloadClock.elapsed()));
            m_plugin = nullptr;
            printDark();
            record("U1");
            QTest::qWait(100);
            return;
        }
        if (scenario == "quit" || scenario == "show") {
            // 2.7.1 review: dark mode on from the first tab while the second is hidden (TSMEDIA_DARKCARDS_SETTLE=0: no event
            // loop turn after it; TSMEDIA_DARKCARDS_OFF=1: off again, from the first tab too).
            const bool settle = qgetenv("TSMEDIA_DARKCARDS_SETTLE") != "0";
            place();
            record("R");
            QVERIFY(plugin.menuItem);
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 10, 0);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            if (settle)
                settleBoth(3000);
            if (qEnvironmentVariableIntValue("TSMEDIA_DARKCARDS_OFF") == 1) {
                plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 11, 0);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
                if (settle)
                    settleBoth(3000);
            }
            if (scenario == "show") {
                record("S0");
                tabs->setCurrentIndex(1); // the hidden chat's Show, no event loop turn
                record("S1");
                settleBoth(2000);
                record("S2");
                printDark();
                plugin.unload();
                m_plugin = nullptr;
                QTest::qWait(100);
                return;
            }
            record("Q0");
            QElapsedTimer quitClock;
            window.hide(); // TeamSpeak's Quit: it hides its main window, then lets the plugins go
            quitClock.start();
            plugin.unload();
            m_plugin = nullptr;
            report(QStringLiteral("quit unload %1 ms").arg(quitClock.elapsed()));
            printDark();
            record("Q1");
            QTest::qWait(100);
            return;
        }
        if (scenario == "offnear") {
            // 2.7.2 (D1): dark mode on, every card seen in the dark look, the chat at its end, off; then up a screen at a time,
            // one visibility pass per step: each picture that came near is drawn for TeamSpeak's look (from its kept still
            // when Core's cache has lost it), none keeps its dark drawing.
            place();
            record("R");
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 10, 0);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            settleBoth(3000);
            visitCards();
            tabs->setCurrentIndex(0);
            bar->setValue(bar->maximum());
            settleBoth(2000);
            record("Dsettled");
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 11, 0);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            for (int step = 0; step < 400 && bar->value() > 0; ++step) {
                bar->setValue(qMax(0, bar->value() - chats[0]->viewport()->height()));
                pump(chats[0], 300);
            }
            record("Up");
            printDark();
            plugin.unload();
            m_plugin = nullptr;
            QTest::qWait(100);
            return;
        }
        if (scenario == "toggle") {
            place();
            record("R");
            QVERIFY(plugin.menuItem);
            // Turn on dark mode (Plugins menu), then only the queued call it makes: no timer, no paint in between.
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 10, 0);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            record("T1");
            tabs->setCurrentIndex(1); // the hidden chat's Show, still no event loop turn
            record("T2");
            settleBoth(5000);
            record("Dsettled");
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 11, 0); // off, from the second tab
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            record("Toff1");
            tabs->setCurrentIndex(0);
            record("Toff2");
            settleBoth(5000);
            record("Rsettled");
            printDark();
            plugin.unload();
            m_plugin = nullptr;
            QTest::qWait(100);
            return;
        }
        // disable / reload: a Disable with dark mode off first (the reference), then dark mode on and a Disable.
        place();
        record("R");
        const qreal lightY   = lineY(chats[0], marker);
        const int   lightGap = trueMaximum(chats[1]) - chats[1]->verticalScrollBar()->value();
        report(QStringLiteral("before U0: ") + describe(whereOf(chats[0])));
        QElapsedTimer unloadClock; // 2.7.2: the unload's own time (this DLL has no test hooks to say it)
        unloadClock.start();
        plugin.unload();
        report(QStringLiteral("unload U0 %1 ms").arg(unloadClock.elapsed()));
        m_plugin = nullptr;
        report(QStringLiteral("after U0: ") + describe(whereOf(chats[0])));
        printDark();
        record("U0");
        measurePlace("U0", lightY, lightGap);
        writeIni(true);
        m_logFrom = fakets3::logLines().size();
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settleBoth(0);
        place();
        record("D");
        const qreal placedY   = lineY(chats[0], marker);
        const int   hiddenGap = trueMaximum(chats[1]) - chats[1]->verticalScrollBar()->value();
        report(QStringLiteral("before U1: ") + describe(whereOf(chats[0])));
        unloadClock.start();
        plugin.unload();
        report(QStringLiteral("unload U1 %1 ms").arg(unloadClock.elapsed()));
        m_plugin = nullptr;
        report(QStringLiteral("after U1: ") + describe(whereOf(chats[0])));
        printDark();
        record("U1");
        measurePlace("U1", placedY, hiddenGap);
        report(QStringLiteral("sheet back %1").arg(qApp->styleSheet() == skin ? 1 : 0));
        if (scenario == "reload") {
            QTest::qWait(300); // TeamSpeak's event loop (the Options dialog) before the load
            record("G");
            m_logFrom = fakets3::logLines().size();
            QCOMPARE(plugin.load(dll), QString());
            m_plugin = &plugin;
            settleBoth(0);
            place();
            record("D2");
            plugin.unload();
            m_plugin = nullptr;
        }
        QTest::qWait(100);
    }

    // ---- 2.7.2 (D3): a chat hidden while dark mode was toggled is laid out for the new look at its first paint ----------
    // The real plugin under a TeamSpeak-like window: a ChatTabWidget with six chats (TS Media pictures, albums, replies, polls,
    // your own messages on the right, day lines), the reader scrolled up in one of the hidden ones. Dark mode on from the
    // visible chat ("on"), or on, that chat shown once in the dark look and hidden again, then off ("off"); then the hidden
    // chat is opened. Its first paint (a grab at its Show, no event-loop turn) must be its settled look: the same pixels 1.5 s
    // later, no horizontal scroll bar, a column as wide as before the toggle, the reader's line where it was. 2.7.1: the
    // dark sheet's pane changed the column's width by a few px, so the chat was laid out again at its Show and TS Media's
    // width-dependent rows followed 180-200 ms later (a horizontal scroll bar dark -> light, a shift light -> dark). The
    // toggle's time with the five hidden chats is reported.
    static QVector<QPair<QByteArray, QByteArray>> darkShowRows() // row name, the child's environment
    {
        QVector<QPair<QByteArray, QByteArray>> rows;
        for (const char* scenario : {"on", "off"}) {
            const QByteArray child = QByteArray("TSMEDIA_DARKSHOW_CHILD=") + scenario;
            rows.append({QByteArray(scenario) + ", Cozy", child + ";TSMEDIA_DARKSHOW_LAYOUT=1"});
            rows.append({QByteArray(scenario) + ", Compact", child + ";TSMEDIA_DARKSHOW_LAYOUT=2"});
            rows.append({QByteArray(scenario) + ", TeamSpeak classic", child + ";TSMEDIA_DARKSHOW_LAYOUT=0"});
            for (const char* dpi : {"144", "192"})
                rows.append({QByteArray(scenario) + ", Cozy, QT_FONT_DPI=" + dpi, child + ";TSMEDIA_DARKSHOW_LAYOUT=1;QT_FONT_DPI=" + dpi});
        }
        return rows;
    }
    void darkHiddenChatFirstPaint_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const auto& row : darkShowRows())
            QTest::newRow(row.first.constData()) << row.second;
    }
    void darkHiddenChatFirstPaint()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKSHOW_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        runRowsSideBySide("darkHiddenChatFirstPaint", "darkShowChild", darkShowRows(), QByteArray());
        QString out;
        QVERIFY(runNamedChild("darkShowChild", extra, &out));
        static const QRegularExpression re(QStringLiteral("^reload261: darkshow width (\\d+) -> (\\d+) hbar (\\d+) document (\\d+) dy (-?[0-9.]+) settled dy (-?[0-9.]+) "
                                                          "first paint (\\d+) px differ \\(of (\\d+)\\)"),
                                           QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = re.match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.right(3000)));
        qInfo("%s", qPrintable(m.captured(0).mid(11)));
        static const QRegularExpression times(QStringLiteral("^reload261: darkshow (toggle|differ) .*$"), QRegularExpression::MultilineOption);
        for (auto it = times.globalMatch(out); it.hasNext();)
            qInfo("%s", qPrintable(it.next().captured(0).mid(11).trimmed()));
        QCOMPARE(m.captured(2).toInt(), m.captured(1).toInt()); // the column as wide as before the toggle
        QCOMPARE(m.captured(3).toInt(), 0);                     // no horizontal scroll bar at the first paint
        QVERIFY2(m.captured(4).toInt() <= m.captured(2).toInt(), qPrintable(m.captured(0)));
        QVERIFY2(qAbs(m.captured(5).toDouble()) <= 2, qPrintable(m.captured(0)));
        QVERIFY2(qAbs(m.captured(6).toDouble()) <= 2, qPrintable(m.captured(0)));
        QVERIFY2(m.captured(7).toInt() == 0, qPrintable(m.captured(0) + QStringLiteral(" (the first paint isn't the settled look)")));
    }

    void darkShowChild()
    {
        const QByteArray scenario = qgetenv("TSMEDIA_DARKSHOW_CHILD");
        if (scenario.isEmpty())
            QSKIP("runs as a child of darkHiddenChatFirstPaint");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        QCoreApplication::setAttribute(Qt::AA_Use96Dpi, false); // TeamSpeak's DPI (zoomShowChild)
        const auto use96  = qScopeGuard([] { QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true); });
        const int  layout = qBound(0, qEnvironmentVariableIntValue("TSMEDIA_DARKSHOW_LAYOUT"), 2);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
        {
            QFile ini(dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=false\nmakeLinkPreviews=false\nsharePresence=false\nchatBubbles=true\ndarkMode=false\n");
            ini.write("chatLayout=" + QByteArray::number(layout) + "\n");
        }
        const QString skin = QStringLiteral("QTextEdit#ChatTab { background-color: palette(base); }\n");
        qApp->setStyleSheet(skin);
        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        const tschat::Skin s = tschat::darkSkin();
        QVector<QTextBrowser*> chats;
        for (int i = 0; i < 6; ++i) {
            auto* b = new QTextBrowser;
            b->setObjectName(QStringLiteral("ChatTab"));
            b->setOpenLinks(false);
            b->setFont(font);
            if (i == 0) {
                for (int k = 1; k <= 30; ++k)
                    b->append(tschat::messageHtml(s, QStringLiteral("19:%1:00").arg(k, 2, 10, QLatin1Char('0')), 17, kAlice, QStringLiteral("Alice"),
                                                  QStringLiteral("server %1 a line in the server chat").arg(k)));
            } else {
                fillChat(b->document(), i == 1 ? 90 : 50, "mixed");
            }
            tabs->addTab(b, i == 0 ? QStringLiteral("Fake server") : QStringLiteral("Chat %1").arg(i));
            chats << b;
        }
        column->addWidget(tabs);
        window.setCentralWidget(central);
        window.resize(1000, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        PluginDll plugin;
        m_logFrom = fakets3::logLines().size();
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        QTextBrowser* chat = chats[1];
        for (int i = 5; i >= 0; --i) { // every chat laid out once with the plugin (the downloads in)
            tabs->setCurrentIndex(i);
            settle(chats[i], i == 1 ? 15000 : 6000);
        }
        tabs->setCurrentIndex(1);
        QScrollBar*   bar    = chat->verticalScrollBar();
        const QString marker = QStringLiteral("r261 41 ");
        QVERIFY(lineY(chat, marker) < 1e8);
        bar->setValue(bar->value() + qRound(lineY(chat, marker)) - 5);
        settle(chat, 8000);
        const chatscroll::ReaderPlace reader   = chatscroll::captureReader(chat);
        const auto                    readerAt = [&] { return chatscroll::readerY(chat->document(), reader) - bar->value(); };
        const qreal                   y0       = readerAt();
        const int                     before   = chat->viewport()->width();
        const auto                    toggle   = [&](bool on) {
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, on ? 10 : 11, 0);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        };
        QVERIFY(plugin.menuItem);
        tabs->setCurrentIndex(0); // the chat hidden; the server chat in front
        settle(chats[0], 3000);
        if (scenario == "off") { // shown once in the dark look, hidden again, then dark mode off
            toggle(true);
            settle(chats[0], 3000);
            tabs->setCurrentIndex(1);
            settle(chat, 4000);
            tabs->setCurrentIndex(0);
            settle(chats[0], 3000);
            toggle(false);
        } else {
            toggle(true);
        }
        settle(chats[0], 3000);
        QElapsedTimer           since;
        QMetaObject::Connection ranges;
        if (qEnvironmentVariableIntValue("TSMEDIA_DARKSHOW_LOG") == 1) { // every range change after the Show
            since.start();
            ranges = connect(bar, &QScrollBar::rangeChanged, this, [&](int, int max) {
                report(QStringLiteral("  range %1 ms: max %2 value %3 (log %4)").arg(since.elapsed()).arg(max).arg(bar->value()).arg(fakets3::logLines().size()));
            });
        }
        tabs->setCurrentIndex(1); // the Show; no event-loop turn before the first paint (the grab paints it)
        const int   width  = chat->viewport()->width();
        const int   hbar   = chat->horizontalScrollBar()->maximum();
        const int   docW   = qCeil(chat->document()->size().width());
        const qreal showDy = readerAt() - y0;
        const QImage first = chat->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        settle(chat, 6000);
        QObject::disconnect(ranges);
        const QImage later = chat->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        int   differ = 0;
        QRect where; // what differs (the log names it)
        if (first.size() != later.size()) {
            differ = first.width() * first.height();
        } else {
            for (int y = 0; y < first.height(); ++y) {
                const QRgb* a = reinterpret_cast<const QRgb*>(first.constScanLine(y));
                const QRgb* b = reinterpret_cast<const QRgb*>(later.constScanLine(y));
                for (int x = 0; x < first.width(); ++x) {
                    if (a[x] != b[x]) {
                        ++differ;
                        where |= QRect(x, y, 1, 1);
                    }
                }
            }
        }
        if (differ > 0)
            report(QStringLiteral("darkshow differ in %1,%2 %3x%4 of %5x%6").arg(where.x()).arg(where.y()).arg(where.width()).arg(where.height()).arg(first.width()).arg(first.height()));
        if (differ > 0 && !qEnvironmentVariableIsEmpty("TSMEDIA_DARKSHOW_SHOTS")) {
            first.save(qEnvironmentVariable("TSMEDIA_DARKSHOW_SHOTS") + QStringLiteral("/first.png"));
            later.save(qEnvironmentVariable("TSMEDIA_DARKSHOW_SHOTS") + QStringLiteral("/later.png"));
        }
        report(QStringLiteral("darkshow width %1 -> %2 hbar %3 document %4 dy %5 settled dy %6 first paint %7 px differ (of %8)")
                   .arg(before)
                   .arg(width)
                   .arg(hbar)
                   .arg(docW)
                   .arg(showDy, 0, 'f', 1)
                   .arg(readerAt() - y0, 0, 'f', 1)
                   .arg(differ)
                   .arg(first.width() * first.height()));
        static const QRegularExpression timeRe(QStringLiteral("\\[dark\\] (on|off) in (\\d+) ms"));
        QStringList                     toggles;
        const QStringList               lines = fakets3::logLines();
        for (int i = m_logFrom; i < lines.size(); ++i) {
            const QRegularExpressionMatch t = timeRe.match(lines.at(i));
            if (t.hasMatch())
                toggles << t.captured(1) + QLatin1Char(' ') + t.captured(2);
            if (lines.at(i).contains(QLatin1String("[dark] chat column")))
                report(QStringLiteral("  plugin ") + lines.at(i));
        }
        report(QStringLiteral("darkshow toggle with 5 hidden chats (ms): %1").arg(toggles.join(QStringLiteral(", "))));
        plugin.unload();
        m_plugin = nullptr;
        QTest::qWait(100);
    }

    // ---- 2.7.2 (D4): a chat-zoom change before a hidden chat is opened keeps its reader's line ---------------------------
    // The live sequence: the channel chat scrolled up (its reader at message 42) and hidden behind the server chat,
    // Options -> Addons -> Reload (or none), Ctrl + wheel in the server chat, then the channel chat opened. Its reader's
    // line must be where it was at once (the Show, no event-loop turn: the first paint) and after everything settled; a
    // chat at its end stays at its end. 2.7.1: 54 px off at the Show at 100 % (109 px at QT_FONT_DPI=144, 352 at 192),
    // put back by the scale pass after the first paint (live: never, 68 px).
    static QVector<QPair<QByteArray, QByteArray>> zoomShowRows() // row name, the child's environment (after TSMEDIA_ZOOMSHOW_CHILD=1;)
    {
        QVector<QPair<QByteArray, QByteArray>> rows;
        for (const char* layout : {"1", "2"}) {
            for (const char* scaling : {"", "QT_FONT_DPI=144", "QT_FONT_DPI=192"}) {
                rows.append({QByteArray("Reload, ") + (layout[0] == '1' ? "Cozy" : "Compact") + ", " + (scaling[0] ? scaling : "100%"),
                             QByteArray("TSMEDIA_ZOOMSHOW_RELOAD=1;TSMEDIA_ZOOMSHOW_LAYOUT=") + layout + ";" + scaling});
            }
        }
        rows.append({"no Reload, Cozy, 100%", "TSMEDIA_ZOOMSHOW_RELOAD=0;TSMEDIA_ZOOMSHOW_LAYOUT=1"});
        rows.append({"no Reload, Cozy, QT_FONT_DPI=192", "TSMEDIA_ZOOMSHOW_RELOAD=0;TSMEDIA_ZOOMSHOW_LAYOUT=1;QT_FONT_DPI=192"});
        rows.append({"at its end, Reload, Cozy, 100%", "TSMEDIA_ZOOMSHOW_RELOAD=1;TSMEDIA_ZOOMSHOW_LAYOUT=1;TSMEDIA_ZOOMSHOW_END=1"});
        rows.append({"at its end, Reload, Compact, QT_FONT_DPI=192", "TSMEDIA_ZOOMSHOW_RELOAD=1;TSMEDIA_ZOOMSHOW_LAYOUT=2;TSMEDIA_ZOOMSHOW_END=1;QT_FONT_DPI=192"});
        return rows;
    }
    void zoomAtShowKeepsTheReader_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const auto& row : zoomShowRows())
            QTest::newRow(row.first.constData()) << row.second;
    }
    void zoomAtShowKeepsTheReader()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_ZOOMSHOW_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        runRowsSideBySide("zoomAtShowKeepsTheReader", "zoomShowChild", zoomShowRows(), "TSMEDIA_ZOOMSHOW_CHILD=1;");
        QString out;
        QVERIFY(runNamedChild("zoomShowChild", "TSMEDIA_ZOOMSHOW_CHILD=1;" + extra, &out));
        static const QRegularExpression re(QStringLiteral("^reload261: zoomshow font (\\S+) -> (\\S+) show dy (-?[0-9.]+) gap (-?\\d+) settled dy (-?[0-9.]+) gap (-?\\d+)"),
                                           QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = re.match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.right(3000)));
        qInfo("%s", qPrintable(m.captured(0).mid(11)));
        QVERIFY2(m.captured(1) != m.captured(2), qPrintable(m.captured(0))); // the zoom changed in the server chat
        if (extra.contains("END=1")) {
            QVERIFY2(m.captured(4).toInt() >= 0 && m.captured(4).toInt() <= 2, qPrintable(m.captured(0) + QStringLiteral(" (not at its end at the Show)")));
            QVERIFY2(m.captured(6).toInt() >= 0 && m.captured(6).toInt() <= 2, qPrintable(m.captured(0) + QStringLiteral(" (not at its end after)")));
            return;
        }
        QVERIFY2(qAbs(m.captured(3).toDouble()) <= 2, qPrintable(m.captured(0) + QStringLiteral(" (the reader's line moved at the Show)")));
        QVERIFY2(qAbs(m.captured(5).toDouble()) <= 2, qPrintable(m.captured(0) + QStringLiteral(" (the reader's line moved)")));
    }

    void zoomShowChild()
    {
        if (qEnvironmentVariableIsEmpty("TSMEDIA_ZOOMSHOW_CHILD"))
            QSKIP("runs as a child of zoomAtShowKeepsTheReader");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        // TeamSpeak's DPI: this executable assumes 96 DPI (testmain), so Qt would draw every picture of a chat twice as big
        // as TeamSpeak does at QT_FONT_DPI=192 (its image handler scales by the viewport's DPI over the default DPI), and a
        // picture's new size above the view would move the text by twice what the plugin keeps the place for.
        QCoreApplication::setAttribute(Qt::AA_Use96Dpi, false);
        const auto use96 = qScopeGuard([] { QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true); });
        const int  layout = qBound(0, qEnvironmentVariableIntValue("TSMEDIA_ZOOMSHOW_LAYOUT"), 2);
        const bool reload = qEnvironmentVariableIntValue("TSMEDIA_ZOOMSHOW_RELOAD") == 1;
        const bool atEnd  = qEnvironmentVariableIntValue("TSMEDIA_ZOOMSHOW_END") == 1;

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
        {
            QFile ini(dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=false\nmakeLinkPreviews=false\nsharePresence=false\nchatBubbles=true\n");
            ini.write("chatLayout=" + QByteArray::number(layout) + "\n");
        }
        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        const tschat::Skin s       = tschat::darkSkin();
        auto*              server  = new QTextBrowser;
        auto*              channel = new QTextBrowser;
        for (QTextBrowser* b : {server, channel}) {
            b->setObjectName(QStringLiteral("ChatTab"));
            b->setOpenLinks(false);
            b->setFont(font); // TeamSpeak's chat font is the widget's (the zoom's base); QTextEdit gives it to its document
        }
        for (int i = 1; i <= 40; ++i)
            server->append(tschat::messageHtml(s, QStringLiteral("19:%1:00").arg(i, 2, 10, QLatin1Char('0')), 17, kAlice, QStringLiteral("Alice"),
                                               QStringLiteral("server %1 a line in the server chat").arg(i)));
        fillChat(channel->document(), 120, "mixed");
        tabs->addTab(server, QStringLiteral("Fake server"));
        tabs->addTab(channel, QStringLiteral("Fake channel"));
        column->addWidget(tabs);
        window.setCentralWidget(central);
        window.resize(1000, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        tabs->setCurrentIndex(1);
        QTest::qWait(50);
        tabs->setCurrentIndex(0);
        QTest::qWait(50);

        PluginDll plugin;
        m_logFrom = fakets3::logLines().size();
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settle(server, 8000);
        tabs->setCurrentIndex(1);
        settle(channel, 20000);
        QScrollBar*   bar    = channel->verticalScrollBar();
        const QString marker = QStringLiteral("r261 42 ");
        if (atEnd) {
            bar->setValue(bar->maximum());
        } else {
            QVERIFY(lineY(channel, marker) < 1e8);
            bar->setValue(bar->value() + qRound(lineY(channel, marker)) - 5);
        }
        settle(channel, 8000);
        // The reader's line as TS Media reads it (its message's top, or the line of TeamSpeak's text at the view's top; the
        // cursor follows the edits): where it is on screen.
        const chatscroll::ReaderPlace reader   = chatscroll::captureReader(channel);
        const auto                    readerAt = [&] { return chatscroll::readerY(channel->document(), reader) - bar->value(); };
        const qreal                   y0       = atEnd ? lineY(channel, marker) : readerAt();
        report(QStringLiteral("zoomshow placed: ") + describe(whereOf(channel)) + QStringLiteral(" reader line %1 (%2)").arg(y0).arg(reader.line ? "a line" : "a message"));
        tabs->setCurrentIndex(0); // the channel chat hidden behind the server chat
        settle(server, 3000);
        if (reload) {
            plugin.unload();
            m_plugin = nullptr;
            QTest::qWait(300); // TeamSpeak's event loop (the Options dialog) between the unload and the load
            QCOMPARE(plugin.load(dll), QString());
            m_plugin = &plugin;
            settle(server, 8000);
        }
        // Ctrl + wheel in the server chat: one notch up (the zoom TS Media keeps for every chat).
        const qreal before = server->font().pointSizeF();
        {
            QWidget*      vp = server->viewport();
            const QPointF pos(vp->width() / 2.0, vp->height() / 2.0);
            QWheelEvent   wheel(pos, QPointF(vp->mapToGlobal(pos.toPoint())), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(vp, &wheel);
        }
        settle(server, 3000);
        const qreal after = server->font().pointSizeF();
        report(QStringLiteral("zoomshow hidden before the Show: ") + describe(whereOf(channel)));
        QElapsedTimer           since;
        QMetaObject::Connection trace;
        if (qEnvironmentVariableIntValue("TSMEDIA_ZOOMSHOW_LOG") == 1) { // every move after the Show, with the plugin's log so far
            since.start();
            trace = connect(bar, &QScrollBar::valueChanged, this, [&, channel](int value) {
                QString blocks; // the blocks around the reader's: number:height (a change there moves the reader)
                QTextBlock tb = channel->document()->lastBlock();
                while (tb.isValid() && !tb.text().contains(marker))
                    tb = tb.previous();
                for (int k = 0; k < 6 && tb.isValid(); ++k, tb = tb.previous())
                    blocks += QStringLiteral(" %1:%2").arg(tb.blockNumber()).arg(channel->document()->documentLayout()->blockBoundingRect(tb).height(), 0, 'f', 0);
                const layoutdoc::Anchor a = layoutdoc::anchorAt(channel->document(), value);
                report(QStringLiteral("  move %1 ms: %2/%3 line top %9 dy %4 (log %5) anchor %6+%7 blocks%8")
                           .arg(since.elapsed())
                           .arg(value)
                           .arg(channel->verticalScrollBar()->maximum())
                           .arg((atEnd ? lineY(channel, marker) : readerAt()) - y0, 0, 'f', 1)
                           .arg(fakets3::logLines().size())
                           .arg(a.block)
                           .arg(a.offset, 0, 'f', 0)
                           .arg(blocks)
                           .arg(lineY(channel, marker) + value, 0, 'f', 0));
            });
        }
        tabs->setCurrentIndex(1); // the Show; no event-loop turn before the measure (what the first paint shows)
        const qreal showDy  = (atEnd ? lineY(channel, marker) : readerAt()) - y0;
        const int   showGap = trueMaximum(channel) - bar->value();
        settle(channel, 8000);
        QObject::disconnect(trace);
        report(QStringLiteral("zoomshow settled: ") + describe(whereOf(channel)));
        report(QStringLiteral("zoomshow font %1 -> %2 show dy %3 gap %4 settled dy %5 gap %6")
                   .arg(before)
                   .arg(after)
                   .arg(showDy, 0, 'f', 1)
                   .arg(showGap)
                   .arg((atEnd ? lineY(channel, marker) : readerAt()) - y0, 0, 'f', 1)
                   .arg(trueMaximum(channel) - bar->value()));
        if (qEnvironmentVariableIntValue("TSMEDIA_ZOOMSHOW_LOG") == 1) { // every plugin line since the load (a TESTHOOKS DLL says more)
            const QStringList lines = fakets3::logLines();
            for (int i = m_logFrom; i < lines.size(); ++i)
                report(QStringLiteral("  plugin %1 ").arg(i + 1) + lines.at(i));
            m_logFrom = lines.size();
        }
        printPluginSteps();
        plugin.unload();
        m_plugin = nullptr;
        QTest::qWait(100);
    }

    // ---- 2.7.3 (M1): the layout's geometry follows the chat's zoom on every path -----------------------------------------
    // Cozy's and Compact's row geometry (the content column G or the time column L, Cozy's line height) comes from the chat's
    // font (layoutart::tokensFor). 2.7.2 restyled a hidden chat at its Show with the new zoom's tokens (D4), but a zoom in a
    // shown chat (Ctrl + wheel) changed them only with the next line TeamSpeak added: after "zoom while hidden, open, zoom
    // back" the rows kept the larger geometry (the live N3: 52/17 at 100 %, a 125 px message 132 px). The real plugin in a
    // fake TeamSpeak window, a chat of 160 lines, its reader in the middle (or at its end): zoom +1 and -1 while shown, +1 in
    // the server chat while the channel chat is hidden and its Show, -1 there, a new line, Reload. After each step (and at
    // the Show for the rows on screen, before any event-loop turn) every message row has the geometry of the chat's current
    // font, and the reader's line is where it was (a chat at its end stays at its end). Classic: nothing styled, the place
    // kept. 2.7.4 (S2, the crash rule): then a new chat font with the unload right after it (S7): the step the font change
    // posts to the chat layout is dropped with the layout and never runs after the DLL is gone (a functor posted anywhere
    // else, such as to qApp, crashes this child after the unload: tried).
    static QVector<QPair<QByteArray, QByteArray>> zoomLayoutRows() // row name, the child's environment (after TSMEDIA_ZOOMLAYOUT_CHILD=1;)
    {
        QVector<QPair<QByteArray, QByteArray>> rows;
        for (const char* layout : {"1", "2"}) {
            for (const char* scaling : {"", "QT_FONT_DPI=144", "QT_FONT_DPI=192"}) {
                rows.append({QByteArray(layout[0] == '1' ? "Cozy" : "Compact") + ", " + (scaling[0] ? scaling : "100%"),
                             QByteArray("TSMEDIA_ZOOMLAYOUT_LAYOUT=") + layout + ";" + scaling});
            }
        }
        rows.append({"at its end, Cozy, QT_FONT_DPI=192", "TSMEDIA_ZOOMLAYOUT_LAYOUT=1;TSMEDIA_ZOOMLAYOUT_END=1;QT_FONT_DPI=192"});
        rows.append({"at its end, Compact, 100%", "TSMEDIA_ZOOMLAYOUT_LAYOUT=2;TSMEDIA_ZOOMLAYOUT_END=1"});
        rows.append({"Classic, 100%", "TSMEDIA_ZOOMLAYOUT_LAYOUT=0"});
        // Merge check (2.7.3): the same with dark mode on from the load, then off after S5 (M1's restyles for the zoom, with
        // dark mode's look, and the way back of M5/M8), then on again for the Reload.
        rows.append({"dark mode, Cozy, QT_FONT_DPI=144", "TSMEDIA_ZOOMLAYOUT_LAYOUT=1;TSMEDIA_ZOOMLAYOUT_DARK=1;QT_FONT_DPI=144"});
        rows.append({"dark mode, Compact, 100%", "TSMEDIA_ZOOMLAYOUT_LAYOUT=2;TSMEDIA_ZOOMLAYOUT_DARK=1"});
        return rows;
    }
    void zoomLayoutFollowsTheFont_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const auto& row : zoomLayoutRows())
            QTest::newRow(row.first.constData()) << row.second;
    }
    void zoomLayoutFollowsTheFont()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_ZOOMLAYOUT_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        runRowsSideBySide("zoomLayoutFollowsTheFont", "zoomLayoutChild", zoomLayoutRows(), "TSMEDIA_ZOOMLAYOUT_CHILD=1;");
        QString out;
        QVERIFY(runNamedChild("zoomLayoutChild", "TSMEDIA_ZOOMLAYOUT_CHILD=1;" + extra, &out));
        static const QRegularExpression re(QStringLiteral("^reload261: zoomlayout (S\\w+) .* styled (\\d+) rows (\\d+) off (\\d+) .* dy (-?[0-9.]+) gap (-?\\d+)"),
                                           QRegularExpression::MultilineOption);
        const bool  classic = extra.contains("LAYOUT=0");
        const bool  atEnd   = extra.contains("END=1");
        QStringList steps;
        QStringList failures;
        auto        it = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m    = it.next();
            const QString                 line = m.captured(0).mid(11);
            qInfo("%s", qPrintable(line));
            steps.append(m.captured(1));
            if (classic && m.captured(2).toInt() != 0)
                failures.append(line + QStringLiteral(" (styled in Classic)"));
            if (!classic && m.captured(3).toInt() == 0)
                failures.append(line + QStringLiteral(" (no message rows)"));
            if (!classic && m.captured(4).toInt() != 0)
                failures.append(line + QStringLiteral(" (rows with another font's geometry)"));
            if (atEnd && !(m.captured(6).toInt() >= 0 && m.captured(6).toInt() <= 2))
                failures.append(line + QStringLiteral(" (not at its end)"));
            if (!atEnd && qAbs(m.captured(5).toDouble()) > 2)
                failures.append(line + QStringLiteral(" (the reader's line moved)"));
        }
        // Dark rows: nothing in either chat keeps a dark-mode colour after off and after the unload (fragments, block
        // character formats, the chat cursor's format). At S5 (the control) the light rows have none and the dark rows,
        // still on, have them.
        const bool                      dark = extra.contains("DARK=1");
        static const QRegularExpression left(QStringLiteral("^reload261: zoomlayout-darkleft (\\w+) (\\d+) \\([^)]*\\)"), QRegularExpression::MultilineOption);
        QStringList                     checked;
        for (auto l = left.globalMatch(out); l.hasNext();) {
            const QRegularExpressionMatch m      = l.next();
            const bool                    whileOn = dark && m.captured(1) == QLatin1String("S5");
            qInfo("%s", qPrintable(m.captured(0).mid(11)));
            checked.append(m.captured(1));
            if (whileOn ? m.captured(2).toInt() == 0 : m.captured(2).toInt() != 0)
                failures.append(m.captured(0).mid(11) + (whileOn ? QStringLiteral(" (no dark-mode colours while on)") : QStringLiteral(" (dark-mode colours)")));
        }
        // S7 (2.7.4, S2): a font change posts a step to the layout (not in Classic: no step there), and the one pending at
        // the unload was dropped with the layout (it ran neither during the unload nor after it), and the child lived on.
        static const QRegularExpression pending(QStringLiteral("^reload261: zoomlayout-pending S7 layout (\\d+) posted by a font change (\\d+), "
                                                               "during the unload (\\d+), after it (\\d+)"),
                                                QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   p = pending.match(out);
        QVERIFY2(p.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(p.captured(0).mid(11)));
        if (!classic)
            QVERIFY2(p.captured(1).toInt() == 1 && p.captured(2).toInt() >= 1, qPrintable(p.captured(0)));
        QVERIFY2(p.captured(3).toInt() == 0 && p.captured(4).toInt() == 0, qPrintable(p.captured(0)));
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
        QCOMPARE(checked.join(QLatin1Char(' ')), dark ? QStringLiteral("S5 S5off S6unload") : QStringLiteral("S5"));
        QCOMPARE(steps.join(QLatin1Char(' ')), dark ? QStringLiteral("S0 S1 S2 S3show S3 S4 S5 S5off S6") : QStringLiteral("S0 S1 S2 S3show S3 S4 S5 S6"));
    }

    void zoomLayoutChild()
    {
        if (qEnvironmentVariableIsEmpty("TSMEDIA_ZOOMLAYOUT_CHILD"))
            QSKIP("runs as a child of zoomLayoutFollowsTheFont");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        QCoreApplication::setAttribute(Qt::AA_Use96Dpi, false); // TeamSpeak's DPI (zoomShowChild)
        const auto use96  = qScopeGuard([] { QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true); });
        const int  layout = qBound(0, qEnvironmentVariableIntValue("TSMEDIA_ZOOMLAYOUT_LAYOUT"), 2);
        const bool atEnd  = qEnvironmentVariableIntValue("TSMEDIA_ZOOMLAYOUT_END") == 1;
        const bool dark   = qEnvironmentVariableIntValue("TSMEDIA_ZOOMLAYOUT_DARK") == 1;

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
        {
            QFile ini(dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=false\nmakeLinkPreviews=false\nsharePresence=false\nchatBubbles=true\n");
            ini.write("chatLayout=" + QByteArray::number(layout) + "\n");
            ini.write(dark ? "darkMode=true\n" : "darkMode=false\n");
        }
        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        QFont font(QStringLiteral("MS Shell Dlg 2")); // TeamSpeak's chat font
        font.setPointSizeF(8.25);
        const tschat::Skin s       = tschat::lightSkin();
        auto*              server  = new QTextBrowser;
        auto*              channel = new QTextBrowser;
        for (QTextBrowser* b : {server, channel}) {
            b->setObjectName(QStringLiteral("ChatTab"));
            b->setOpenLinks(false);
            b->setFont(font);
        }
        for (int i = 1; i <= 30; ++i)
            server->append(tschat::messageHtml(s, QStringLiteral("19:%1:00").arg(i, 2, 10, QLatin1Char('0')), 17, kAlice, QStringLiteral("Alice"),
                                               QStringLiteral("server %1 a line in the server chat").arg(i)));
        for (int i = 0; i < 160; ++i) {
            const QString time = QStringLiteral("20:%1:%2").arg((i / 60) % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
            if (i % 10 == 0)
                channel->append(tschat::systemHtml(s, time, QStringLiteral("\"Bob\" connected")));
            else
                channel->append(tschat::messageHtml(s, time, 17 + (i % 3), i % 2 ? kAlice : kBob, i % 2 ? QStringLiteral("Alice") : QStringLiteral("Bob"),
                                                    QStringLiteral("p%1 a message of the channel chat that is long enough to wrap now and then at this "
                                                                   "width, with a few more words in it")
                                                        .arg(i)));
        }
        tabs->addTab(server, QStringLiteral("Fake server"));
        tabs->addTab(channel, QStringLiteral("Fake channel"));
        column->addWidget(tabs);
        window.setCentralWidget(central);
        window.resize(1000, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        tabs->setCurrentIndex(1);
        QTest::qWait(50);

        PluginDll plugin;
        m_logFrom = fakets3::logLines().size();
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settle(channel, 20000);
        QScrollBar* bar = channel->verticalScrollBar();
        bar->setValue(atEnd ? bar->maximum() : bar->maximum() / 2 + 7); // a reader inside a wrapped message
        settle(channel, 8000);
        const chatscroll::ReaderPlace reader = chatscroll::captureReader(channel);
        const qreal                   y0     = chatscroll::readerY(channel->document(), reader) - bar->value();
        const auto                    wheel  = [](QTextBrowser* b, int delta) {
            QWidget*      vp = b->viewport();
            const QPointF pos(vp->width() / 2.0, vp->height() / 2.0);
            QWheelEvent   e(pos, QPointF(vp->mapToGlobal(pos.toPoint())), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(vp, &e);
        };
        // Every styled message row (only those on screen: screenOnly) against the tokens of the chat's current font.
        const auto measure = [&](const char* step, bool screenOnly) {
            QTextDocument*          doc  = channel->document();
            const layoutart::Tokens t    = layoutart::tokensFor(layout == 2 ? layoutart::Mode::Compact : layoutart::Mode::Cozy, doc->defaultFont(), false, layout == 1);
            int                     from = 0;
            int                     to   = doc->blockCount() - 1;
            if (screenOnly) {
                QAbstractTextDocumentLayout* l      = doc->documentLayout();
                const int                    top    = l->hitTest(QPointF(0, bar->value()), Qt::FuzzyHit);
                const int                    bottom = l->hitTest(QPointF(channel->viewport()->width(), bar->value() + channel->viewport()->height()), Qt::FuzzyHit);
                from                                = top < 0 ? 0 : doc->findBlock(top).blockNumber();
                to                                  = bottom < 0 ? to : doc->findBlock(bottom).blockNumber();
            }
            int                styled = 0, rows = 0, off = 0;
            QMap<QString, int> lm, lh;
            for (QTextBlock b = doc->findBlockByNumber(from); b.isValid() && b.blockNumber() <= to; b = b.next()) {
                const QTextBlockFormat f = b.blockFormat();
                if (!f.hasProperty(layoutformat::kKind))
                    continue;
                ++styled;
                if (f.intProperty(layoutformat::kKind) != layoutformat::RowMessage)
                    continue;
                ++rows;
                const bool ok = layout == 2 ? qRound(f.leftMargin()) == t.L
                                            : qRound(f.leftMargin()) == t.G && f.lineHeightType() == QTextBlockFormat::MinimumHeight && qRound(f.lineHeight()) == t.lineMin;
                off += ok ? 0 : 1;
                ++lm[QString::number(f.leftMargin())];
                ++lh[QStringLiteral("%1/%2").arg(f.lineHeight()).arg(f.lineHeightType())];
            }
            QStringList a, c;
            for (auto it = lm.cbegin(); it != lm.cend(); ++it)
                a << QStringLiteral("%1:%2").arg(it.key()).arg(it.value());
            for (auto it = lh.cbegin(); it != lh.cend(); ++it)
                c << QStringLiteral("%1:%2").arg(it.key()).arg(it.value());
            report(QStringLiteral("zoomlayout %1 font %2 px tokens G %3 L %4 line %5 styled %6 rows %7 off %8 left {%9} line {%10} dy %11 gap %12")
                       .arg(QString::fromLatin1(step))
                       .arg(QFontInfo(doc->defaultFont()).pixelSize())
                       .arg(t.G)
                       .arg(t.L)
                       .arg(t.lineMin)
                       .arg(styled)
                       .arg(rows)
                       .arg(off)
                       .arg(a.join(QLatin1Char(' ')))
                       .arg(c.join(QLatin1Char(' ')))
                       .arg(chatscroll::readerY(doc, reader) - bar->value() - y0, 0, 'f', 1)
                       .arg(trueMaximum(channel) - bar->value()));
        };
        measure("S0", false);
        wheel(channel, 120); // S1: zoom +1 in the shown chat
        settle(channel, 8000);
        measure("S1", false);
        wheel(channel, -120); // S2: and back
        settle(channel, 8000);
        measure("S2", false);
        tabs->setCurrentIndex(0); // S3: +1 in the server chat while the channel chat is hidden, then its Show
        settle(server, 3000);
        wheel(server, 120);
        settle(server, 3000);
        tabs->setCurrentIndex(1); // no event-loop turn before the measure (what the first paint shows)
        measure("S3show", true);
        settle(channel, 8000);
        measure("S3", false);
        wheel(channel, -120); // S4: back in the shown chat (the live N3)
        settle(channel, 8000);
        measure("S4", false);
        channel->append(tschat::messageHtml(s, QStringLiteral("21:00:00"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("a new message after the zoom")));
        settle(channel, 8000); // S5: a new line
        measure("S5", false);
        // Merge check: the fragments, block character formats (separators) and chat cursor formats of both chats that hold
        // one of dark mode's colours, or a colour as light as those (its lines' colours made readable on its chat), which
        // no line of this light skin has; in the chats as shown and as TeamSpeak has them under the layout (each format as
        // the layout gives it back, and the runs of TeamSpeak's text its pictures keep). Read only: nothing is edited.
        const auto darkLeft = [&](const char* step) {
            const darkpalette::Tokens& k = darkpalette::tokens();
            QSet<QRgb>                 tokens;
            for (const QColor& c : {k.text, k.secondary, k.muted, k.link, k.success, k.error, k.warning, k.accent})
                tokens.insert(c.rgba());
            const auto isDark = [&](const QTextCharFormat& f) {
                const QColor c = f.foreground().color();
                return f.hasProperty(QTextFormat::ForegroundBrush) && (tokens.contains(c.rgba()) || c.lightness() > 140);
            };
            int                fragments = 0, separators = 0, cursors = 0;
            QMap<QString, int> colours; // the text colours of TeamSpeak's lines, for the report
            const auto         teamSpeaks = [&](const QTextCharFormat& shown) {
                QVector<QTextCharFormat> out;
                if (layoutformat::isObject(shown)) {
                    const QVariantList runs = layoutformat::runsOf(shown);
                    for (int i = 1; i < runs.size(); i += 2)
                        out << qvariant_cast<QTextFormat>(runs.at(i)).toCharFormat();
                } else {
                    out << layoutdoc::originalCharFormat(shown);
                }
                return out;
            };
            for (QTextBrowser* b : {server, channel}) {
                for (QTextBlock block = b->document()->begin(); block.isValid(); block = block.next()) {
                    separators += isDark(block.charFormat()) || isDark(layoutdoc::originalCharFormat(block.charFormat())) ? 1 : 0;
                    for (auto it = block.begin(); !it.atEnd(); ++it) {
                        if (!it.fragment().isValid())
                            continue;
                        const QTextCharFormat shown = it.fragment().charFormat();
                        fragments += isDark(shown) ? 1 : 0;
                        for (const QTextCharFormat& f : teamSpeaks(shown)) {
                            fragments += isDark(f) ? 1 : 0;
                            if (f.hasProperty(QTextFormat::ForegroundBrush))
                                ++colours[f.foreground().color().name()];
                        }
                    }
                }
                cursors += isDark(b->currentCharFormat()) ? 1 : 0;
            }
            QStringList seen;
            for (auto it = colours.cbegin(); it != colours.cend(); ++it)
                seen << QStringLiteral("%1:%2").arg(it.key()).arg(it.value());
            report(QStringLiteral("zoomlayout-darkleft %1 %2 (fragments %3, separators %4, cursors %5) colours %6")
                       .arg(QString::fromLatin1(step))
                       .arg(fragments + separators + cursors)
                       .arg(fragments)
                       .arg(separators)
                       .arg(cursors)
                       .arg(seen.join(QLatin1Char(' '))));
        };
        darkLeft("S5"); // the control: none in the light rows, dark mode's while it is on
        if (dark) {
            // S5off: dark mode off with the zoom's restyles done in its look; a click in a line printed while dark first (the
            // chat cursor takes its format), and a plain line appended after off (it takes the cursor's).
            QTextCursor click(channel->document()->lastBlock());
            click.movePosition(QTextCursor::StartOfBlock);
            click.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, qMin(12, click.block().length() - 1));
            const int keep = bar->value();
            channel->setTextCursor(click);
            bar->setValue(keep); // the reader stays (setTextCursor shows the cursor)
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 11, 0);
            settle(channel, 8000);
            channel->append(QStringLiteral("a plain line after dark mode went off"));
            settle(channel, 8000);
            measure("S5off", false);
            darkLeft("S5off");
            plugin.menuItem(fakets3::kConnection, PLUGIN_MENU_TYPE_GLOBAL, 10, 0); // on again for the Reload
            settle(channel, 8000);
        }
        plugin.unload(); // S6: Options > Addons > Reload
        m_plugin = nullptr;
        if (dark)
            darkLeft("S6unload");
        QTest::qWait(300);
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settle(channel, 20000);
        measure("S6", false);
        printPluginSteps();
        // S7 (2.7.4, S2): the unload with the step a new chat font posts (QEvent::FontChange, ChatLayout::eventFilter) still
        // pending. It is posted to the chat layout itself, which the shutdown deletes, so Qt drops it with it: it must never
        // run after the DLL is gone (the child would crash). A font change and the event loop (the layout is found by the
        // calls it gets); one whose posted calls to the layout are delivered at once (the step is there); one with the
        // unload right after it; then TeamSpeak's event loop.
        LayoutCallSpy spy;
        qApp->installEventFilter(&spy);
        const QFont base   = channel->font();
        QFont       bigger = base;
        bigger.setPointSizeF(base.pointSizeF() + 1);
        channel->setFont(bigger);
        settle(channel, 8000);
        spy.find   = false;
        int posted = 0;
        if (spy.layout) {
            const int before = spy.delivered;
            channel->setFont(base);
            QCoreApplication::sendPostedEvents(spy.layout, QEvent::MetaCall);
            posted = spy.delivered - before;
        }
        channel->setFont(bigger); // no event-loop turn from here to the unload
        const int beforeUnload = spy.delivered;
        plugin.unload();
        m_plugin               = nullptr;
        const int duringUnload = spy.delivered - beforeUnload;
        QTest::qWait(300);
        const int afterUnload = spy.delivered - beforeUnload - duringUnload;
        qApp->removeEventFilter(&spy);
        report(QStringLiteral("zoomlayout-pending S7 layout %1 posted by a font change %2, during the unload %3, after it %4")
                   .arg(spy.layout ? 1 : 0)
                   .arg(posted)
                   .arg(duringUnload)
                   .arg(afterUnload));
    }

    // ---- review fix (2.7.3, M1): a zoom gesture in a long chat holds the GUI thread only briefly ----------------------------
    // The real plugin, a 1000-line Cozy or Compact channel chat, the reader in the middle or near the end. Gestures of 8 Ctrl
    // + wheel notches 40 ms apart, alternately in and out; a 1 ms timer measures the longest turn of the event loop during
    // each gesture and in the 2.5 s after it (the zoom's scale pass settles 150 ms after the last notch, the rows are
    // restyled for the new font in steps), and so does the longest call that held the GUI thread (a notch's wheel event or
    // one processEvents call: a chain of 0 ms work that only starves the timer doesn't count there). With the reader in the
    // middle the first 2.7.3 code laid the whole chat out in one turn after each gesture (38-350 ms here; 2.7.2: 7-28 ms)
    // and held it up to 358 ms in one turn during one. Review fix (integration review): the next one held it up to 190 ms
    // in Compact while zooming in, at one gesture of three (2.7.2: 31-37 ms), which "the best of the 3 gestures under 120 ms"
    // let pass. Now every gesture is bounded: at most 100 ms held during it (2.7.2 held 94-98 ms zooming out of the Cozy
    // chat, and 330-360 ms with the reader near the end) and the median of each direction's 3 gestures at most 75 ms (this
    // code, quiet machine: 44-70 ms; 2.7.2 in Compact 36-62), at most 40 ms held after it; the reader's line stays where it
    // was after each zoom in and back out.
    static QVector<QPair<QByteArray, QByteArray>> zoomTurnRows()
    {
        return {{"Cozy, reader in the middle", "TSMEDIA_ZOOMPERF_LAYOUT=1"},
                {"Compact, reader in the middle", "TSMEDIA_ZOOMPERF_LAYOUT=2"},
                {"Cozy, reader near the end", "TSMEDIA_ZOOMPERF_LAYOUT=1;TSMEDIA_ZOOMPERF_NEAR=1"}};
    }
    void zoomTurnsStayShort_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const auto& row : zoomTurnRows())
            QTest::newRow(row.first.constData()) << row.second;
    }
    void zoomTurnsStayShort()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_ZOOMPERF_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runNamedChild("zoomPerfChild", "TSMEDIA_ZOOMPERF_CHILD=1;" + extra, &out));
        static const QRegularExpression re(QStringLiteral("^reload261: zoomperf round (\\d+) dir (-?\\d+) during (\\d+) after (\\d+) .*dy (-?[0-9.]+) held (\\d+) (\\d+)"),
                                           QRegularExpression::MultilineOption);
        QVector<int> heldDuring[2]; // [0]: in, [1]: out
        int          rounds = 0;
        auto         it     = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            qInfo("%s", qPrintable(m.captured(0).mid(11)));
            const int d = m.captured(2).toInt() > 0 ? 0 : 1;
            heldDuring[d].append(m.captured(6).toInt());
            ++rounds;
            QVERIFY2(qAbs(m.captured(5).toDouble()) <= 2, qPrintable(m.captured(0) + QStringLiteral(" (the reader's line moved)")));
            QVERIFY2(m.captured(6).toInt() <= 100, qPrintable(m.captured(0) + QStringLiteral(" (held the GUI thread too long during the gesture)")));
            QVERIFY2(m.captured(7).toInt() <= 40, qPrintable(m.captured(0) + QStringLiteral(" (held the GUI thread too long after the gesture)")));
        }
        QCOMPARE(rounds, 6);
        for (int d = 0; d < 2; ++d) {
            std::sort(heldDuring[d].begin(), heldDuring[d].end());
            QCOMPARE(heldDuring[d].size(), 3);
            QVERIFY2(heldDuring[d].at(1) <= 75, qPrintable(QStringLiteral("zoom %1: the median of the longest held calls during a gesture: %2 ms")
                                                                  .arg(QLatin1String(d == 0 ? "in" : "out"))
                                                                  .arg(heldDuring[d].at(1))));
        }
    }
    // Live test fix (2.7.3, M1): a gesture of 8 Ctrl + wheel notches at the end of a long Cozy or Compact chat left the chat
    // scrolled up from its end (live, 100 %: 700-1900 px above it after zooming in, still above it after zooming back; a single
    // notch kept the end; 2.7.2 kept it), and a chat not at its end gets no new line in view. The end stays the end after every
    // gesture, in and out.
    void zoomGestureAtTheEnd_data()
    {
        QTest::addColumn<QByteArray>("extra");
        QTest::newRow("Cozy") << QByteArray("TSMEDIA_ZOOMPERF_LAYOUT=1");
        QTest::newRow("Compact") << QByteArray("TSMEDIA_ZOOMPERF_LAYOUT=2");
    }
    void zoomGestureAtTheEnd()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_ZOOMPERF_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runNamedChild("zoomPerfChild", "TSMEDIA_ZOOMPERF_CHILD=1;TSMEDIA_ZOOMPERF_END=1;" + extra, &out));
        static const QRegularExpression re(QStringLiteral("^reload261: (zoomperf round \\d+ dir -?\\d+ .* end (\\d))"), QRegularExpression::MultilineOption);
        int  rounds = 0;
        auto it     = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            qInfo("%s", qPrintable(m.captured(1)));
            QVERIFY2(m.captured(2) == QLatin1String("1"), qPrintable(m.captured(1) + QStringLiteral(" (the chat left its end)")));
            ++rounds;
        }
        QCOMPARE(rounds, 6);
    }
    void zoomPerfChild()
    {
        if (qEnvironmentVariableIsEmpty("TSMEDIA_ZOOMPERF_CHILD"))
            QSKIP("runs as a child of zoomTurnsStayShort");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        QCoreApplication::setAttribute(Qt::AA_Use96Dpi, false); // TeamSpeak's DPI (zoomShowChild)
        const auto use96  = qScopeGuard([] { QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true); });
        const int  layout = qBound(0, qEnvironmentVariableIntValue("TSMEDIA_ZOOMPERF_LAYOUT"), 2);
        const bool near   = qEnvironmentVariableIntValue("TSMEDIA_ZOOMPERF_NEAR") == 1;
        const bool atEnd  = qEnvironmentVariableIntValue("TSMEDIA_ZOOMPERF_END") == 1; // zoomGestureAtTheEnd

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
        {
            QFile ini(dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=false\nmakeLinkPreviews=false\nsharePresence=false\nchatBubbles=true\n");
            ini.write("chatLayout=" + QByteArray::number(layout) + "\n");
        }
        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        QFont font(QStringLiteral("MS Shell Dlg 2")); // TeamSpeak's chat font
        font.setPointSizeF(8.25);
        const tschat::Skin s       = tschat::lightSkin();
        auto*              channel = new QTextBrowser;
        channel->setObjectName(QStringLiteral("ChatTab"));
        channel->setOpenLinks(false);
        channel->setFont(font);
        for (int i = 0; i < 1000; ++i) {
            const QString time = QStringLiteral("%1:%2:%3")
                                     .arg(10 + i / 3600, 2, 10, QLatin1Char('0'))
                                     .arg((i / 60) % 60, 2, 10, QLatin1Char('0'))
                                     .arg(i % 60, 2, 10, QLatin1Char('0'));
            if (i % 10 == 0)
                channel->append(tschat::systemHtml(s, time, QStringLiteral("\"Bob\" connected")));
            else
                channel->append(tschat::messageHtml(s, time, 17 + (i % 3), i % 2 ? kAlice : kBob, i % 2 ? QStringLiteral("Alice") : QStringLiteral("Bob"),
                                                    QStringLiteral("p%1 a message of the channel chat that is long enough to wrap now and then at this "
                                                                   "width, with a few more words in it")
                                                        .arg(i)));
        }
        tabs->addTab(channel, QStringLiteral("Fake channel"));
        column->addWidget(tabs);
        window.setCentralWidget(central);
        window.resize(1000, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);

        PluginDll plugin;
        m_logFrom = fakets3::logLines().size();
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settle(channel, 60000);
        QScrollBar* bar = channel->verticalScrollBar();
        bar->setValue(atEnd ? bar->maximum() : near ? bar->maximum() - 3 * channel->viewport()->height() : bar->maximum() / 2 + 7);
        settle(channel, 8000);
        const chatscroll::ReaderPlace reader = chatscroll::captureReader(channel);
        const auto                    lineY  = [&] { return chatscroll::readerY(channel->document(), reader) - bar->value(); };
        const qreal                   y0     = lineY();
        const auto                    wheel  = [](QTextBrowser* b, int delta) {
            QWidget*      vp = b->viewport();
            const QPointF pos(vp->width() / 2.0, vp->height() / 2.0);
            QWheelEvent   e(pos, QPointF(vp->mapToGlobal(pos.toPoint())), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(vp, &e);
        };
        // The longest turn of the event loop: the longest gap between two ticks of a 1 ms timer.
        QElapsedTimer clock;
        clock.start();
        qint64 last = 0, longest = 0;
        QTimer ticker;
        ticker.setTimerType(Qt::PreciseTimer);
        ticker.setInterval(1);
        QObject::connect(&ticker, &QTimer::timeout, &ticker, [&] {
            const qint64 now = clock.elapsed();
            longest          = qMax(longest, now - last);
            last             = now;
        });
        ticker.start();
        // Review fix: and the longest call that held the GUI thread (a wheel notch's event, or one processEvents call of at most
        // 5 ms of events plus the event it was in), which a chain of 0 ms work that starves the timer above doesn't inflate.
        qint64     held   = 0;
        const auto runFor = [&held](int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms) {
                QElapsedTimer call;
                call.start();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                held = qMax(held, call.elapsed());
            }
        };
        for (int round = 0; round < 6; ++round) {
            const int direction = round % 2 == 0 ? 1 : -1;
            runFor(20);
            last    = clock.elapsed();
            longest = 0;
            held    = 0;
            for (int notch = 0; notch < 8; ++notch) {
                QElapsedTimer call;
                call.start();
                wheel(channel, 120 * direction);
                held = qMax(held, call.elapsed());
                runFor(40);
            }
            const qint64 during     = longest;
            const qint64 heldDuring = held;
            last                    = clock.elapsed();
            longest                 = 0;
            held                    = 0;
            runFor(2500);
            const qint64 after     = longest;
            const qint64 heldAfter = held;
            settle(channel, 8000);
            report(QStringLiteral("zoomperf round %1 dir %2 during %3 after %4 font %5 px dy %6 held %7 %8 end %9")
                       .arg(round)
                       .arg(direction)
                       .arg(during)
                       .arg(after)
                       .arg(QFontInfo(channel->document()->defaultFont()).pixelSize())
                       .arg(round % 2 == 1 ? lineY() - y0 : 0.0, 0, 'f', 1)
                       .arg(heldDuring)
                       .arg(heldAfter)
                       .arg(chatscroll::atBottom(channel) ? 1 : 0));
        }
        ticker.stop();
        printPluginSteps();
        plugin.unload();
        m_plugin = nullptr;
        QTest::qWait(100);
    }

    // ---- 2.7.2 (D8): a link that waits for its automatic-load turn shows "Load preview", never the loading ring ----------
    // The real plugin with automatic loading on and the chat proxy pointed at a fake SOCKS5 proxy on 127.0.0.1 that accepts
    // and never answers (nothing leaves this PC: every load that starts stays loading for the whole test). A flood of 8
    // picture links from one sender into the visible chat, then 8 from another while that chat's tab is hidden, shown a
    // few seconds later. At the first paint of the cards (after the plugin drew them, before Qt paints) only a card whose
    // load has started may look other than it looks a second and a half later (one load per sender: the rest wait as
    // "Load preview" click cards all along). 2.7.1 drew every waiting card as the loading ring until a paint's pass 100 ms
    // later started one and turned the others into click cards (live: the ring for 105 s while the chat got no paint).
    // The hidden chat comes to its Show three ways: painted with TS Media before (shown once after the load); shown and
    // painted before TS Media loaded and hidden since (as after Options > Addons > Reload: TS Media never painted it); and
    // a new chat filled while hidden (its first Show, with Qt's own move to the text cursor). The first 2.7.2 code started
    // the shown chat's loads only in the first way (a slow plugin start that painted the chat before its first scan failed
    // the whole suite the same way).
    void autoLinksWaitAsClickCards_data()
    {
        QTest::addColumn<QByteArray>("how");
        QTest::newRow("hidden chat painted with TS Media before") << QByteArray("painted");
        QTest::newRow("hidden chat shown before TS Media loaded") << QByteArray("reload");
        QTest::newRow("a new chat's first Show") << QByteArray("new");
    }
    void autoLinksWaitAsClickCards()
    {
        QFETCH(QByteArray, how);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_LINKFLOOD_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("QT_FONT_DPI"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        env.insert(QStringLiteral("TSMEDIA_LINKFLOOD_CHILD"), QStringLiteral("1"));
        env.insert(QStringLiteral("TSMEDIA_LINKFLOOD_CASE"), QString::fromLatin1(how));
        const QString fonts = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts"));
        if (QDir(fonts).exists() && !env.contains(QStringLiteral("QT_QPA_FONTDIR")))
            env.insert(QStringLiteral("QT_QPA_FONTDIR"), fonts);
        QProcess child;
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QStringLiteral("offscreen"), QStringLiteral("linkFloodChild")});
        QVERIFY(child.waitForStarted());
        const bool finished = child.waitForFinished(120000);
        if (!finished)
            child.kill();
        const QString out = QString::fromUtf8(child.readAll());
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_SHOW"))
            std::fprintf(stdout, "%s\n", out.toUtf8().constData());
        QVERIFY2(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, qPrintable(out.right(3000)));
        static const QRegularExpression re(QStringLiteral("^reload261: flood (\\w+) cards (\\d+) drawn (\\d+) started (\\d+) changed (\\d+) final starts (\\d+)"),
                                           QRegularExpression::MultilineOption);
        int  phases = 0;
        auto it     = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            ++phases;
            qInfo("%s", qPrintable(m.captured(0).mid(11)));
            const int cards = m.captured(2).toInt(), drawn = m.captured(3).toInt(), started = m.captured(4).toInt(), changed = m.captured(5).toInt(),
                      final = m.captured(6).toInt();
            QCOMPARE(cards, 8);
            QCOMPARE(drawn, 8);       // the first paint drew every card
            QCOMPARE(final, started); // one load for that sender all along (the others wait for its turn)
            QVERIFY2(started <= 1, qPrintable(m.captured(0)));
            // Only a started load's card may look different later (its ring turns): no card went from the ring to "Load
            // preview", nor from "Load preview" to the ring.
            QVERIFY2(changed <= started, qPrintable(m.captured(0) + QStringLiteral(" (a card drawn as the ring before its load started)")));
        }
        QVERIFY2(phases == 2, qPrintable(out.right(3000)));
        // The load that could start did, before the first paint (the visible chat: right after the plugin put the cards
        // in; the hidden one: at its Show).
        QVERIFY2(out.contains(QLatin1String("reload261: flood visible cards 8 drawn 8 started 1")), qPrintable(out.right(2000)));
        QVERIFY2(out.contains(QLatin1String("reload261: flood shown cards 8 drawn 8 started 1")), qPrintable(out.right(2000)));
        QVERIFY2(out.contains(QLatin1String("reload261: flood shown hidden starts 0")), qPrintable(out.right(2000))); // nothing for a hidden chat
    }

    void linkFloodChild()
    {
        if (qEnvironmentVariableIsEmpty("TSMEDIA_LINKFLOOD_CHILD"))
            QSKIP("runs as a child of autoLinksWaitAsClickCards");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        proxyfakes::SocksScript script;
        script.silent = true; // accepts and never answers: a load through it stays loading
        proxyfakes::FakeSocks tarpit(script);
        QVERIFY(tarpit.port() != 0);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
        {
            QFile ini(dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=true\nmakeLinkPreviews=false\nsharePresence=false\n");
            ini.write("proxyOn=true\nproxyType=socks5\nproxyHost=127.0.0.1\nproxyPort=" + QByteArray::number(tarpit.port()) + "\n");
        }

        const QByteArray how = qgetenv("TSMEDIA_LINKFLOOD_CASE"); // painted (default), reload, new (autoLinksWaitAsClickCards_data)

        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        const tschat::Skin s        = tschat::lightSkin();
        QTextBrowser*      chats[3] = {nullptr, nullptr, nullptr};
        PaintTap           taps[3];
        const auto         makeChat = [&](int i, const QString& title) {
            chats[i] = new QTextBrowser;
            chats[i]->setObjectName(QStringLiteral("ChatTab"));
            chats[i]->setOpenLinks(false);
            chats[i]->document()->setDefaultFont(font);
            for (int k = 1; k <= 30; ++k)
                chats[i]->append(tschat::messageHtml(s, QStringLiteral("20:%1:00").arg(k, 2, 10, QLatin1Char('0')), 17, kAlice, QStringLiteral("Alice"),
                                                     QStringLiteral("flood %1 a plain line before the links").arg(k)));
            chats[i]->viewport()->installEventFilter(&taps[i]); // before the plugin's filters: runs after them
            tabs->addTab(chats[i], title);
        };
        makeChat(0, QStringLiteral("Fake channel"));
        makeChat(1, QStringLiteral("Server"));
        column->addWidget(tabs);
        window.setCentralWidget(central);
        window.resize(1000, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        if (how == "reload") { // shown and painted before the plugin loads, hidden when it does (TS Media never paints it)
            tabs->setCurrentIndex(1);
            QTest::qWait(300);
            tabs->setCurrentIndex(0);
            QTest::qWait(300);
        }

        PluginDll plugin;
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        if (how != "reload") {
            tabs->setCurrentIndex(1); // both painted once with the plugin
            pump(chats[1], 500);
            tabs->setCurrentIndex(0);
        }
        pump(chats[0], 500);
        for (QTextBrowser* b : chats) {
            if (b)
                b->verticalScrollBar()->setValue(b->verticalScrollBar()->maximum());
        }
        pump(chats[0], 300);

        // The automatic loads of one site so far. Each phase's links are on a site of their own: a load through the silent
        // proxy ends after about 6 s (the proxy's greeting never comes), and the visible chat's next link then starts in its
        // turn, which is no load of the later phase (the new chat's row, the longest, met it now and then).
        const auto startsOf = [](const QString& host) {
            const QString what = QStringLiteral(" from %1 (automatic)").arg(host);
            int           n    = 0;
            for (const QString& line : fakets3::logLines())
                n += line.contains(what) ? 1 : 0;
            return n;
        };
        // One phase: the first paint that draws all 8 new cards (what it drew, the loads started by then), then the
        // same cards 1.5 s later (a paint of their own).
        const auto phase = [&](const char* name, int chat, int clid, const QString& uid, const QString& nick, bool showLater) {
            const QString                host   = qstrcmp(name, "visible") == 0 ? QStringLiteral("cdn.example.com") : QStringLiteral("cdn.example.net");
            const auto                   starts = [&] { return startsOf(host); };
            const int                    before = starts();
            QMap<QString, QString>       first;
            int                          startedAtFirst = -1;
            const QMap<QString, QString> old            = linkCards(chats[chat]);
            taps[chat].onPaint                          = [&] {
                if (startedAtFirst >= 0)
                    return;
                QMap<QString, QString> now = linkCards(chats[chat]);
                for (const QString& key : old.keys())
                    now.remove(key);
                if (now.size() < 8)
                    return;
                for (const QString& v : qAsConst(now)) {
                    if (v == QLatin1String("blank"))
                        return;
                }
                first          = now;
                startedAtFirst = starts() - before;
            };
            for (int k = 1; k <= 8; ++k)
                chats[chat]->append(tschat::messageHtml(s, QStringLiteral("21:00:%1").arg(k, 2, 10, QLatin1Char('0')), clid, uid, nick,
                                                        QStringLiteral("flood %1 %2 ").arg(QLatin1String(name)).arg(k)
                                                            + tschat::linkHtml(s, QStringLiteral("https://%1/flood/%2-%3.png").arg(host, QLatin1String(name)).arg(k))));
            if (showLater) {
                pump(chats[chat], 2500); // the hidden chat gets its cards (no paint, no load)
                report(QStringLiteral("flood %1 hidden starts %2").arg(QLatin1String(name)).arg(starts() - before));
                tabs->setCurrentIndex(chat);
            }
            QElapsedTimer t;
            t.start();
            while (startedAtFirst < 0 && t.elapsed() < 10000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            taps[chat].onPaint = nullptr;
            pump(chats[chat], 1500);
            chats[chat]->viewport()->repaint(); // the cards drawn again for this paint
            const QMap<QString, QString> later   = linkCards(chats[chat]);
            int                          changed = 0, drawn = 0;
            for (auto it = first.constBegin(); it != first.constEnd(); ++it) {
                drawn += it.value() != QLatin1String("blank") ? 1 : 0;
                if (later.value(it.key()) != it.value())
                    ++changed;
                report(QStringLiteral("flood %1 card %2 first %3 later %4").arg(QLatin1String(name), it.key().right(6), it.value(), later.value(it.key())));
            }
            report(QStringLiteral("flood %1 cards %2 drawn %3 started %4 changed %5 final starts %6")
                       .arg(QLatin1String(name))
                       .arg(first.size())
                       .arg(drawn)
                       .arg(startedAtFirst)
                       .arg(changed)
                       .arg(starts() - before));
        };
        phase("visible", 0, 1, QStringLiteral("serveradmin"), QStringLiteral("serveradmin"), false);
        int shownChat = 1;
        if (how == "new") { // a third chat added and filled while hidden: its Show below is its first
            makeChat(2, QStringLiteral("Bob"));
            pump(chats[0], 2000); // found and styled by the plugin while hidden
            shownChat = 2;
        }
        phase("shown", shownChat, 19, kBob, QStringLiteral("Bob"), true);
        plugin.unload();
        m_plugin = nullptr;
        QTest::qWait(100);
    }

    // ---- 2.7.1 (issue 3): a chat filled while it was hidden shows its end ----------------------------------------------
    // The real plugin loaded under a shown server chat; then a new chat tab is added hidden and filled the way TeamSpeak
    // fills a new connection's channel chat (QTextEdit::append: its history, cards shown from cache/links, then this
    // session's events), the plugin styles it while hidden, and the reader opens it. It must show its end at once (before
    // its first paint) and after everything settled, and keep it across a switch away and back. The chat ends in a run of
    // events the layout collapses (2.6.0 and 2.7.0 showed the chat's top: Qt's first-show move goes to TeamSpeak's cursor,
    // in a hidden row), in an album's later messages (hidden too), or in a card taller than the view; in Cozy, Compact and
    // TeamSpeak classic, with dark mode on and off.
    void newChatShowsItsEnd_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const char* layout : {"1", "2", "0"}) {
            for (const char* dark : {"1", "0"}) {
                const QString name = QStringLiteral("%1, dark mode %2, a run of events at the end")
                                         .arg(QLatin1String(layout[0] == '1' ? "cozy" : layout[0] == '2' ? "compact" : "classic"), QLatin1String(dark[0] == '1' ? "on" : "off"));
                QTest::newRow(qPrintable(name)) << (QByteArray("TSMEDIA_NEWCHAT_LAYOUT=") + layout + ";TSMEDIA_NEWCHAT_DARK=" + dark + ";TSMEDIA_NEWCHAT_ENDING=run");
            }
        }
        QTest::newRow("cozy, dark mode on, an album's later messages at the end") << QByteArray("TSMEDIA_NEWCHAT_LAYOUT=1;TSMEDIA_NEWCHAT_DARK=1;TSMEDIA_NEWCHAT_ENDING=album");
        QTest::newRow("cozy, dark mode on, a card taller than the view at the end")
            << QByteArray("TSMEDIA_NEWCHAT_LAYOUT=1;TSMEDIA_NEWCHAT_DARK=1;TSMEDIA_NEWCHAT_ENDING=card;TSMEDIA_NEWCHAT_HEIGHT=330");
    }
    void newChatShowsItsEnd() { newChat(); }

    // The same at QT_FONT_DPI=192 and QT_SCALE_FACTOR=2 (a function of its own: QtTest's watchdog counts all rows together).
    void newChatShowsItsEndScaled_data()
    {
        QTest::addColumn<QByteArray>("extra");
        for (const char* scaling : {"QT_FONT_DPI=192", "QT_SCALE_FACTOR=2"}) {
            for (const char* dark : {"1", "0"})
                QTest::newRow(qPrintable(QStringLiteral("cozy, dark mode %1, a run of events at the end, %2").arg(QLatin1String(dark[0] == '1' ? "on" : "off"), QLatin1String(scaling))))
                    << (QByteArray(scaling) + ";TSMEDIA_NEWCHAT_LAYOUT=1;TSMEDIA_NEWCHAT_DARK=" + dark + ";TSMEDIA_NEWCHAT_ENDING=run");
            QTest::newRow(qPrintable(QStringLiteral("compact, dark mode on, an album's later messages at the end, %1").arg(QLatin1String(scaling))))
                << (QByteArray(scaling) + ";TSMEDIA_NEWCHAT_LAYOUT=2;TSMEDIA_NEWCHAT_DARK=1;TSMEDIA_NEWCHAT_ENDING=album");
        }
    }
    void newChatShowsItsEndScaled() { newChat(); }

    // Review fix: the chat zoom changed (Ctrl + wheel in the shown server chat) while the new chat was hidden, and the chat
    // area narrower than the hidden chat's layout (640 px, a widget made without a parent). At its Show the chat grows
    // taller under the same scroll value, then gets the saved zoom before its first paint (ChatZoom), which starts a scale
    // pass that reads the place there, short of the end; ShowPlace puts the end back, and that pass must keep the end at
    // its 150 ms settle (2.7.0 and the first 2.7.1 fix showed older messages).
    void newChatShowsItsEndZoomed_data()
    {
        QTest::addColumn<QByteArray>("extra");
        QTest::newRow("cozy, dark mode on, a run of events at the end, zoomed in while hidden, 420 px")
            << QByteArray("TSMEDIA_NEWCHAT_LAYOUT=1;TSMEDIA_NEWCHAT_DARK=1;TSMEDIA_NEWCHAT_ENDING=run;TSMEDIA_NEWCHAT_ZOOM=1;TSMEDIA_NEWCHAT_WIDTH=420");
        QTest::newRow("classic, dark mode off, a plain end, zoomed in while hidden, 420 px")
            << QByteArray("TSMEDIA_NEWCHAT_LAYOUT=0;TSMEDIA_NEWCHAT_DARK=0;TSMEDIA_NEWCHAT_ENDING=run;TSMEDIA_NEWCHAT_ZOOM=1;TSMEDIA_NEWCHAT_WIDTH=420");
        QTest::newRow("compact, dark mode off, an album's later messages at the end, zoomed out while hidden, 420 px")
            << QByteArray("TSMEDIA_NEWCHAT_LAYOUT=2;TSMEDIA_NEWCHAT_DARK=0;TSMEDIA_NEWCHAT_ENDING=album;TSMEDIA_NEWCHAT_ZOOM=-1;TSMEDIA_NEWCHAT_WIDTH=420");
        QTest::newRow("cozy, dark mode on, a run of events at the end, zoomed in while hidden, 520 px, QT_FONT_DPI=144")
            << QByteArray("QT_FONT_DPI=144;TSMEDIA_NEWCHAT_LAYOUT=1;TSMEDIA_NEWCHAT_DARK=1;TSMEDIA_NEWCHAT_ENDING=run;TSMEDIA_NEWCHAT_ZOOM=1;TSMEDIA_NEWCHAT_WIDTH=520");
    }
    void newChatShowsItsEndZoomed() { newChat(); }

    // The child: TSMEDIA_NEWCHAT_LAYOUT (0 classic, 1 Cozy, 2 Compact), _DARK (1: dark mode on), _ENDING (run, album,
    // card), _HEIGHT (the window's height: a short chat for the card), _WIDTH (the window's width), _ZOOM (Ctrl + wheel
    // notches in the shown server chat while the new chat is hidden).
    void newChatChild()
    {
        if (qEnvironmentVariableIsEmpty("TSMEDIA_NEWCHAT_CHILD"))
            QSKIP("runs as a child of newChatShowsItsEnd");
        const QString dll = pluginPath();
        QVERIFY2(!dll.isEmpty() && QFile::exists(dll), qPrintable(dll));
        const int        layout = qBound(0, qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_LAYOUT"), 2);
        const bool       dark   = qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_DARK") == 1;
        const QByteArray ending = qgetenv("TSMEDIA_NEWCHAT_ENDING");
        const int        height = qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_HEIGHT") > 0 ? qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_HEIGHT") : 760;
        const int        messages = 60;

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        fakets3::setConnected(true);
        fakets3::setOwnUid(kMe);
        QDir().mkpath(dir.path() + QStringLiteral("/plugins/tsmedia"));
        {
            QFile ini(dir.path() + QStringLiteral("/plugins/tsmedia/settings.ini"));
            QVERIFY(ini.open(QIODevice::WriteOnly));
            ini.write("[General]\nvideoVolume=0\nvideosStartMuted=true\nvoiceSounds=false\nupdateCheck=2\nupdateConsentAsked=2\n"
                      "autoLoadLinkPreviews=false\nmakeLinkPreviews=false\nsharePresence=false\nchatBubbles=true\nchatCollapseEvents=true\n");
            ini.write("chatLayout=" + QByteArray::number(layout) + "\n");
            ini.write(dark ? "darkMode=true\n" : "darkMode=false\n");
        }
        QVERIFY(seedCards(dir.path() + QStringLiteral("/plugins/tsmedia/cache/links"), messages + 1));

        QMainWindow window;
        window.setObjectName(QStringLiteral("MainWindow"));
        auto* central = new QWidget(&window);
        auto* column  = new QVBoxLayout(central);
        auto* tabs    = new ChatTabWidget(central);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9.0);
        const tschat::Skin s     = tschat::darkSkin();
        const auto         icons = [](QTextDocument* doc) {
            for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
                QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
                icon.fill(QColor(0x43, 0x8b, 0xe8));
                doc->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
            }
        };
        auto* server = new QTextBrowser;
        server->setObjectName(QStringLiteral("ChatTab"));
        server->setOpenLinks(false);
        server->document()->setDefaultFont(font);
        icons(server->document());
        tabs->addTab(server, QStringLiteral("Fake server"));
        column->addWidget(tabs);
        window.setCentralWidget(central);
        window.resize(qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_WIDTH") > 0 ? qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_WIDTH") : 1000, height);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        server->append(tschat::statusHtml(s, QStringLiteral("20:00:00"), QStringLiteral("Trying to connect to server")));
        server->append(tschat::statusHtml(s, QStringLiteral("20:00:01"), QStringLiteral("Connected to Server")));

        PluginDll plugin;
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settle(server, 30000);

        // The new connection's channel chat, as TeamSpeak makes it: a widget of its own (640 x 480 until it is shown),
        // added as a hidden tab and filled in the same turn, each line with QTextEdit::append.
        auto* fresh = new QTextBrowser;
        fresh->setObjectName(QStringLiteral("ChatTab"));
        fresh->setOpenLinks(false);
        fresh->document()->setDefaultFont(font);
        icons(fresh->document());
        tabs->addTab(fresh, QStringLiteral("Fake channel"));
        fresh->append(tschat::historyHtml(s, QStringLiteral("2026-11-14 20:10:00")));
        int seed = 1;
        for (int i = 1; i <= messages; ++i) {
            const QString time = QStringLiteral("20:%1:%2").arg(10 + i / 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
            const bool    bob  = i % 2 == 0;
            QString       body = i % 5 == 0 ? QStringLiteral("newchat %1 read this ").arg(i) + tschat::linkHtml(s, cardUrl(i))
                                            : QStringLiteral("newchat %1 a message of the history, long enough to take a second line in a chat of ordinary "
                                                             "width now and then")
                                                  .arg(i);
            fresh->append(tschat::messageHtml(s, time, bob ? 19 : 17, bob ? kBob : kAlice, bob ? QStringLiteral("Bob") : QStringLiteral("Alice"), body));
        }
        const auto talk = [&](const char* time, int id, const char* channel) {
            fresh->append(tschat::systemHtml(s, QString::fromLatin1(time),
                                             QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, id, QString::fromLatin1(channel))));
        };
        if (ending == "album") { // an album sent as three messages: the later two fold under the first
            const quint32 id = 0x6e5d4c00u;
            for (int k = 1; k <= 3; ++k) {
                const MediaLink link = mediaLink(seed++, 640, 480, id, k, 3);
                fresh->append(tschat::messageHtml(s, QStringLiteral("21:00:0%1").arg(k), 19, kBob, QStringLiteral("Bob"), tschat::linkHtml(s, link.toUrl(), link.fileName) + note()));
            }
        } else if (ending == "card") { // the last message's card from cache/links, taller than the short chat
            fresh->append(tschat::messageHtml(s, QStringLiteral("21:00:00"), 17, kAlice, QStringLiteral("Alice"),
                                              QStringLiteral("newchat last ") + tschat::linkHtml(s, cardUrl(messages + 1))));
        } else { // the history's last sessions (channel switches), the end of the history, this session's line
            talk("21:00:00", 1, "Lobby");
            talk("21:05:00", 2, "Games");
            talk("21:09:00", 3, "Music");
            fresh->append(tschat::historyEndHtml(s));
            talk("09:00:01", 1, "Lobby");
        }
        QScrollBar* bar = fresh->verticalScrollBar();
        settle(fresh, 30000);
        // Ctrl + wheel in the shown server chat (the load hold ended in the settle): the visible chats follow 300 ms after
        // the last notch, the hidden one keeps its font until its Show.
        const qreal hiddenPoints = fresh->font().pointSizeF();
        if (const int notches = qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_ZOOM"); notches != 0) {
            const qreal   serverPoints = server->font().pointSizeF();
            QWidget*      vp           = server->viewport();
            const QPointF pos(vp->width() / 2.0, vp->height() / 2.0);
            QWheelEvent   wheel(pos, QPointF(vp->mapToGlobal(pos.toPoint())), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(vp, &wheel);
            pump(server, 1000);
            QVERIFY2(server->font().pointSizeF() != serverPoints, "the server chat was zoomed");
            QCOMPARE(fresh->font().pointSizeF(), hiddenPoints); // not yet: at its Show
            QVERIFY2(fresh->isHidden() || !fresh->isVisible(), "the new chat is still hidden");
        }
        int hiddenRows = 0;
        for (QTextBlock b = fresh->document()->begin(); b.isValid(); b = b.next())
            hiddenRows += b.isVisible() ? 0 : 1;
        report(QStringLiteral("newchat hidden: scroll %1/%2 true max %3, visible %4, blocks %5, hidden rows %6, last block shown %7, cursor at the end %8")
                   .arg(bar->value())
                   .arg(bar->maximum())
                   .arg(trueMaximum(fresh))
                   .arg(fresh->isVisible() ? 1 : 0)
                   .arg(fresh->document()->blockCount())
                   .arg(hiddenRows)
                   .arg(fresh->document()->lastBlock().isVisible() ? 1 : 0)
                   .arg(fresh->textCursor().position() == fresh->document()->characterCount() - 1 ? 1 : 0));

        const auto measure = [&](const char* phase) {
            const Where w = whereOf(fresh);
            report(QStringLiteral("newchat measure %1 gap %2 value %3 max %4 true max %5")
                       .arg(QLatin1String(phase))
                       .arg(trueMaximum(fresh) - w.value)
                       .arg(w.value)
                       .arg(w.maximum)
                       .arg(trueMaximum(fresh)));
        };
        const int hiddenWidth = fresh->viewport()->width();
        tabs->setCurrentIndex(1); // the reader opens it: its first Show
        measure("shown");         // what its first paint shows
        if (qEnvironmentVariableIntValue("TSMEDIA_NEWCHAT_ZOOM") != 0) // the saved zoom came with the Show
            QVERIFY2(fresh->font().pointSizeF() != hiddenPoints, "the new chat got the zoom at its Show");
        report(QStringLiteral("newchat width: hidden %1 px, shown %2 px; font %3 pt hidden, %4 pt shown")
                   .arg(hiddenWidth)
                   .arg(fresh->viewport()->width())
                   .arg(hiddenPoints)
                   .arg(fresh->font().pointSizeF()));
        settle(fresh, 30000);
        measure("settled");
        tabs->setCurrentIndex(0);
        pump(fresh, 300);
        tabs->setCurrentIndex(1);
        pump(fresh, 300);
        measure("again");
        plugin.unload();
        m_plugin = nullptr;
        QTest::qWait(100);
    }

  private:
    void newChat()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_NEWCHAT_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("QT_FONT_DPI"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        env.insert(QStringLiteral("TSMEDIA_NEWCHAT_CHILD"), QStringLiteral("1"));
        for (const QByteArray& pair : extra.split(';')) {
            if (pair.contains('='))
                env.insert(QString::fromLatin1(pair.left(pair.indexOf('='))), QString::fromLatin1(pair.mid(pair.indexOf('=') + 1)));
        }
        const QString fonts = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts"));
        if (QDir(fonts).exists() && !env.contains(QStringLiteral("QT_QPA_FONTDIR")))
            env.insert(QStringLiteral("QT_QPA_FONTDIR"), fonts);
        QProcess child;
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QStringLiteral("offscreen"), QStringLiteral("newChatChild")});
        QVERIFY(child.waitForStarted());
        const bool finished = child.waitForFinished(120000);
        if (!finished)
            child.kill();
        const QString out = QString::fromUtf8(child.readAll());
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_SHOW"))
            std::fprintf(stdout, "%s\n", out.toUtf8().constData());
        QVERIFY2(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, qPrintable(out.right(3000)));
        static const QRegularExpression hiddenRe(QStringLiteral("^reload261: newchat hidden: [^\r\n]*"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   hidden = hiddenRe.match(out);
        QVERIFY2(hidden.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(hidden.captured(0)));
        if (extra.contains("ENDING=run") && !extra.contains("LAYOUT=0")) // the layout collapsed the run: the last row is hidden
            QVERIFY2(hidden.captured(0).contains(QLatin1String("last block shown 0")), qPrintable(hidden.captured(0)));
        if (extra.contains("ENDING=album"))
            QVERIFY2(hidden.captured(0).contains(QLatin1String("last block shown 0")), qPrintable(hidden.captured(0)));
        QVERIFY2(hidden.captured(0).contains(QLatin1String("cursor at the end 1")), qPrintable(hidden.captured(0)));
        static const QRegularExpression widthRe(QStringLiteral("^reload261: newchat width: hidden (\\d+) px, shown (\\d+) px[^\r\n]*"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   width = widthRe.match(out);
        QVERIFY2(width.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(width.captured(0)));
        if (extra.contains("NEWCHAT_ZOOM")) // the review's case: shown narrower than its hidden layout
            QVERIFY2(width.captured(2).toInt() < width.captured(1).toInt(), qPrintable(width.captured(0)));
        static const QRegularExpression re(QStringLiteral("^reload261: newchat measure (\\w+) gap (-?\\d+) value (-?\\d+) max (-?\\d+)"), QRegularExpression::MultilineOption);
        QStringList phases;
        auto        it = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            qInfo("%s", qPrintable(m.captured(0)));
            phases << m.captured(1);
            const int gap = m.captured(2).toInt();
            QVERIFY2(gap >= 0 && gap <= 4, qPrintable(m.captured(0)));
        }
        QCOMPARE(phases, (QStringList{QStringLiteral("shown"), QStringLiteral("settled"), QStringLiteral("again")}));
    }

    void darkDisable()
    {
        QFETCH(QByteArray, extra);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_DARKCARDS_CHILD"))
            QSKIP("inside a child");
        if (pluginPath().isEmpty())
            QSKIP("this test build has no plugin DLL");
        QString out;
        QVERIFY(runCardsChild("disable", extra, &out));
        const CardMap cards = cardsOf(out);
        for (int chat : {1, 2}) {
            const auto& r  = cards["R"][chat];
            const auto& u0 = cards["U0"][chat];
            const auto& u1 = cards["U1"][chat];
            QVERIFY2(r.size() >= 6, qPrintable(out.right(3000)));
            QString diff = sameCards(u0, r); // a Disable with dark mode off changes no picture
            QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat %1, light Disable: ").arg(chat) + diff));
            QVERIFY2(cardsDiffer(r, cards["D"][chat]), qPrintable(QStringLiteral("chat %1: dark mode drew nothing differently").arg(chat)));
            diff = sameCards(u1, u0);
            QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("chat %1, Disable with dark mode on: ").arg(chat) + diff));
        }
        static const QRegularExpression placeRe(QStringLiteral("^reload261: place U1 dy (-?[0-9.]+) hidden gap (-?\\d+) -> (-?\\d+)"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch   m = placeRe.match(out);
        QVERIFY2(m.hasMatch(), qPrintable(out.right(2000)));
        qInfo("%s", qPrintable(m.captured(0)));
        QVERIFY2(qAbs(m.captured(1).toDouble()) <= 2, qPrintable(m.captured(0)));
        if (m.captured(2).toInt() <= 2) // the hidden chat was at its end: it stays there
            QVERIFY2(m.captured(3).toInt() >= 0 && m.captured(3).toInt() <= 2, qPrintable(m.captured(0)));
        QVERIFY2(out.contains(QLatin1String("reload261: sheet back 1")), qPrintable(out.right(2000)));
    }

    // Every non-blank picture in both maps is the same ("" if so; otherwise the first that differs). A picture blank in one
    // of them (the picture budget's stand-in) isn't compared.
    static QString sameCards(const QMap<QString, QString>& got, const QMap<QString, QString>& want)
    {
        int compared = 0;
        for (auto it = want.constBegin(); it != want.constEnd(); ++it) {
            const QString g = got.value(it.key());
            if (g.isEmpty() || g == QLatin1String("blank") || it.value() == QLatin1String("blank"))
                continue;
            ++compared;
            if (g != it.value())
                return QStringLiteral("%1: %2, want %3").arg(it.key(), g, it.value());
        }
        return compared >= 4 ? QString() : QStringLiteral("only %1 pictures to compare").arg(compared);
    }
    static bool cardsDiffer(const QMap<QString, QString>& a, const QMap<QString, QString>& b)
    {
        for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
            if (b.contains(it.key()) && b.value(it.key()) != it.value() && it.value() != QLatin1String("blank") && b.value(it.key()) != QLatin1String("blank"))
                return true;
        }
        return false;
    }
    // 2.7.2: the environment of a child of this executable: NAME=value pairs ("A=1;B=2"), Windows' fonts, no scaling of ours.
    static QProcessEnvironment namedChildEnv(const QByteArray& extra)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("QT_FONT_DPI"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        for (const QByteArray& pair : extra.split(';')) {
            if (pair.contains('='))
                env.insert(QString::fromLatin1(pair.left(pair.indexOf('='))), QString::fromLatin1(pair.mid(pair.indexOf('=') + 1)));
        }
        const QString fonts = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts"));
        if (QDir(fonts).exists() && !env.contains(QStringLiteral("QT_QPA_FONTDIR")))
            env.insert(QStringLiteral("QT_QPA_FONTDIR"), fonts);
        return env;
    }
    static QString namedChildKey(const char* function, const QByteArray& extra)
    {
        return QString::fromLatin1(function) + QLatin1Char('|') + QString::fromLatin1(extra);
    }
    // Review fix (2.7.3): a child's run stays inside QTest's 300 s per row: 10 s to start, 200 s to run, 5 s to end after a
    // kill (and QProcess's own 30 s at its end). A child that ran 240 s or more once took its row past 300 s, and QTest
    // aborted the whole suite with its output lost; one that runs out of time now fails its row, with what it printed so far
    // (the steps it reported) in the log. TSMEDIA_RELOAD261_CHILD_MS: less (a look at how such a row fails).
    static int childMs()
    {
        constexpr int kChildMs = 200000;
        const int     ms       = qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_CHILD_MS");
        return ms > 0 ? qMin(ms, kChildMs) : kChildMs;
    }
    static bool finishChild(QProcess& child, const char* what, QString* out)
    {
        if (!child.waitForStarted(10000)) {
            *out = QStringLiteral("%1: the child did not start").arg(QLatin1String(what));
            return false;
        }
        const bool finished = child.waitForFinished(childMs());
        if (!finished) {
            child.kill();
            child.waitForFinished(5000);
        }
        *out = QString::fromUtf8(child.readAll());
        if (!finished)
            qWarning("%s: the child ran out of its %d s and was killed; what it printed so far:\n%s", what, childMs() / 1000, qPrintable(out->right(8000)));
        return finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
    }
    // 2.7.2: one test function of this executable as a child (namedChildEnv); its output. A row whose child already ran
    // beside the others (runRowsSideBySide) gets that child's output.
    bool runNamedChild(const char* function, const QByteArray& extra, QString* out)
    {
        bool ok = false;
        const auto ran = m_sideBySide.find(namedChildKey(function, extra));
        if (ran != m_sideBySide.end()) {
            ok   = ran->first;
            *out = ran->second;
            m_sideBySide.erase(ran);
        } else {
            QProcess child;
            child.setProcessEnvironment(namedChildEnv(extra));
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QStringLiteral("offscreen"), QString::fromLatin1(function)});
            ok = finishChild(child, function, out);
        }
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_SHOW"))
            std::fprintf(stdout, "%s\n", out->toUtf8().constData());
        if (!ok)
            qWarning("%s", qPrintable(out->right(3000)));
        return ok;
    }
    // Stabilize fix (the suite's time): the children of a data-driven test whose rows are independent and time nothing
    // (darkHiddenChatFirstPaint, zoomAtShowKeepsTheReader: places and pixels; each child waits mostly for its chats to be
    // quiet) run side by side, at most 5 at once (TSMEDIA_RELOAD261_SIDE: another number, 1 one by one): the function's
    // first row starts every row's child, and each row then checks its own child's output exactly as before
    // (runNamedChild). A command line that names rows of the function ("function:row") runs each named row's own child.
    void runRowsSideBySide(const char* function, const char* child, const QVector<QPair<QByteArray, QByteArray>>& rows, const QByteArray& prefix)
    {
        if (m_sideBySideRan.contains(QString::fromLatin1(function)))
            return;
        m_sideBySideRan.insert(QString::fromLatin1(function));
        const QString named = QString::fromLatin1(function) + QLatin1Char(':');
        for (const QString& arg : QCoreApplication::arguments()) {
            if (arg.startsWith(named))
                return;
        }
        const int most = qEnvironmentVariableIsSet("TSMEDIA_RELOAD261_SIDE") ? qMax(1, qEnvironmentVariableIntValue("TSMEDIA_RELOAD261_SIDE")) : 5;
        if (most <= 1)
            return;
        struct Run {
            std::unique_ptr<QProcess> process;
            QByteArray                extra;
            QElapsedTimer             clock;
        };
        std::vector<Run> running;
        int              next = 0;
        while (next < rows.size() || !running.empty()) {
            while (next < rows.size() && static_cast<int>(running.size()) < most) {
                Run run;
                run.extra   = prefix + rows.at(next++).second;
                run.process = std::make_unique<QProcess>();
                run.process->setProcessEnvironment(namedChildEnv(run.extra));
                run.process->setProcessChannelMode(QProcess::MergedChannels);
                run.process->start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QStringLiteral("offscreen"), QString::fromLatin1(child)});
                if (!run.process->waitForStarted()) {
                    m_sideBySide.insert(namedChildKey(child, run.extra), {false, QStringLiteral("the child did not start")});
                    continue;
                }
                run.clock.start();
                running.push_back(std::move(run));
            }
            QTest::qWait(50); // the children's output read as it comes, their ends seen
            for (auto it = running.begin(); it != running.end();) {
                const bool done = it->process->state() == QProcess::NotRunning;
                const bool late = !done && it->clock.elapsed() > childMs();
                if (!done && !late) {
                    ++it;
                    continue;
                }
                if (late) {
                    it->process->kill();
                    it->process->waitForFinished(5000);
                }
                const bool ok = !late && it->process->exitStatus() == QProcess::NormalExit && it->process->exitCode() == 0;
                QString    out = QString::fromUtf8(it->process->readAll());
                if (late)
                    out += QStringLiteral("\n%1: the child ran out of its %2 s and was killed\n").arg(QLatin1String(child)).arg(childMs() / 1000);
                m_sideBySide.insert(namedChildKey(child, it->extra), {ok, out});
                it = running.erase(it);
            }
        }
    }
    bool runCardsChild(const QByteArray& scenario, const QByteArray& extra, QString* out)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("QT_FONT_DPI"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        env.insert(QStringLiteral("TSMEDIA_DARKCARDS_CHILD"), QString::fromLatin1(scenario));
        for (const QByteArray& pair : extra.split(';')) {
            if (pair.contains('='))
                env.insert(QString::fromLatin1(pair.left(pair.indexOf('='))), QString::fromLatin1(pair.mid(pair.indexOf('=') + 1)));
        }
        const QString fonts = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts"));
        if (QDir(fonts).exists() && !env.contains(QStringLiteral("QT_QPA_FONTDIR")))
            env.insert(QStringLiteral("QT_QPA_FONTDIR"), fonts);
        QProcess child;
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QStringLiteral("offscreen"), QStringLiteral("darkCardsChild")});
        const bool ok = finishChild(child, "darkCardsChild", out);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_SHOW"))
            std::fprintf(stdout, "%s\n", out->toUtf8().constData());
        if (!ok) {
            qWarning("%s", qPrintable(out->right(3000)));
            return false;
        }
        return true;
    }

    void report(const QString& line)
    {
        std::fprintf(stdout, "reload261: %s\n", line.toUtf8().constData());
        std::fflush(stdout);
    }
    // A scratch DLL with per-step logging (never committed) writes "[r261] ..." lines to TeamSpeak's log, and a TESTHOOKS
    // build (TSMEDIA_RELOAD_DLL=<v24c's DLL>) its "[test] place: ..." lines (the unload, the load, the hold): shown as
    // they come.
    void printPluginSteps()
    {
        const QStringList lines = fakets3::logLines();
        for (int i = m_logFrom; i < lines.size(); ++i) {
            if (lines.at(i).contains(QLatin1String("[r261]")) || lines.at(i).contains(QLatin1String("[test] place:")))
                report(QStringLiteral("  plugin ") + lines.at(i));
        }
        m_logFrom = lines.size();
    }
    int pendingDownloads() const
    {
        int pending = 0;
        for (const fakets3::Transfer& t : fakets3::downloads())
            pending += !t.halted && !m_delivered.contains(t.id) && fakets3::hasServerFile(t.remotePath) ? 1 : 0;
        return pending;
    }
    // TeamSpeak's file transfers: every download the plugin asked for arrives at once.
    void deliver()
    {
        if (!m_plugin || !m_plugin->transferStatus)
            return;
        for (const fakets3::Transfer& t : fakets3::downloads()) {
            if (t.halted || m_delivered.contains(t.id) || !fakets3::hasServerFile(t.remotePath))
                continue;
            m_delivered.insert(t.id);
            fakets3::deliverServerFile(t.id);
            m_plugin->transferStatus(t.id, ERROR_file_transfer_complete, "", static_cast<uint64>(fakets3::serverFile(t.remotePath).size()), fakets3::kConnection);
        }
    }
    void pump(QTextBrowser*, int ms)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            deliver();
            runDue();
            QTest::qWait(20);
        }
    }
    // The load's timed actions that are due (reloadNormalUse), each once, in their order.
    void runDue()
    {
        while (!m_due.isEmpty() && m_loadClock.isValid()) {
            int next = -1;
            for (int i = 0; i < m_due.size(); ++i) {
                if (m_due.at(i).first <= m_loadClock.elapsed() && (next < 0 || m_due.at(i).first < m_due.at(next).first))
                    next = i;
            }
            if (next < 0)
                return;
            const std::function<void()> action = m_due.takeAt(next).second;
            action();
        }
    }
    // Until nothing changes for 2 s (no download waits, the document and the scroll bar stay).
    void settle(QTextBrowser* b, int maxMs)
    {
        QElapsedTimer t;
        t.start();
        int    revision = -1, value = -1, maximum = -1;
        qint64 since    = 0;
        while (t.elapsed() < maxMs) {
            pump(b, 100);
            const int r = b->document()->revision();
            if (r != revision || b->verticalScrollBar()->value() != value || b->verticalScrollBar()->maximum() != maximum || pendingDownloads() > 0) {
                revision = r;
                value    = b->verticalScrollBar()->value();
                maximum  = b->verticalScrollBar()->maximum();
                since    = t.elapsed();
            } else if (t.elapsed() - since > 2000) {
                return;
            }
        }
    }

    // The parent's side: one child (this executable, reload261Child) for a chat, a reader and a scaling; its output.
    bool runChild(const QByteArray& chat, const QByteArray& reader, const QByteArray& scaling, const QByteArray& extra, int reloads, QString* out)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("QT_FONT_DPI"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        env.insert(QStringLiteral("TSMEDIA_RELOAD261_CHILD"), QString::fromLatin1(reader));
        env.insert(QStringLiteral("TSMEDIA_RELOAD261_KIND"), QString::fromLatin1(chat));
        env.insert(QStringLiteral("TSMEDIA_RELOAD261_RELOADS"), QString::number(reloads));
        QList<QByteArray> pairs = extra.split(';');
        if (scaling.contains('='))
            pairs.prepend(scaling);
        for (const QByteArray& pair : qAsConst(pairs)) {
            if (pair.contains('='))
                env.insert(QString::fromLatin1(pair.left(pair.indexOf('='))), QString::fromLatin1(pair.mid(pair.indexOf('=') + 1)));
        }
        // The offscreen platform finds no fonts of its own; Windows' fonts give the chat TeamSpeak's text sizes.
        const QString fonts = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts"));
        if (QDir(fonts).exists() && !env.contains(QStringLiteral("QT_QPA_FONTDIR")))
            env.insert(QStringLiteral("QT_QPA_FONTDIR"), fonts);
        QProcess child;
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QStringLiteral("offscreen"), QStringLiteral("reload261Child")});
        const bool ok = finishChild(child, "reload261Child", out);
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_SHOW")) // the child's whole story, for a look at the steps
            std::fprintf(stdout, "%s\n", out->toUtf8().constData());
        if (!ok) {
            qWarning("%s", qPrintable(out->right(3000)));
            return false;
        }
        return true;
    }

    struct Measure {
        int     reload = 0;
        QString phase;
        int     gap   = 0;
        qreal   dy    = 0;
        int     block = -1;
        int     step  = 0;
    };
    // "reload261: measure R PHASE gap G dy D block B step S"
    static QVector<Measure> measures(const QString& out)
    {
        static const QRegularExpression re(QStringLiteral("^reload261: measure (\\d) (\\w+) gap (-?\\d+) dy (-?[0-9.e+]+) block (-?\\d+) step (\\d+)"),
                                           QRegularExpression::MultilineOption);
        QVector<Measure> all;
        auto             it = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            Measure                       x;
            x.reload = m.captured(1).toInt();
            x.phase  = m.captured(2);
            x.gap    = m.captured(3).toInt();
            x.dy     = m.captured(4).toDouble();
            x.block  = m.captured(5).toInt();
            x.step   = m.captured(6).toInt();
            all.append(x);
        }
        return all;
    }

    PluginDll*                                    m_plugin = nullptr;
    QSet<anyID>                                   m_delivered;
    int                                           m_logFrom = 0;
    QHash<QString, QPair<bool, QString>>          m_sideBySide;    // runRowsSideBySide: function|environment -> ok, output
    QSet<QString>                                 m_sideBySideRan; // ... the functions whose children it started
    QElapsedTimer                                 m_loadClock; // reloadNormalUse: the load's timed actions
    QVector<QPair<qint64, std::function<void()>>> m_due;
    int                                           m_bookedBlanks[2][2] = {{0, 0}, {0, 0}}; // darkStandInsBooked: [with blurhashes][U0, U1]
    bool                                          m_bookedLoaded[2]    = {false, false};
};

TSMEDIA_REGISTER_TEST(TestReload261)

#include "tst_reload261.moc"
