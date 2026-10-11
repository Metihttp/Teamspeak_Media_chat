// 2.3 compose-v2: the send window itself (ComposeDialog) with a fake host: "Add files…" opens the file
// picker (it was never connected in 2.2: a click did nothing), Ctrl+O and the empty drop zone too, a
// cancelled picker adds nothing; Enter sends, Esc closes, blockers keep Send off; the grid tiles'
// spoiler, remove and keyboard shortcuts; drops; and the colours keep their contrast in both skins.
// Run with -platform offscreen (no window takes the focus).

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolTip>
#include <QUrl>
#include <QtTest>

#include <utility>

#include "composedialog.h"
#include "composewidgets.h"
#include "uiutil.h"
#include "testmain.h"

namespace {

// What the window asked of its host. Declared before the window's Owner, so it outlives the window.
struct Calls {
    int                  picks      = 0;
    QWidget*             pickParent = nullptr;
    QString              pickTitle;
    QStringList          answer;
    QVector<SendRequest> sent;
    QString              blocker;
};

// Deletes the window at the end of a test (also a failed one) unless it closed itself.
struct Owner {
    QPointer<ComposeDialog> dialog;
    ~Owner() { delete dialog.data(); }
    ComposeDialog* operator->() const { return dialog.data(); }
    ComposeDialog* get() const { return dialog.data(); }
};

ComposeHost fakeHost(Calls* calls)
{
    ComposeHost host;
    host.describeTarget  = [](const ChatTarget&) { return QStringLiteral("the channel “Lobby”"); };
    host.blocker         = [calls](const ChatTarget&) { return calls->blocker; };
    host.uploadLimitMB   = [](const ChatTarget&) { return 100; };
    host.linkContext     = [](const ChatTarget&) { return compose::LinkContext(); };
    host.savePastedImage = [](const QImage&) { return QString(); };
    host.send            = [calls](const SendRequest& request) {
        calls->sent.append(request);
        return calls->sent.size();
    };
    host.rememberAlbum = [](bool) {};
    host.pickFiles     = [calls](QWidget* parent, const QString& title, const QString&, const QString&) {
        ++calls->picks;
        calls->pickParent = parent;
        calls->pickTitle  = title;
        return calls->answer;
    };
    return host;
}

ChatTarget channel()
{
    ChatTarget target;
    target.sch  = 1;
    target.mode = TextMessageTarget_CHANNEL;
    return target;
}

QString makeImage(const QTemporaryDir& dir, const QString& name, const QColor& color)
{
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(color);
    const QString path = dir.path() + QLatin1Char('/') + name;
    image.save(path);
    return path;
}

// Until the window's workers have looked at every file and the views are rebuilt.
void settle(ComposeDialog* dialog)
{
    QElapsedTimer t;
    t.start();
    while ((dialog->isBusy() || t.elapsed() < 50) && t.elapsed() < 10000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // views replaced meanwhile
}

template <typename T>
QList<T*> shown(QWidget* root, const char* name)
{
    QList<T*> out;
    for (T* w : root->findChildren<T*>(QString::fromLatin1(name))) {
        if (w->isVisibleTo(root))
            out << w;
    }
    return out;
}

template <typename T>
T* named(QWidget* root, const char* name)
{
    const QList<T*> all = shown<T>(root, name);
    return all.isEmpty() ? nullptr : all.first();
}

} // namespace

class TestComposeDialog : public QObject
{
    Q_OBJECT

