// 2.5 GIFs: the emoji picker's GIFs page offscreen (design_gifs 10.2): the switch row and the 42 px shift of the emoji
// page, the grid's geometry, the footer, the keyboard rules (no send without navigation, Space never sends, Ctrl+Tab
// guarded like Ctrl+E), activation by the pressed id, tool tips, spoilers, the animation budget (one at a time, after a
// rest, decoded on a worker, one video engine per opening), no web requests, the remembered page, and the renders of
// every state at dpr 1 and 2, light and dark (written to $TSMEDIA_RENDER_DIR/gifs when set).

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolTip>
#include <QtTest>

#include <memory>

#include "core.h"
#include "emojipicker.h"
#include "emojisection.h"
#include "emojiprefs.h"
#include "fakets3.h"
#include "giflibrary.h"
#include "gifpanel.h"
#include "gifrules.h"
#include "linkfixtures.h"
#include "linkpreview/linkurl.h"
#include "pickerstyle.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"
#include "uiutil.h"
#include "video/mfvideo.h"

namespace {

QString a(const char* text)
{
    return QString::fromLatin1(text);
}

QString idOf(int n)
{
    return QString::number(n, 16).rightJustified(16, QLatin1Char('0'));
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QByteArray encoded(const QImage& image, const char* format)
{
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

// A synthesized thumbnail: a gradient with a label (an alpha one for a sticker).
QImage thumbImage(int w, int h, const QString& label, int hue, bool alpha = false)
{
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, w, h);
    g.setColorAt(0, QColor::fromHsv(hue, 160, 230));
    g.setColorAt(1, QColor::fromHsv((hue + 60) % 360, 200, 120));
    if (alpha) {
        p.setBrush(g);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(w * 0.15, h * 0.1, w * 0.7, h * 0.8));
    } else {
        p.fillRect(image.rect(), g);
    }
    QFont f = p.font();
    f.setPixelSize(h / 5);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(image.rect(), Qt::AlignCenter, label);
    p.end();
    return image;
}

// A GIF with a logical screen of w x h and frames 1x1 images, each delayCs hundredths of a second.
QByteArray gifBytes(int w, int h, int frames, int delayCs)
{
    QByteArray out("GIF89a");
    out.append(static_cast<char>(w & 0xff)).append(static_cast<char>(w >> 8)).append(static_cast<char>(h & 0xff)).append(static_cast<char>(h >> 8));
    out.append('\x80').append('\0').append('\0');
    out.append("\xff\x40\x40\x00\x00\x00", 6);
    if (frames > 1)
        out.append("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
    for (int i = 0; i < frames; ++i) {
        out.append("\x21\xf9\x04\x00", 4).append(static_cast<char>(delayCs & 0xff)).append(static_cast<char>(delayCs >> 8)).append("\x00\x00", 2);
        out.append("\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00", 15);
    }
    out.append(';');
    return out;
}

struct FakeHost {
    gifs::SendState state;
    QStringList     sent;
    QString         refuse; // a send's error (empty: it goes)
    gifs::Host      host(GifLibrary* lib)
    {
        gifs::Host h;
        h.library = lib;
        h.state   = [this] { return state; };
        h.send    = [this](const QString& id, const gifs::ChatFingerprint& opened, QString* error) {
            if (opened != state.chat) {
                *error = gifs::changedText();
                return false;
            }
            if (!refuse.isEmpty()) {
                *error = refuse;
                return false;
            }
            sent << id;
            return true;
        };
        return h;
    }
};

gifs::SendState channelState()
{
    gifs::SendState s;
    s.canText         = true;
    s.canFiles        = true;
    s.destination     = QString::fromUtf8("the channel \xe2\x80\x9cLobby\xe2\x80\x9d");
    s.uploadLimit     = 100LL << 20;
    s.uploadLimitText = a("over the limit");
    s.chat.valid      = true;
    s.chat.sch        = 1;
    s.chat.serverUid  = a("server=");
    s.chat.mode       = TextMessageTarget_CHANNEL;
    s.chat.channelId  = 5;
    return s;
}

} // namespace

class TestGifPanel : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void sizesAndShift();
    void tilesAndFooter();
    void contrast();
    void englishDates();
    void keyboard();
    void shortcutOverrides();
    void activationById();
    void openGuard();
    void footerReasonsFit();
    void removeAllLeftovers();
    void removeButtonWording();
    void toolTips();
    void spoilerCovered();
    void animationBudget();
    void loopsShareOnePlayer();
    void noWebOnOpen();
    void rememberedPage();
    void renders();

  private:
    // Fills <dir>/gifs with n items (links, files, a placeholder, a spoiler, a sticker), then starts the library.
    void fill(int n = 12);
    std::unique_ptr<EmojiPicker> picker(bool dark, bool gifsPage);
    QString linksDir() const { return m_dir->path() + a("/links"); }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
    std::unique_ptr<GifLibrary>    m_lib;
    FakeHost                       m_host;
    TS3Functions                   m_savedFuncs{};
    QString                        m_savedPluginId;
    Settings                       m_savedSettings;
};

void TestGifPanel::initTestCase()
{
    m_savedFuncs    = ts3::funcs;
    m_savedPluginId = ts3::pluginId;
    m_savedSettings = Settings::instance();
}

void TestGifPanel::cleanupTestCase()
{
    ts3::funcs           = m_savedFuncs;
    ts3::pluginId        = m_savedPluginId;
    Settings::instance() = m_savedSettings;
    GifPanel::setAnimationsEnabledForTests(-1);
    emoji::prefs::setFile(QString());
}

void TestGifPanel::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    fakets3::install(m_dir->path());
    Settings::instance()              = Settings();
    Settings::instance().rememberGifs = true;
    emoji::prefs::setFile(m_dir->path() + a("/emoji.ini"));
    m_core = std::make_unique<Core>();
    m_core->start();
    m_host       = FakeHost();
    m_host.state = channelState();
    GifPanel::setAnimationsEnabledForTests(1);
}

void TestGifPanel::cleanup()
{
    m_lib.reset();
    m_core.reset();
    m_dir.reset();
}

