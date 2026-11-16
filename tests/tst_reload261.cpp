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
#include <QLibrary>
#include <QMainWindow>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QtTest>

#include <functional>
#include <memory>

#include "fakets3.h"
#include "layoutdoc.h"
#include "linkpreview/linkurl.h"
#include "medialink.h"
#include "polls.h"
#include "testmain.h"
#include "ts3api.h"
#include "tschat.h"

#include "teamspeak/public_errors.h"

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
        if (!setFunctionPointers || !init || !shutdown || !transferStatus)
            return QStringLiteral("exports missing");
        setFunctionPointers(ts3::funcs);
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
    }
    bool loaded() const { return lib != nullptr; }
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

QByteArray picture(int w, int h, int seed)
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
    image.save(&buffer, "PNG");
    return bytes;
}

// A TS Media picture on the fake server, and its link as a sender's TS Media writes it.
MediaLink mediaLink(int seed, int w, int h, quint32 album = 0, int index = 0, int count = 0)
{
    const QByteArray bytes = picture(w, h, seed);
    MediaLink        link;
    link.host       = QString::fromLatin1(fakets3::kHost);
    link.port       = fakets3::kPort;
    link.serverUid  = QString::fromLatin1(fakets3::kServerUid);
    link.channelId  = fakets3::kChannel;
    link.path       = QStringLiteral("/tsmedia");
    link.fileName   = QStringLiteral("photo%1.png").arg(seed);
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
        PluginDll plugin;
        QCOMPARE(plugin.load(dll), QString());
        m_plugin = &plugin;
        settle(browser, 30000);
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
            printPluginSteps();
            report(QStringLiteral("reload %1 unloaded: ").arg(r) + describe(whereOf(browser)));
            if (gapMs > 0) {
                QTest::qWait(gapMs); // TeamSpeak's event loop runs (the Options dialog) before the DLL is loaded again
                report(QStringLiteral("reload %1 unloaded+events: ").arg(r) + describe(whereOf(browser)) + QStringLiteral(" true max %1").arg(trueMaximum(browser)));
            }
            if (move != 0) { // the reader scrolls while TS Media is off
                bar->setValue(bar->value() + move);
                report(QStringLiteral("moved %1 block %2: ").arg(r).arg(whereOf(browser).block) + describe(whereOf(browser)));
            }
            measure(r, "unloaded");
            m_logFrom = fakets3::logLines().size();
            QCOMPARE(plugin.load(dll), QString());
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
        QTest::qWait(100);
    }

  private:
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
        if (!child.waitForStarted())
            return false;
        const bool finished = child.waitForFinished(300000);
        if (!finished)
            child.kill();
        *out = QString::fromUtf8(child.readAll());
        if (!qEnvironmentVariableIsEmpty("TSMEDIA_RELOAD261_SHOW")) // the child's whole story, for a look at the steps
            std::fprintf(stdout, "%s\n", out->toUtf8().constData());
        if (!(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0)) {
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
    QElapsedTimer                                 m_loadClock; // reloadNormalUse: the load's timed actions
    QVector<QPair<qint64, std::function<void()>>> m_due;
};

TSMEDIA_REGISTER_TEST(TestReload261)

#include "tst_reload261.moc"