  private slots:
    void addFilesButtonOpensThePicker();
    void ctrlOAndTheEmptyZoneOpenThePicker();
    void cancelledPickerAddsNothing();
    void enterSendsAndEscCloses();
    void blockerKeepsSendOff();
    void tilesSpoilerRemoveAndKeys();
    void dropAddsFiles();
    void pasteAddsPicture();
    void themeContrast();
    // 2.3 compose-v2 review
    void tooltipsKeepTheirBackground();
    void clickedRemoveKeepsTheCaptionFocus();
    void deleteKeyMovesToTheNextTile();
    void altSMarksTheSingleItem();
    void singleViewKeepsItsFocusWhenRebuilt();
    void letterboxShowsThePicture();
    void confirmationsKeepAFocusRingInDarkSkins();
    void focusRingSurvivesWindowAndMenuSwitches();
};

void TestComposeDialog::addFilesButtonOpensThePicker()
{
    QTemporaryDir dir;
    Calls         calls;
    calls.answer = QStringList{makeImage(dir, QStringLiteral("a.png"), Qt::red), makeImage(dir, QStringLiteral("b.png"), Qt::blue)};
    Owner dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    auto* add = named<QAbstractButton>(dialog.get(), "tsmComposeAddFiles");
    QVERIFY(add);
    QVERIFY(add->isEnabled());
    // The 2.2 bug: the button had no connection; a click did nothing.
    QTest::mouseClick(add, Qt::LeftButton);
    QCOMPARE(calls.picks, 1);
    QCOMPARE(calls.pickParent, static_cast<QWidget*>(dialog.get())); // over the send window, not TeamSpeak's
    QCOMPARE(calls.pickTitle, QStringLiteral("Add files to send"));
    QCOMPARE(dialog->itemCount(), 2);
    settle(dialog.get());
    // The same files again are not added twice; a new one is.
    calls.answer << makeImage(dir, QStringLiteral("c.png"), Qt::green);
    named<QAbstractButton>(dialog.get(), "tsmComposeAddFiles")->click();
    QCOMPARE(calls.picks, 2);
    QCOMPARE(dialog->itemCount(), 3);
    settle(dialog.get());
    QCOMPARE(shown<QWidget>(dialog.get(), "tsmComposeTile").size(), 3);
}

void TestComposeDialog::ctrlOAndTheEmptyZoneOpenThePicker()
{
    QTemporaryDir dir;
    Calls         calls;
    calls.answer = QStringList{makeImage(dir, QStringLiteral("a.png"), Qt::red)};
    Owner dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    // Nothing yet: the drop zone, which opens the picker when clicked.
    auto* empty = named<QAbstractButton>(dialog.get(), "tsmComposeEmpty");
    QVERIFY(empty);
    QTest::mouseClick(empty, Qt::LeftButton);
    QCOMPARE(calls.picks, 1);
    QCOMPARE(dialog->itemCount(), 1);
    settle(dialog.get());
    QVERIFY(!named<QAbstractButton>(dialog.get(), "tsmComposeEmpty"));
    // Ctrl+O.
    calls.answer = QStringList{makeImage(dir, QStringLiteral("b.png"), Qt::blue)};
    QTest::keyClick(dialog->captionField(), Qt::Key_O, Qt::ControlModifier);
    QCOMPARE(calls.picks, 2);
    QCOMPARE(dialog->itemCount(), 2);
    settle(dialog.get());
}

void TestComposeDialog::cancelledPickerAddsNothing()
{
    Calls calls; // answers nothing: the picker was cancelled
    Owner dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    named<QAbstractButton>(dialog.get(), "tsmComposeAddFiles")->click();
    QCOMPARE(calls.picks, 1);
    QCOMPARE(dialog->itemCount(), 0);
    // Nothing changed: Esc closes without asking, and the window goes (deleted on close).
    QTest::keyClick(dialog->captionField(), Qt::Key_Escape);
    QVERIFY(!dialog->isVisible());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(dialog.dialog.isNull());
}

void TestComposeDialog::enterSendsAndEscCloses()
{
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    QSignalSpy    spy(dialog.get(), &ComposeDialog::sent);
    dialog->addFiles({makeImage(dir, QStringLiteral("a.png"), Qt::red)});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    settle(dialog.get());
    auto* box = dialog->findChild<QDialogButtonBox*>();
    QVERIFY(box); // the live-test driver finds Send there
    QAbstractButton* sendButton = nullptr;
    for (QAbstractButton* b : box->buttons()) {
        if (box->buttonRole(b) == QDialogButtonBox::AcceptRole)
            sendButton = b;
    }
    QVERIFY(sendButton);
    QCOMPARE(sendButton->text(), QStringLiteral("Send"));
    QVERIFY(sendButton->isEnabled());
    QTest::keyClicks(dialog->captionField(), QStringLiteral("Hello"));
    QTest::keyClick(dialog->captionField(), Qt::Key_Return);
    QCOMPARE(calls.sent.size(), 1);
    QCOMPARE(calls.sent.first().items.size(), 1);
    QCOMPARE(calls.sent.first().caption, QStringLiteral("Hello"));
    QCOMPARE(spy.count(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(dialog.dialog.isNull()); // sent: closed and deleted
    // (Esc with a typed caption asks first: a QMessageBox, which Qt's offscreen platform can't show on
    // Windows; cancelledPickerAddsNothing checks Esc on an untouched window.)
}

void TestComposeDialog::blockerKeepsSendOff()
{
    QTemporaryDir dir;
    Calls         calls;
    calls.blocker = QStringLiteral("You're not connected to a server. Connect to one to send files.");
    Owner dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->addFiles({makeImage(dir, QStringLiteral("a.png"), Qt::red)});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    settle(dialog.get());
    auto* sendButton = named<QAbstractButton>(dialog.get(), "tsmComposeSend");
    QVERIFY(sendButton);
    QVERIFY(!sendButton->isEnabled());
    bool bannerShown = false;
    for (QLabel* label : dialog->findChildren<QLabel*>()) {
        if (label->isVisibleTo(dialog.get()) && label->text() == calls.blocker)
            bannerShown = true;
    }
    QVERIFY(bannerShown);
    QTest::keyClick(dialog->captionField(), Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClick(dialog->captionField(), Qt::Key_Return);
    QVERIFY(calls.sent.isEmpty());
    QVERIFY(!dialog.dialog.isNull());
    // Connected again (checked every second): Send comes back.
    calls.blocker.clear();
    QTRY_VERIFY_WITH_TIMEOUT(sendButton->isEnabled(), 3000);
}

void TestComposeDialog::tilesSpoilerRemoveAndKeys()
{
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->addFiles({makeImage(dir, QStringLiteral("a.png"), Qt::red), makeImage(dir, QStringLiteral("b.png"), Qt::blue),
                      makeImage(dir, QStringLiteral("c.png"), Qt::green)});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    settle(dialog.get());
    QCOMPARE(shown<QWidget>(dialog.get(), "tsmComposeTile").size(), 3);
    // The tile's spoiler toggle, with the old helper text in its tooltip.
    const QList<QAbstractButton*> spoilers = shown<QAbstractButton>(dialog.get(), "tsmTileSpoiler");
    QCOMPARE(spoilers.size(), 3);
    spoilers.first()->click();
    QVERIFY(dialog->itemAt(0).spoiler);
    QVERIFY(spoilers.first()->isChecked());
    QVERIFY(spoilers.first()->toolTip().contains(QStringLiteral("2.1 or older")));
    // S on a focused tile, then Delete removes it.
    const QList<QAbstractButton*> removes = shown<QAbstractButton>(dialog.get(), "tsmTileRemove");
    QCOMPARE(removes.size(), 3);
    removes.at(1)->setFocus(Qt::TabFocusReason);
    QTest::keyClick(removes.at(1), Qt::Key_S);
    QVERIFY(dialog->itemAt(1).spoiler);
    QTest::keyClick(removes.at(1), Qt::Key_Delete);
    settle(dialog.get());
    QCOMPARE(dialog->itemCount(), 2);
    QCOMPARE(QFileInfo(dialog->itemAt(1).path).fileName(), QStringLiteral("c.png"));
    // "Send as an album" for two pictures: a switch, on by default here.
    auto* album = named<QAbstractButton>(dialog.get(), "tsmComposeAlbum");
    QVERIFY(album);
    QVERIFY(album->isCheckable());
    QVERIFY(album->isChecked());
    // One left: the large preview with its own spoiler and remove; no album switch.
    named<QAbstractButton>(dialog.get(), "tsmTileRemove")->click();
    settle(dialog.get());
    QCOMPARE(dialog->itemCount(), 1);
    QVERIFY(!named<QAbstractButton>(dialog.get(), "tsmComposeAlbum"));
    QVERIFY(named<QAbstractButton>(dialog.get(), "tsmTileSpoiler"));
    QVERIFY(named<QAbstractButton>(dialog.get(), "tsmComposeSend")->isEnabled());
    // The last one removed: the empty drop zone, and Send is off.
    named<QAbstractButton>(dialog.get(), "tsmTileRemove")->click();
    settle(dialog.get());
    QCOMPARE(dialog->itemCount(), 0);
    QVERIFY(named<QAbstractButton>(dialog.get(), "tsmComposeEmpty"));
    QVERIFY(!named<QAbstractButton>(dialog.get(), "tsmComposeSend")->isEnabled());
}

void TestComposeDialog::dropAddsFiles()
{
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(makeImage(dir, QStringLiteral("a.png"), Qt::red)), QUrl::fromLocalFile(makeImage(dir, QStringLiteral("b.png"), Qt::blue))});
    const QPoint    at(dialog->width() / 2, dialog->height() / 2);
    QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dialog.get(), &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dialog.get(), &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(dialog->itemCount(), 2);
    settle(dialog.get());
}

void TestComposeDialog::pasteAddsPicture()
{
    // Only Qt's own clipboard: on the real platform this would replace the user's clipboard.
    if (QGuiApplication::platformName() != QLatin1String("offscreen"))
        QSKIP("needs -platform offscreen (an in-process clipboard)");
    Calls calls;
    Owner dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QImage picture(120, 80, QImage::Format_RGB32);
    picture.fill(Qt::magenta);
    QGuiApplication::clipboard()->setImage(picture);
    QTest::keyClick(dialog->captionField(), Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(dialog->itemCount(), 1);
    QVERIFY(dialog->itemAt(0).isPasted());
    QVERIFY(dialog->captionField()->text().isEmpty()); // a picture, not text
    settle(dialog.get());
    QGuiApplication::clipboard()->clear();
}

void TestComposeDialog::themeContrast()
{
    // TeamSpeak's dark skin (dark.qss) and Windows' light palette.
    const std::pair<QColor, QColor> skins[] = {{QColor(0x36, 0x39, 0x3e), QColor(0xdc, 0xdd, 0xde)}, {QColor(0xf0, 0xf0, 0xf0), QColor(Qt::black)}};
    for (const auto& colors : skins) {
        QPalette pal;
        pal.setColor(QPalette::Window, colors.first);
        pal.setColor(QPalette::WindowText, colors.second);
        const composeui::Theme theme = composeui::themeFor(pal);
        QCOMPARE(theme.dark, colors.first.lightness() < 128);
        QVERIFY2(ui::contrastRatio(theme.muted, theme.body) >= 4.5, qPrintable(theme.muted.name()));
        QVERIFY2(ui::contrastRatio(theme.error, theme.body) >= 4.5, qPrintable(theme.error.name()));
        QVERIFY2(ui::contrastRatio(theme.error, theme.text) > 1.0, qPrintable(theme.error.name())); // not the text-colour fallback
        QVERIFY2(ui::contrastRatio(theme.text, theme.footer) >= 4.5, qPrintable(theme.footer.name()));
        QVERIFY2(ui::contrastRatio(theme.onPrimary, theme.primary) >= 4.5, qPrintable(theme.primary.name()));
        QVERIFY2(ui::contrastRatio(theme.accent, theme.body) >= 3.0, qPrintable(theme.accent.name()));
        QVERIFY2(ui::contrastRatio(theme.text, theme.chip) >= 4.5, qPrintable(theme.chip.name()));
    }
}

// ---- 2.3 compose-v2 review ------------------------------------------------------------------------

namespace {

// The window active, so focus and shortcuts work (offscreen: no real window takes the focus).
bool activate(ComposeDialog* dialog)
{
    QApplication::setActiveWindow(dialog);
    return QTest::qWaitForWindowActive(dialog);
}

QWidget* tipLabel()
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->inherits("QTipLabel") && w->isVisible())
            return w;
    }
    return nullptr;
}

} // namespace