void TestGifPanel::fill(int n)
{
    const QString gifs = m_dir->path() + a("/gifs");
    QJsonArray    list;
    const qint64  now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < n; ++i) {
        gifs::Item item;
        item.id      = idOf(i + 1);
        item.addedSec = item.usedSec = now - i * 60;
        const int kind = i % 6;
        if (kind == 1 || kind == 4) {
            item.source = gifs::Source::File;
            item.kind   = gifs::Kind::Gif;
            item.how    = gifs::How::Sent;
            item.name   = a("party%1.gif").arg(i);
            item.file   = item.id + a(".gif");
            const QByteArray bytes = gifBytes(4, 4, 3, 10);
            item.sha    = QCryptographicHash::hash(bytes + QByteArray::number(i), QCryptographicHash::Sha256);
            item.size   = bytes.size();
            QVERIFY(writeFile(gifs + a("/files/") + item.file, bytes));
        } else {
            item.source = gifs::Source::Link;
            item.kind   = kind == 2 ? gifs::Kind::Gif : gifs::Kind::Loop;
            item.how    = i % 2 ? gifs::How::Saved : gifs::How::Sent;
            item.url    = i == 0 ? a("https://giphy.com/gifs/cat-typing-JIX9t2j0ZTN9S") : a("https://tenor.com/view/item-%1").arg(1000 + i);
            item.urlKey = linkpreview::classify(item.url).key;
        }
        item.spoiler = i == 5;
        if (i != 3) { // one placeholder
            const bool  sticker = i == 2;
            const QSize size    = i == 6 ? QSize(258, 459) : i == 7 ? QSize(459, 258) : QSize(348, 258);
            item.thumb          = item.id + (sticker ? a(".png") : a(".jpg"));
            QVERIFY(writeFile(gifs + a("/thumbs/") + item.thumb,
                              encoded(thumbImage(size.width(), size.height(), QString::number(i + 1), (i * 47) % 360, sticker), sticker ? "PNG" : "JPG")));
        }
        list.append(gifs::Store::itemToJson(item));
    }
    QJsonObject top;
    top.insert(a("format"), 1);
    top.insert(a("items"), list);
    QVERIFY(writeFile(gifs + a("/gifs.json"), QJsonDocument(top).toJson()));
    m_lib = std::make_unique<GifLibrary>(m_core.get(), gifs, linksDir());
    m_lib->start();
    m_lib->waitForWorkers();
}

std::unique_ptr<EmojiPicker> TestGifPanel::picker(bool dark, bool gifsPage)
{
    auto p = std::make_unique<EmojiPicker>(EmojiPicker::Mode::Insert, dark, QColor());
    p->setAttribute(Qt::WA_DeleteOnClose, false);
    if (m_lib)
        p->setGifHost(m_host.host(m_lib.get()));
    if (gifsPage)
        p->showPage(EmojiPicker::Page::Gifs);
    return p;
}