void TestComposeDialog::tooltipsKeepTheirBackground()
{
    // On Windows a tooltip is a child of the widget it is for, so the window's style sheet reaches it: the
    // window's rules (transparent containers and labels, role colours) must leave the tooltips opaque, in
    // the light skin and under TeamSpeak's dark one (a transparent tooltip shows black in a real window).
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->addFiles({makeImage(dir, QStringLiteral("a.png"), Qt::red), makeImage(dir, QStringLiteral("b.png"), Qt::blue)});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    settle(dialog.get());
    const QString before = qApp->styleSheet();
    for (int skin = 0; skin < 2; ++skin) {
        if (skin == 1) // TeamSpeak's dark skin: every widget and the tooltips are painted by style sheet
            qApp->setStyleSheet(QString::fromLatin1("QWidget{background:#36393E;color:#dcddde;} QToolTip{background:#36393E;color:#dddddd;border:1px solid #222427;}"));
        for (const char* name : {"tsmComposeAddFiles", "tsmTileRemove", "tsmComposeAlbum"}) {
            QWidget* widget = named<QWidget>(dialog.get(), name);
            QVERIFY2(widget, name);
            QToolTip::showText(widget->mapToGlobal(QPoint(2, 2)), QStringLiteral("Tip"), widget);
            QWidget* tip = tipLabel();
            QVERIFY2(tip, name);
            tip->ensurePolished();
            QVERIFY2(tip->palette().color(tip->backgroundRole()).alpha() == 255, name);
            QToolTip::hideText();
            QApplication::processEvents();
        }
    }
    qApp->setStyleSheet(before);
}

void TestComposeDialog::clickedRemoveKeepsTheCaptionFocus()
{
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->addFiles({makeImage(dir, QStringLiteral("a.png"), Qt::red), makeImage(dir, QStringLiteral("b.png"), Qt::blue),
                      makeImage(dir, QStringLiteral("c.png"), Qt::green)});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QVERIFY(activate(dialog.get()));
    settle(dialog.get());
    dialog->captionField()->setFocus(Qt::MouseFocusReason);
    QTest::keyClicks(dialog->captionField(), QStringLiteral("Hi"));
    // A tile's x takes no focus when clicked: typing goes on in the caption afterwards.
    QAbstractButton* remove = shown<QAbstractButton>(dialog.get(), "tsmTileRemove").first();
    QTest::mouseClick(remove, Qt::LeftButton);
    settle(dialog.get());
    QCOMPARE(dialog->itemCount(), 2);
    QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(dialog->captionField()));
    // Typing on: a space and an S are caption text, not "remove" and "spoiler" on the next tile.
    QTest::keyClicks(QApplication::focusWidget(), QStringLiteral(" s"));
    settle(dialog.get());
    QCOMPARE(dialog->itemCount(), 2);
    QVERIFY(!dialog->itemAt(0).spoiler);
    QCOMPARE(dialog->captionField()->text(), QStringLiteral("Hi s"));
    dialog->captionField()->setText(QStringLiteral("Hi"));
    // Down to one (the large preview), then none: still the caption.
    QTest::mouseClick(shown<QAbstractButton>(dialog.get(), "tsmTileRemove").first(), Qt::LeftButton);
    settle(dialog.get());
    QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(dialog->captionField()));
    QTest::mouseClick(named<QAbstractButton>(dialog.get(), "tsmTileRemove"), Qt::LeftButton);
    settle(dialog.get());
    QCOMPARE(dialog->itemCount(), 0);
    QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(dialog->captionField()));
    QCOMPARE(dialog->captionField()->text(), QStringLiteral("Hi"));
}