void TestGifPanel::sizesAndShift()
{
    fill();
    QCOMPARE(EmojiPicker::preferredSize(true), QSize(384, 477));
    QCOMPARE(EmojiPicker::preferredSize(false), QSize(384, 435));
    auto plain = std::make_unique<EmojiPicker>(EmojiPicker::Mode::Insert, true, QColor());
    plain->setAttribute(Qt::WA_DeleteOnClose, false);
    QCOMPARE(plain->size(), QSize(384, 435));
    QVERIFY(!plain->pageSwitch());
    QVERIFY(!plain->hasGifHost());
    auto hosted = picker(true, false);
    QCOMPARE(hosted->size(), QSize(384, 477));
    QVERIFY(hosted->pageSwitch());
    QCOMPARE(hosted->accessibleName(), a("Emoji and GIFs"));
    // React mode never gets a host.
    EmojiPicker react(EmojiPicker::Mode::React, true, QColor());
    react.setAttribute(Qt::WA_DeleteOnClose, false);
    react.setGifHost(m_host.host(m_lib.get()));
    QVERIFY(!react.hasGifHost());
    // Every child of the emoji page exactly 42 px lower: the search, the tone button, the tone popup, the grid.
    const auto children = [](EmojiPicker* p) {
        QList<QRect> out;
        out << p->findChild<QLineEdit*>(a("tsmediaEmojiSearch"))->geometry();
        for (QAbstractButton* b : p->findChildren<QAbstractButton*>()) {
            if (b->accessibleName() == a("Skin tone"))
                out << b->geometry();
        }
        out << p->findChild<QWidget*>(a("tsmediaEmojiGrid"))->geometry();
        for (QWidget* w : p->findChildren<QWidget*>()) {
            if (w->accessibleName() == a("Skin tone") && !qobject_cast<QAbstractButton*>(w))
                out << w->geometry();
        }
        return out;
    };
    const QList<QRect> before = children(plain.get());
    const QList<QRect> after  = children(hosted.get());
    QCOMPARE(before.size(), 4);
    QCOMPARE(after.size(), before.size());
    for (int i = 0; i < before.size(); ++i)
        QCOMPARE(after.at(i), before.at(i).translated(0, 42));
    // The pictures: the emoji page of the hosted picker is the old one moved down by 42 px.
    const auto grab = [](QWidget* w) {
        QImage image(w->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        w->render(&image);
        return image;
    };
    // The emoji pictures come from the renderer's worker: rendered until they stop changing.
    QImage oldImage = grab(plain.get());
    QImage newImage = grab(hosted.get());
    for (int round = 0; round < 20; ++round) {
        QTest::qWait(100);
        const QImage again = grab(plain.get());
        newImage           = grab(hosted.get());
        if (again == oldImage)
            break;
        oldImage = again;
    }
    int differ = 0;
    for (int y = 10; y < oldImage.height(); ++y) {
        for (int x = 0; x < oldImage.width(); ++x)
            differ += oldImage.pixel(x, y) != newImage.pixel(x, y + 42) ? 1 : 0;
    }
    QVERIFY2(differ < oldImage.width() * 4, qPrintable(QString::number(differ)));
}

void TestGifPanel::tilesAndFooter()
{
    fill();
    auto p = picker(true, true);
    GifPanel* panel = p->gifPanel();
    QVERIFY(panel);
    QCOMPARE(panel->order().size(), 12);
    QCOMPARE(panel->order().first(), idOf(1)); // newest first
    const QRect t0 = panel->tileRect(0);
    QCOMPARE(t0.size(), QSize(116, 86));
    QCOMPARE(panel->tileRect(1).left() - t0.left(), 120);
    QCOMPARE(panel->tileRect(3).top() - t0.top(), 90);
    QCOMPARE(panel->mapTo(p.get(), t0.topLeft()), QPoint(10, 54));
    QCOMPARE(panel->mapTo(p.get(), QPoint(0, panel->height() - GifPanel::kFooterH)).y(), 425);
    // Nothing hovered: where it goes.
    QCOMPARE(panel->footerTitle(), QString::fromUtf8("Send to the channel \xe2\x80\x9cLobby\xe2\x80\x9d"));
    QCOMPARE(panel->footerText(), QString::fromUtf8("Click to send \xc2\xb7 Shift+click keeps this open"));
    // A link and a file hovered.
    panel->hoverIndex(0);
    QCOMPARE(panel->footerTitle(), a("giphy.com"));
    QVERIFY(panel->footerText().startsWith(QString::fromUtf8("Link \xc2\xb7 sent ")));
    panel->hoverIndex(1);
    QCOMPARE(panel->footerTitle(), a("party1.gif"));
    QVERIFY(panel->footerText().startsWith(QString::fromUtf8("Uploads the file again \xc2\xb7 ")));
    panel->hoverIndex(-1);
    // A password channel: file tiles disabled with the reason, links not.
    m_host.state.canFiles     = false;
    m_host.state.filesBlocked = uploadErrorText(MediaError::Password);
    panel->refreshState();
    panel->hoverIndex(1);
    QCOMPARE(panel->footerText(), uploadErrorText(MediaError::Password));
    QVERIFY(panel->footerDanger());
    QVERIFY(!panel->sendIndex(1, false));
    QVERIFY(m_host.sent.isEmpty());
    QVERIFY(panel->sendIndex(0, false));
    QCOMPARE(m_host.sent, QStringList{idOf(1)});
    // The server chat.
    m_host.state          = channelState();
    m_host.state.serverChat = true;
    panel->refreshState();
    panel->hoverIndex(-1);
    QCOMPARE(panel->footerText(), a("Everyone on this server gets it."));
    panel->hoverIndex(1);
    QCOMPARE(panel->footerText(), gifs::serverFileText());
    // A missing copy.
    m_host.state = channelState();
    panel->refreshState();
    QVERIFY(QFile::remove(m_lib->copyPath(*m_lib->find(idOf(2)))));
    panel->hoverIndex(1);
    QCOMPARE(panel->footerText(), gifs::missingCopyText());
    // The chat changed since opening: every tile disabled for the rest of the opening.
    m_host.state.chat.channelId = 6;
    QVERIFY(!panel->sendIndex(0, false));
    QCOMPARE(panel->footerText(), gifs::changedText());
    m_host.state = channelState();
    QVERIFY(!panel->sendIndex(0, false));
}

void TestGifPanel::contrast()
{
    for (const bool dark : {true, false}) {
        const pickerstyle::Colors c = pickerstyle::colorsFor(dark, QColor());
        QVERIFY(ui::contrastRatio(c.danger, c.background) >= 4.5);
        QVERIFY(ui::contrastRatio(QColor(Qt::white), c.dangerFill) >= 4.5);
        QVERIFY(ui::contrastRatio(c.muted, c.background) >= 4.5);
        QVERIFY(ui::contrastRatio(c.text, c.field) >= 4.5);
        QVERIFY(ui::contrastRatio(c.muted, c.field) >= 4.5);
    }
}

void TestGifPanel::englishDates()
{
    const QDateTime when(QDate(2026, 11, 9), QTime(12, 0));
    for (const QLocale::Language language : {QLocale::German, QLocale::Persian}) {
        QLocale::setDefault(QLocale(language));
        QCOMPARE(gifs::dateText(when.toSecsSinceEpoch()), a("Nov 9"));
    }
    QLocale::setDefault(QLocale(QLocale::English));
}

void TestGifPanel::keyboard()
{
    fill();
    auto p = picker(true, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    QWidget*  grid  = panel->grid();
    panel->focusGrid(Qt::PopupFocusReason);
    QSignalSpy sent(panel, &GifPanel::sent);
    // Right after opening: Enter, Space, Delete do nothing.
    QTest::keyClick(grid, Qt::Key_Return);
    QTest::keyClick(grid, Qt::Key_Space);
    QTest::keyClick(grid, Qt::Key_Delete);
    QVERIFY(m_host.sent.isEmpty());
    QCOMPARE(m_lib->count(), 12);
    QVERIFY(panel->focusedId().isEmpty());
    // Down focuses item 0; Enter sends it.
    QTest::keyClick(grid, Qt::Key_Down);
    QCOMPARE(panel->focusedId(), idOf(1));
    QCOMPARE(grid->accessibleName(), a("GIF link from giphy.com"));
    QTest::keyClick(grid, Qt::Key_Right);
    QTest::keyClick(grid, Qt::Key_Down);
    QCOMPARE(panel->focusedId(), panel->order().at(4));
    QTest::keyClick(grid, Qt::Key_End);
    QCOMPARE(panel->focusedId(), panel->order().last());
    QTest::keyClick(grid, Qt::Key_Home);
    QCOMPARE(panel->focusedId(), panel->order().first());
    QTest::keyClick(grid, Qt::Key_PageDown);
    QCOMPARE(panel->focusedId(), panel->order().last());
    QTest::keyClick(grid, Qt::Key_PageUp);
    QCOMPARE(panel->focusedId(), panel->order().first());
    QTest::keyClick(grid, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(m_host.sent, QStringList{idOf(1)});
    QCOMPARE(sent.count(), 1);
    QCOMPARE(sent.last().at(0).toBool(), true); // Shift: keep open
    QSignalSpy closedBySend(p.get(), &EmojiPicker::closed);
    QTest::keyClick(grid, Qt::Key_Return);
    QCOMPARE(sent.last().at(0).toBool(), false);
    QCOMPARE(closedBySend.count(), 1); // a plain send closes the picker
    p->show();
    // Up from the first row goes to the switch row.
    QSignalSpy up(panel, &GifPanel::upFromGrid);
    QTest::keyClick(grid, Qt::Key_Up);
    QCOMPARE(up.count(), 1);
    // Ctrl+Tab both ways.
    grid->setFocus();
    QTest::keyClick(grid, Qt::Key_Tab, Qt::ControlModifier);
    QCOMPARE(p->page(), EmojiPicker::Page::Emoji);
    QLineEdit* search = p->findChild<QLineEdit*>(a("tsmediaEmojiSearch"));
    QTest::keyClick(search, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(p->page(), EmojiPicker::Page::Gifs);
    // Typing switches to the emoji with the character in the search.
    panel->focusGrid();
    QTest::keyClick(grid, Qt::Key_C, Qt::NoModifier);
    QCOMPARE(p->page(), EmojiPicker::Page::Emoji);
    QCOMPARE(search->text(), a("c"));
    search->clear();
    // On the emoji page Space types a space (Ctrl+E's opening is always the emoji).
    search->setFocus();
    QTest::keyClick(search, Qt::Key_Space);
    QCOMPARE(search->text(), a(" "));
    // Esc: the confirmation bar first, then the picker.
    p->showPage(EmojiPicker::Page::Gifs);
    panel->showConfirm();
    QVERIFY(panel->confirmShown());
    QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : grid, Qt::Key_Escape);
    QVERIFY(!panel->confirmShown());
    QSignalSpy closed(p.get(), &EmojiPicker::closed);
    QTest::keyClick(grid, Qt::Key_E, Qt::ControlModifier); // Ctrl+E closes it, as on the emoji page
    QCOMPARE(closed.count(), 1);
}

void TestGifPanel::shortcutOverrides()
{
    fill();
    auto p = picker(true, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    panel->showConfirm();
    QList<QWidget*> targets = {p->findChild<QLineEdit*>(a("tsmediaEmojiSearch")), p->findChild<QWidget*>(a("tsmediaEmojiGrid")), p->pageSwitch(),
                               p->moreButton()};
    targets += panel->keyWidgets();
    for (QWidget* w : qAsConst(targets)) {
        QVERIFY(w);
        for (const auto& key : {std::make_pair(Qt::Key_Tab, Qt::KeyboardModifiers(Qt::ControlModifier)),
                                std::make_pair(Qt::Key_Backtab, Qt::KeyboardModifiers(Qt::ControlModifier | Qt::ShiftModifier)),
                                std::make_pair(Qt::Key_E, Qt::KeyboardModifiers(Qt::ControlModifier))}) {
            QKeyEvent override(QEvent::ShortcutOverride, key.first, key.second);
            override.ignore();
            QCoreApplication::sendEvent(w, &override);
            QVERIFY2(override.isAccepted(), qPrintable(w->objectName()));
        }
    }
}

void TestGifPanel::activationById()
{
    fill();
    auto p = picker(true, true);
    p->show();
    QTest::qWait(GifPanel::kOpenGuardMs + 50); // past the opening's click guard (openGuard)
    GifPanel* panel = p->gifPanel();
    QWidget*  grid  = panel->grid();
    const auto centre = [&](int index) { return grid->mapFrom(panel, panel->tileRect(index).center()); };
    const QString third = panel->order().at(3);
    // A new record, a touch, a save arrive between press and release: the pressed tile's id goes, never another.
    QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier, centre(3));
    QString message;
    QVERIFY(m_lib->saveLink(a("https://tenor.com/view/new-99999"), linkpreview::classify(a("https://tenor.com/view/new-99999")), QString(), false,
                            gifs::Kind::Loop, &message));
    QVERIFY(m_lib->touch(panel->order().at(7)));
    QCOMPARE(panel->order().size(), 12); // frozen order
    QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier, centre(3));
    QCOMPARE(m_host.sent, QStringList{third});
    // Shift+click twice: the same id twice, the picker stays.
    QSignalSpy sent(panel, &GifPanel::sent);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::ShiftModifier, centre(3));
    QTest::mouseClick(grid, Qt::LeftButton, Qt::ShiftModifier, centre(3));
    QCOMPARE(m_host.sent, (QStringList{third, third, third}));
    QVERIFY(sent.last().at(0).toBool());
    // A Remove of tile 2 between press and release on tile 3: nothing.
    m_host.sent.clear();
    QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier, centre(3));
    QVERIFY(m_lib->remove(panel->order().at(2)));
    QCOMPARE(panel->order().size(), 11);
    QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier, centre(3));
    QVERIFY(m_host.sent.isEmpty());
}