void TestComposeDialog::deleteKeyMovesToTheNextTile()
{
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->addFiles({makeImage(dir, QStringLiteral("a.png"), Qt::red), makeImage(dir, QStringLiteral("b.png"), Qt::blue),
                      makeImage(dir, QStringLiteral("c.png"), Qt::green)});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QVERIFY(activate(dialog.get()));
    settle(dialog.get());
    QAbstractButton* remove = shown<QAbstractButton>(dialog.get(), "tsmTileRemove").at(1);
    remove->setFocus(Qt::TabFocusReason);
    QTest::keyClick(remove, Qt::Key_Delete);
    settle(dialog.get());
    QCOMPARE(dialog->itemCount(), 2);
    // Keyboard removal: the focus goes on to the tile that took its place (c.png's x).
    QWidget* focus = QApplication::focusWidget();
    QVERIFY(focus);
    QCOMPARE(focus->objectName(), QStringLiteral("tsmTileRemove"));
    QCOMPARE(shown<QAbstractButton>(dialog.get(), "tsmTileRemove").indexOf(static_cast<QAbstractButton*>(focus)), 1);
}

void TestComposeDialog::altSMarksTheSingleItem()
{
    // 2.2's single view had "Mark as &spoiler"; the toggle on the preview keeps Alt+S.
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    dialog->addFiles({makeImage(dir, QStringLiteral("a.png"), Qt::red)});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QVERIFY(activate(dialog.get()));
    settle(dialog.get());
    auto* spoiler = named<QAbstractButton>(dialog.get(), "tsmTileSpoiler");
    QVERIFY(spoiler);
    QCOMPARE(spoiler->shortcut(), QKeySequence(Qt::ALT | Qt::Key_S));
    QVERIFY(spoiler->toolTip().contains(QStringLiteral("Alt+S")));
    dialog->captionField()->setFocus(Qt::MouseFocusReason);
    QTest::keyClick(dialog->captionField(), Qt::Key_S, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(dialog->itemAt(0).spoiler, 2000);
    QVERIFY(dialog->captionField()->text().isEmpty());
}

void TestComposeDialog::singleViewKeepsItsFocusWhenRebuilt()
{
    // The large preview is built again when its file changed (and when its details arrive): the focus
    // stays on the same control rather than jumping to the caption.
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    const QString path = makeImage(dir, QStringLiteral("a.png"), Qt::red);
    dialog->addFiles({path});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QVERIFY(activate(dialog.get()));
    settle(dialog.get());
    auto* remove = named<QAbstractButton>(dialog.get(), "tsmTileRemove");
    QVERIFY(remove);
    remove->setFocus(Qt::TabFocusReason);
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        file.write(QByteArray(64, '\0')); // the size changed: Send shows it first
    }
    QTest::keyClick(remove, Qt::Key_Return, Qt::ControlModifier);
    settle(dialog.get());
    QVERIFY(calls.sent.isEmpty());
    QWidget* focus = QApplication::focusWidget();
    QVERIFY(focus);
    QCOMPARE(focus->objectName(), QStringLiteral("tsmTileRemove"));
    QVERIFY(focus != remove);
}

void TestComposeDialog::letterboxShowsThePicture()
{
    // A wide picture on the large preview: the bands above and below show it blurred, so the badge and
    // the buttons in the corners sit on the picture rather than on an empty grey band.
    QTemporaryDir dir;
    Calls         calls;
    Owner         dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
    QImage        wide(800, 100, QImage::Format_RGB32);
    wide.fill(QColor(220, 30, 30));
    const QString path = dir.path() + QStringLiteral("/wide.png");
    QVERIFY(wide.save(path));
    dialog->addFiles({path});
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    settle(dialog.get());
    QWidget* stage = named<QWidget>(dialog.get(), "tsmComposeTile");
    QVERIFY(stage);
    QVERIFY(stage->height() > stage->width() / 8 + 20); // letterboxed
    const QImage shot = stage->grab().toImage();
    const QColor band = shot.pixelColor(shot.width() / 2, qRound(6 * shot.devicePixelRatio()));
    QVERIFY2(band.red() > band.green() + 40, qPrintable(band.name()));
    // A spoiler never shows it sharp: the cover fills the whole stage (dimmed, the pill in the middle).
    dialog->setSpoiler(0, true);
    QApplication::processEvents();
    const QImage covered = named<QWidget>(dialog.get(), "tsmComposeTile")->grab().toImage();
    const QColor middle  = covered.pixelColor(covered.width() / 4, covered.height() / 2);
    QVERIFY2(middle.red() < 200, qPrintable(middle.name()));
}

void TestComposeDialog::confirmationsKeepAFocusRingInDarkSkins()
{
    // TeamSpeak's dark skin paints "Discard" and "Keep editing" alike and draws no focus: 2.2 gave the
    // confirmation boxes (children of the window) a ring; the light skin keeps Windows' own.
    const QString before = qApp->styleSheet();
    const QString rule   = QStringLiteral("QMessageBox QPushButton:focus");
    {
        Calls calls;
        Owner dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
        QVERIFY(!dialog->styleSheet().contains(rule));
    }
    qApp->setStyleSheet(QString::fromLatin1("QWidget{background:#36393E;color:#dcddde;} QPushButton{padding:8px 14px;border:none;}"));
    {
        Calls calls;
        Owner dialog{new ComposeDialog(fakeHost(&calls), channel(), nullptr)};
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
        QVERIFY(dialog->styleSheet().contains(rule));
    }
    qApp->setStyleSheet(before);
}

void TestComposeDialog::focusRingSurvivesWindowAndMenuSwitches()
{
    // The controls draw a ring for keyboard focus only. Focus that comes back from another window or from
    // the Quality menu keeps it once the keyboard was used in the window, like Windows' focus rectangles.
    QWidget window;
    auto*   chip = new QWidget(&window);
    using composeui::FocusTracker;
    for (const Qt::FocusReason reason : {Qt::ActiveWindowFocusReason, Qt::PopupFocusReason, Qt::OtherFocusReason}) {
        QFocusEvent in(QEvent::FocusIn, reason);
        window.setAttribute(Qt::WA_KeyboardFocusChange, false);
        QVERIFY(!FocusTracker::byKeyboard(&in, chip)); // mouse users: no ring
        window.setAttribute(Qt::WA_KeyboardFocusChange, true);
        QVERIFY(FocusTracker::byKeyboard(&in, chip));
    }
    QFocusEvent click(QEvent::FocusIn, Qt::MouseFocusReason);
    QVERIFY(!FocusTracker::byKeyboard(&click, chip));
    QFocusEvent tab(QEvent::FocusIn, Qt::TabFocusReason);
    window.setAttribute(Qt::WA_KeyboardFocusChange, false);
    QVERIFY(FocusTracker::byKeyboard(&tab, chip));
}

TSMEDIA_REGISTER_TEST(TestComposeDialog)
#include "tst_composedialog.moc"