void TestGifPanel::openGuard()
{
    // A mouse opening on the GIFs page (or a page switch): a click within kOpenGuardMs sends nothing, so a habitual
    // click where the emoji page's recent emoji sit never posts a GIF (review fix; design_gifs 13.2 #2).
    fill();
    auto p = picker(true, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    QWidget*  grid  = panel->grid();
    const QPoint at = grid->mapFrom(panel, panel->tileRect(0).center());
    QVERIFY(panel->pressGuarded());
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, at);
    QVERIFY(m_host.sent.isEmpty());
    QTest::qWait(GifPanel::kOpenGuardMs + 50);
    QVERIFY(!panel->pressGuarded());
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, at);
    QCOMPARE(m_host.sent.size(), 1);
    // A press inside the guard released after it: nothing either (the press must start after it).
    m_host.sent.clear();
    p->showPage(EmojiPicker::Page::Emoji);
    p->showPage(EmojiPicker::Page::Gifs);
    QVERIFY(panel->pressGuarded());
    QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier, at);
    QTest::qWait(GifPanel::kOpenGuardMs + 50);
    QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier, at);
    QVERIFY(m_host.sent.isEmpty());
}

void TestGifPanel::footerReasonsFit()
{
    // A long block reason gets a second line in the footer (no "...isn't su..."), the title one line (review fix).
    fill();
    auto p = picker(false, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    m_host.state.canFiles     = false;
    m_host.state.filesBlocked = a("Sending files to password-protected channels isn't supported by this server.");
    panel->refreshState();
    const int file = panel->order().indexOf(idOf(2)); // i == 1: a file item
    QVERIFY(file >= 0);
    panel->hoverIndex(file);
    QVERIFY(panel->footerDanger());
    const QStringList lines = panel->footerTextLines();
    QVERIFY(lines.size() >= 1 && lines.size() <= 2);
    QCOMPARE(lines.size(), 2); // too long for one line whatever the font
    QVERIFY(!lines.first().contains(QChar(0x2026)));
    QVERIFY(m_host.state.filesBlocked.startsWith(lines.first()));
    // Offscreen without Windows' fonts Qt measures with a wide stand-in: "nothing cut" needs real metrics (Segoe UI).
    QFont lineFont = panel->font();
    lineFont.setPixelSize(12);
    const QFontMetrics fm(lineFont);
    if (fm.horizontalAdvance(a("abcdefghij")) < 10 * 0.7 * 12)
        QVERIFY2(!lines.join(QLatin1Char(' ')).contains(QChar(0x2026)), qPrintable(lines.join(QLatin1Char('|'))));
    // A short line stays one line.
    panel->hoverIndex(panel->order().indexOf(idOf(1)));
    QCOMPARE(panel->footerTextLines().size(), 1);
    // The server chat's file hint beside the mini thumbnail is not a reason, but it is read whole too (live fix: it
    // ended "others may lack acc...").
    m_host.state            = channelState();
    m_host.state.serverChat = true;
    panel->refreshState();
    panel->hoverIndex(file);
    QVERIFY(!panel->footerDanger());
    QCOMPARE(panel->footerText(), gifs::serverFileText());
    const QStringList hint = panel->footerTextLines();
    QVERIFY(hint.size() >= 1 && hint.size() <= 2);
    QVERIFY(gifs::serverFileText().startsWith(hint.first()));
    if (fm.horizontalAdvance(a("abcdefghij")) < 10 * 0.7 * 12) {
        QVERIFY2(!hint.join(QLatin1Char(' ')).contains(QChar(0x2026)), qPrintable(hint.join(QLatin1Char('|'))));
        QCOMPARE(hint.join(QLatin1Char(' ')), gifs::serverFileText());
    }
    panel->hoverIndex(-1);
    // The tile's tool tip is escaped rich text: a sender's file name that looks like HTML is shown as text.
    QCOMPARE(GifPanel::toolTipHtml(a("<img src=x.png>.gif\nUploads"), true), a("<p style='white-space:pre'>&lt;img src=x.png&gt;.gif<br>Uploads</p>"));
    QCOMPARE(GifPanel::toolTipHtml(a("a & b"), false), a("<p>a &amp; b</p>"));
    // The bar's question: one GIF, several, none (leftover copies only).
    QCOMPARE(GifPanel::removeAllQuestion(1), a("Remove the GIF?"));
    QCOMPARE(GifPanel::removeAllQuestion(37), a("Remove all 37 GIFs?"));
    QCOMPARE(GifPanel::removeAllQuestion(0), a("Remove the leftover GIF copies?"));
}

void TestGifPanel::removeAllLeftovers()
{
    // A corrupt list moved aside: the list is empty, its copies stay until Remove all, which must be reachable (the
    // panel's menu and Settings -> Emoji; review fix).
    fill(3);
    m_lib.reset();
    const QString gifs = m_dir->path() + a("/gifs");
    QVERIFY(writeFile(gifs + a("/gifs.json"), "{broken"));
    m_lib = std::make_unique<GifLibrary>(m_core.get(), gifs, linksDir());
    m_lib->start();
    m_lib->waitForWorkers();
    QCOMPARE(m_lib->count(), 0);
    QVERIFY(QFileInfo::exists(gifs + a("/gifs.json.bad")));
    int files = 0;
    QVERIFY(m_lib->leftoverBytes(&files) > 0);
    QVERIFY(files >= 3);
    QVERIFY(m_lib->hasAnythingToRemove());
    QCOMPARE(EmojiSection::removeAllQuestion(0), a("Remove the leftover GIF copies from this computer?"));
    QCOMPARE(EmojiSection::removeAllQuestion(1), a("Remove the GIF from this computer?"));
    QCOMPARE(EmojiSection::removeAllQuestion(4), a("Remove all 4 GIFs from this computer?"));
    m_lib->clear();
    QVERIFY(!m_lib->hasAnythingToRemove());
    QCOMPARE(m_lib->leftoverBytes(), qint64(0));
    QVERIFY(!QFileInfo::exists(gifs + a("/gifs.json.bad")));
}

// 2.6 (design 8.3): the destructive button says "Remove" for one GIF (or only leftover copies) and "Remove all" for more,
// in the picker's bar (built once, its label follows the count at every showConfirm) and in the Settings box.
void TestGifPanel::removeButtonWording()
{
    QCOMPARE(GifPanel::removeButtonText(0), a("Remove"));
    QCOMPARE(GifPanel::removeButtonText(1), a("Remove"));
    QCOMPARE(GifPanel::removeButtonText(2), a("Remove all"));
    QCOMPARE(GifPanel::removeButtonText(37), a("Remove all"));

    // The bar's button fits its text and the Cancel button stays left of it, inside the bar.
    const auto checkBar = [](GifPanel* panel, const QString& expected) {
        auto* remove = panel->findChild<QAbstractButton*>(a("tsmediaGifRemoveAll"));
        auto* cancel = panel->findChild<QAbstractButton*>(a("tsmediaGifCancel"));
        auto* bar    = panel->findChild<QWidget*>(a("tsmediaGifConfirm"));
        QVERIFY(remove && cancel && bar);
        QCOMPARE(remove->text(), expected);
        QCOMPARE(remove->accessibleName(), expected);
        QFont f = remove->font(); // the bar's buttons paint at 13 px DemiBold (gifpanel.cpp scaled())
        f.setPixelSize(13);
        f.setWeight(QFont::DemiBold);
        QCOMPARE(remove->width(), QFontMetrics(f).horizontalAdvance(expected) + 24);
        QVERIFY(remove->geometry().right() <= bar->width());
        QVERIFY(cancel->geometry().right() < remove->x());
        QVERIFY(cancel->x() > 0);
    };

    fill(3);
    {
        auto p = picker(false, true);
        p->show();
        GifPanel* panel = p->gifPanel();
        panel->showConfirm();
        QVERIFY(panel->confirmShown());
        checkBar(panel, a("Remove all"));
        const int wideRemove = panel->findChild<QAbstractButton*>(a("tsmediaGifRemoveAll"))->width();
        panel->hideConfirm();
        // Two removed through the menu: the same bar now asks about one GIF and its button says Remove (narrower).
        panel->removeIndex(0);
        panel->removeIndex(1);
        QCOMPARE(m_lib->count(), 1);
        panel->showConfirm();
        checkBar(panel, a("Remove"));
        QVERIFY(panel->findChild<QAbstractButton*>(a("tsmediaGifRemoveAll"))->width() < wideRemove);
    }

    fill(1);
    {
        auto p = picker(true, true);
        p->show();
        GifPanel* panel = p->gifPanel();
        panel->showConfirm();
        checkBar(panel, a("Remove"));
    }

    // Settings -> General -> Emoji: Remove all GIFs… asks with "Remove" for the one GIF. (QMessageBox::showEvent asks the
    // platform for the window's system menu, which the offscreen platform doesn't have: its Show event is swallowed.)
    QCOMPARE(GifLibrary::instance(), m_lib.get());
    struct SwallowBoxShow : QObject {
        bool eventFilter(QObject* watched, QEvent* event) override { return event->type() == QEvent::Show && qobject_cast<QMessageBox*>(watched); }
    } swallow;
    qApp->installEventFilter(&swallow);
    {
        QWidget window;
        window.setWindowTitle(a("TS Media chat"));
        auto* section = new EmojiSection(&window);
        section->load(Settings::instance());
        auto* button = window.findChild<QPushButton*>(a("tsmediaRemoveAllGifs"));
        QVERIFY(button && button->isEnabled());
        button->click();
        QMessageBox* box = window.findChild<QMessageBox*>();
        QVERIFY(box);
        QStringList texts;
        for (QAbstractButton* b : box->buttons())
            texts << b->text();
        QVERIFY2(texts.contains(a("Remove")) && !texts.contains(a("Remove all")), qPrintable(texts.join(a("|"))));
        box->close();
    }
    qApp->removeEventFilter(&swallow);
}

void TestGifPanel::toolTips()
{
    fill();
    auto p = picker(true, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    QWidget*  grid  = panel->grid();
    const QPoint at = grid->mapFrom(panel, panel->tileRect(0).center());
    QHelpEvent help(QEvent::ToolTip, at, grid->mapToGlobal(at));
    QCoreApplication::sendEvent(grid, &help);
    QVERIFY(QToolTip::isVisible());
    QVERIFY(QToolTip::text().contains(a("giphy.com<br>"))); // escaped rich text, lines kept
    QVERIFY(!QToolTip::text().contains(a("Recently used")));
    QToolTip::hideText();
    // The picker's own tool tip handler gives nothing of the emoji page over the GIFs page.
    QHelpEvent pickerHelp(QEvent::ToolTip, QPoint(100, 100), p->mapToGlobal(QPoint(100, 100)));
    QCoreApplication::sendEvent(p.get(), &pickerHelp);
    QVERIFY(!QToolTip::isVisible() || !QToolTip::text().contains(a("Recently used")));
}

void TestGifPanel::spoilerCovered()
{
    fill();
    auto p = picker(true, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    const int covered = panel->order().indexOf(idOf(6)); // i == 5: the spoiler
    QVERIFY(covered >= 0);
    panel->hoverIndex(covered);
    QVERIFY(panel->footerText().startsWith(QString::fromUtf8("Spoiler \xc2\xb7 ")));
    QTest::qWait(250);
    QVERIFY(panel->animatingId().isEmpty()); // no animation until Reveal
    QCOMPARE(m_lib->animationJobs(), 0);
    // Its blurred variant was asked for (the key suffix "|b").
    m_lib->waitForWorkers();
    QVERIFY(!m_lib->thumbnail(idOf(6), QSize(116, 86), true).isNull());
}

void TestGifPanel::animationBudget()
{
    fill();
    // Two more file items: a GIF over the per-sequence budget and one over the per-frame budget, and a 0-delay one.
    const QString gifs = m_dir->path() + a("/gifs");
    auto p = picker(true, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    const QStringList files = {idOf(2), idOf(5)}; // file items (i == 1, 4)
    // Passing quickly over the tiles: no animation job.
    for (int i = 0; i < 10; ++i) {
        panel->hoverIndex(i);
        QTest::qWait(30);
    }
    QCOMPARE(m_lib->animationJobs(), 0);
    // Resting on a file GIF: exactly one decode, on a worker.
    panel->hoverIndex(panel->order().indexOf(idOf(2)));
    QTest::qWait(200);
    m_lib->waitForWorkers();
    QCOMPARE(m_lib->animationJobs(), 1);
    QVERIFY(m_lib->animationRanOnWorker());
    QTRY_VERIFY(panel->animating());
    // Another tile: the old one stops, at most one sequence.
    panel->hoverIndex(panel->order().indexOf(idOf(5)));
    QVERIFY(!panel->animating());
    QTest::qWait(200);
    m_lib->waitForWorkers();
    QCOMPARE(m_lib->animationJobs(), 2);
    // Leave: none playing.
    panel->hoverIndex(-1);
    QVERIFY(!panel->animating());
    // Windows animations off: the rest is 600 ms.
    GifPanel::setAnimationsEnabledForTests(0);
    panel->hoverIndex(panel->order().indexOf(idOf(2)));
    QTest::qWait(250);
    QCOMPARE(m_lib->animationJobs(), 2);
    QTest::qWait(500);
    QCOMPARE(m_lib->animationJobs(), 3);
    GifPanel::setAnimationsEnabledForTests(1);
    panel->hoverIndex(-1);
    // The budget: 1000 x 1000 x 60 frames and 1500 x 1500 are refused (the still stays); 0-delay plays at <= 10 fps.
    QVERIFY(writeFile(m_lib->copyPath(*m_lib->find(idOf(2))), gifBytes(1000, 1000, 60, 5)));
    QVERIFY(writeFile(m_lib->copyPath(*m_lib->find(idOf(5))), gifBytes(1500, 1500, 2, 5)));
    for (const QString& id : files) {
        QVector<QImage> frames;
        m_lib->requestAnimation(id, QSize(118, 88)); // another size than the kept sequence's: decoded again
        m_lib->waitForWorkers();
        QVERIFY2(!m_lib->takeAnimation(id, &frames, nullptr), qPrintable(id));
    }
    QVERIFY(writeFile(m_lib->copyPath(*m_lib->find(idOf(2))), gifBytes(40, 30, 4, 0)));
    panel->hoverIndex(-1);
    // A new opening (the failures are remembered per opening).
    p.reset();
    p     = picker(true, true);
    p->show();
    panel = p->gifPanel();
    panel->hoverIndex(panel->order().indexOf(idOf(2)));
    QTest::qWait(200);
    m_lib->waitForWorkers();
    QTRY_VERIFY(panel->animating());
    const int before = panel->framePaints();
    QTest::qWait(1000);
    QVERIFY2(panel->framePaints() - before <= 12, qPrintable(QString::number(panel->framePaints() - before)));
    QVERIFY(panel->framePaints() - before >= 3);
    panel->hoverIndex(-1);
}

void TestGifPanel::loopsShareOnePlayer()
{
    if (!mf::available())
        QSKIP("Media Foundation is not available here");
    // 12 loop items whose files are in cache/links: hovering them in turn creates exactly one player.
    const QString gifs = m_dir->path() + a("/gifs");
    QJsonArray    list;
    const qint64  now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < 12; ++i) {
        gifs::Item item;
        item.id         = idOf(100 + i);
        item.source     = gifs::Source::Link;
        item.kind       = gifs::Kind::Loop;
        item.url        = a("https://tenor.com/view/loop-%1").arg(5000 + i);
        item.urlKey     = linkpreview::classify(item.url).key;
        item.width      = 480;
        item.height     = 270;
        item.durationMs = 2400;
        item.addedSec = item.usedSec = now - i;
        list.append(gifs::Store::itemToJson(item));
        const linkpreview::Target t = linkpreview::classify(item.url);
        QVERIFY(writeFile(linksDir() + QLatin1Char('/') + t.key + a(".mp4"), linkfixtures::mp4Header()));
        QJsonObject o;
        o.insert(a("v"), 2);
        o.insert(a("cls"), a("media"));
        o.insert(a("at"), static_cast<double>(now));
        o.insert(a("kind"), a("loop"));
        o.insert(a("mime"), a("video/mp4"));
        o.insert(a("thumb"), QString());
        o.insert(a("file"), t.key + a(".mp4"));
        o.insert(a("size"), static_cast<double>(linkfixtures::mp4Header().size()));
        o.insert(a("w"), 480);
        o.insert(a("h"), 270);
        o.insert(a("d"), 2400);
        QVERIFY(writeFile(linksDir() + QLatin1Char('/') + t.key + a(".json"), QJsonDocument(o).toJson()));
    }
    QJsonObject top;
    top.insert(a("format"), 1);
    top.insert(a("items"), list);
    QVERIFY(writeFile(gifs + a("/gifs.json"), QJsonDocument(top).toJson()));
    m_lib = std::make_unique<GifLibrary>(m_core.get(), gifs, linksDir());
    m_lib->start();
    const bool started = mf::startup();
    {
        auto p = picker(true, true);
        p->show();
        GifPanel* panel = p->gifPanel();
        for (int i = 0; i < 12; ++i) {
            panel->hoverIndex(i);
            QTest::qWait(170);
        }
        QCOMPARE(panel->playersCreated(), 1);
        QCOMPARE(m_lib->animationJobs(), 0); // loops never go through the GIF decoder
        panel->hoverIndex(-1);
    }
    if (started)
        mf::shutdown();
}

void TestGifPanel::noWebOnOpen()
{
    // 30 link items whose cache/links is empty: opening, scrolling and hovering make no request, no download and no
    // Core entry (the library has no fetcher at all; the fake TeamSpeak sees no file request).
    const QString gifs = m_dir->path() + a("/gifs");
    QJsonArray    list;
    for (int i = 0; i < 30; ++i) {
        gifs::Item item;
        item.id     = idOf(200 + i);
        item.source = gifs::Source::Link;
        item.kind   = gifs::Kind::Loop;
        item.url    = a("https://giphy.com/gifs/x-ABCdef%1").arg(100 + i);
        item.urlKey = linkpreview::classify(item.url).key;
        item.addedSec = item.usedSec = QDateTime::currentSecsSinceEpoch() - i;
        list.append(gifs::Store::itemToJson(item));
    }
    QJsonObject top;
    top.insert(a("format"), 1);
    top.insert(a("items"), list);
    QVERIFY(writeFile(gifs + a("/gifs.json"), QJsonDocument(top).toJson()));
    m_lib = std::make_unique<GifLibrary>(m_core.get(), gifs, linksDir());
    m_lib->start();
    auto p = picker(false, true);
    p->show();
    GifPanel* panel = p->gifPanel();
    for (int i = 0; i < 30; i += 3) {
        panel->hoverIndex(i);
        panel->scrollTo(i * 30);
        QTest::qWait(20);
    }
    panel->hoverIndex(5);
    QTest::qWait(200);
    m_lib->waitForWorkers();
    QVERIFY(fakets3::downloads().isEmpty());
    QVERIFY(m_core->keys().isEmpty());
    QVERIFY(!QDir(linksDir()).exists() || QDir(linksDir()).entryList(QDir::Files).isEmpty());
    QCOMPARE(m_lib->animationJobs(), 0); // nothing local to animate
}

void TestGifPanel::rememberedPage()
{
    fill();
    emoji::prefs::setLastTabGifs(false);
    auto p = picker(true, false);
    QCOMPARE(p->page(), EmojiPicker::Page::Emoji);
    p->showPage(EmojiPicker::Page::Gifs);
    QVERIFY(emoji::prefs::lastTabGifs());
    p.reset();
    // A new picker reads it back (EmojiInput opens a mouse opening on it; Ctrl+E always on the emoji).
    emoji::prefs::setFile(m_dir->path() + a("/emoji.ini"));
    QVERIFY(emoji::prefs::lastTabGifs());
    p = picker(true, false);
    p->showPage(EmojiPicker::Page::Emoji);
    QVERIFY(!emoji::prefs::lastTabGifs());
}

void TestGifPanel::renders()
{
    const QString root = qEnvironmentVariable("TSMEDIA_RENDER_DIR");
    fill();
    const auto save = [&root](QWidget* w, bool dark, int dpr, const char* state) {
        QImage image(w->size() * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(dark ? QColor(0x31, 0x33, 0x38) : QColor(0xf2, 0xf3, 0xf5));
        w->render(&image);
        if (!root.isEmpty()) {
            const QString dir = root + a("/gifs/%1_%2x").arg(dark ? a("dark") : a("light")).arg(dpr);
            QDir().mkpath(dir);
            image.save(dir + QLatin1Char('/') + QString::fromLatin1(state) + a(".png"));
        }
        return image;
    };
    for (const bool dark : {true, false}) {
        for (const int dpr : {1, 2}) {
            m_host.state = channelState();
            auto p = picker(dark, false);
            p->show();
            save(p.get(), dark, dpr, "01_emoji_page");
            p->showPage(EmojiPicker::Page::Gifs);
            GifPanel* panel = p->gifPanel();
            panel->grid()->repaint();
            m_lib->waitForWorkers();
            const QImage page = save(p.get(), dark, dpr, "02_gifs_page");
            QVERIFY(page.pixel(QPoint(60, 90) * dpr) != page.pixel(QPoint(60, 460) * dpr)); // tiles drawn
            panel->hoverIndex(0);
            m_lib->waitForWorkers();
            save(p.get(), dark, dpr, "03_hover");
            panel->hoverIndex(-1);
            panel->focusGrid();
            QTest::keyClick(panel->grid(), Qt::Key_Right);
            m_lib->waitForWorkers();
            save(p.get(), dark, dpr, "04_focus");
            m_host.state.canFiles     = false;
            m_host.state.filesBlocked = uploadErrorText(MediaError::Password);
            panel->refreshState();
            panel->hoverIndex(1);
            m_lib->waitForWorkers();
            save(p.get(), dark, dpr, "05_disabled");
            m_host.state = channelState();
            panel->refreshState();
            const QString copy = m_lib->copyPath(*m_lib->find(idOf(5)));
            const QByteArray bytes = [&copy] {
                QFile f(copy);
                return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            }();
            QFile::remove(copy);
            panel->hoverIndex(panel->order().indexOf(idOf(5)));
            save(p.get(), dark, dpr, "06_missing_copy");
            writeFile(copy, bytes);
            panel->hoverIndex(panel->order().indexOf(idOf(6)));
            m_lib->waitForWorkers();
            save(p.get(), dark, dpr, "11_spoiler");
            m_host.state.serverChat = true;
            panel->refreshState();
            panel->hoverIndex(1);
            save(p.get(), dark, dpr, "12_server_chat");
            m_host.state = channelState();
            panel->refreshState();
            panel->hoverIndex(-1);
            panel->showConfirm();
            save(p.get(), dark, dpr, "10_confirm_bar");
            panel->hideConfirm();
            panel->scrollTo(130);
            m_lib->waitForWorkers();
            save(p.get(), dark, dpr, "13_scrolled");
            panel->scrollTo(0);
            m_host.state.chat.channelId = 99;
            panel->refreshState();
            save(p.get(), dark, dpr, "14_chat_changed");
        }
    }
    // The empty states (remember on / off), read-only and unavailable.
    for (const bool dark : {true, false}) {
        auto empty = std::make_unique<GifLibrary>(m_core.get(), m_dir->path() + a("/empty"), linksDir());
        empty->start();
        m_host.state = channelState();
        for (const bool remember : {true, false}) {
            Settings::instance().rememberGifs = remember;
            auto p = std::make_unique<EmojiPicker>(EmojiPicker::Mode::Insert, dark, QColor());
            p->setAttribute(Qt::WA_DeleteOnClose, false);
            p->setGifHost(m_host.host(empty.get()));
            p->showPage(EmojiPicker::Page::Gifs);
            p->show();
            save(p.get(), dark, 1, remember ? "07_empty" : "08_empty_off");
        }
        Settings::instance().rememberGifs = true;
        QVERIFY(writeFile(m_dir->path() + a("/newer/gifs.json"), "{\"format\":2}"));
        auto newer = std::make_unique<GifLibrary>(m_core.get(), m_dir->path() + a("/newer"), linksDir());
        newer->start();
        auto p = std::make_unique<EmojiPicker>(EmojiPicker::Mode::Insert, dark, QColor());
        p->setAttribute(Qt::WA_DeleteOnClose, false);
        p->setGifHost(m_host.host(newer.get()));
        p->showPage(EmojiPicker::Page::Gifs);
        p->show();
        save(p.get(), dark, 1, "09_read_only");
        auto unavailable = std::make_unique<GifLibrary>(m_core.get(), m_dir->path() + a("/newer"), linksDir());
        unavailable->setOpenFailsForTests(true);
        unavailable->start();
        auto q = std::make_unique<EmojiPicker>(EmojiPicker::Mode::Insert, dark, QColor());
        q->setAttribute(Qt::WA_DeleteOnClose, false);
        q->setGifHost(m_host.host(unavailable.get()));
        q->showPage(EmojiPicker::Page::Gifs);
        q->show();
        save(q.get(), dark, 1, "09_unavailable");
    }
}

TSMEDIA_REGISTER_TEST(TestGifPanel)

#include "tst_gifpanel.moc"
