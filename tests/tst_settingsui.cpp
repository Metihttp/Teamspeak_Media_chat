// 2.7 F3: the new Settings window (design_settings 8, design 7.5): the switch, the tokens' contrast floors, the row
// helpers (M9 stacking), the layout tiles, Text size and colors with fake hooks (design 4.3), the whole dialog as
// plugin.cpp builds it: pages, the Privacy link (M10), page keys (L10), the save question (M2), the focus chain, the
// size factor k at 200 % and 150 % (H1), Restore defaults and the chat text size, and the source rules (CRASH RULE).

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QStyleOptionComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

#include <functional>
#include <memory>

#include "appearancesection.h" // 2.7 dark mode
#include "chatlayoutsection.h"
#include "chattextsection.h"
#include "composesettings.h"
#include "compresssettings.h"
#include "darkmode.h" // 2.7 dark mode
#include "emojisection.h"
#include "fakets3.h"
#include "privacysection.h"
#include "settings.h"
#include "settingsdialog.h"
#include "settingssection.h"
#include "settingstheme.h"
#include "settingswidgets.h"
#include "spoilersection.h"
#include "testmain.h"
#include "ts3api.h"
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

// The dialog as plugin.cpp's showSettings() builds it (ChatTextSection with the given hooks).
SettingsDialog* buildDialog(const ChatTextHooks& hooks = ChatTextHooks())
{
    auto* dialog = new SettingsDialog(nullptr);
    dialog->addSection(SettingsDialog::Tab::Sending, new ComposeSettingsSection(dialog));
    dialog->addSection(SettingsDialog::Tab::ReceivingPlayback, new SpoilerSection(dialog));
    dialog->addSection(SettingsDialog::Tab::Privacy, new PrivacySection, true);
    dialog->addSection(SettingsDialog::Tab::LinkPreviews, new LinkPreviewsSection);
    dialog->addSection(SettingsDialog::Tab::Sending, new CompressSettingsSection(dialog, nullptr));
    dialog->addSection(SettingsDialog::Tab::EmojiGifs, new EmojiSection(dialog));
    dialog->addSection(SettingsDialog::Tab::Chat, new ChatLayoutSection(dialog), true);
    dialog->addSection(SettingsDialog::Tab::Chat, new AppearanceSection(dialog), true); // 2.7 dark mode: heads the Chat page
    dialog->addSection(SettingsDialog::Tab::Chat, new ChatTextSection(dialog, hooks));
    return dialog;
}

QCheckBox* switchNamed(QWidget* root, const QString& text)
{
    for (QCheckBox* box : root->findChildren<QCheckBox*>()) {
        if (box->text() == text)
            return box;
    }
    return nullptr;
}

QPushButton* standardButton(SettingsDialog* dialog, QDialogButtonBox::StandardButton which)
{
    for (QDialogButtonBox* box : dialog->findChildren<QDialogButtonBox*>()) {
        if (QPushButton* b = box->button(which))
            return b;
    }
    return nullptr;
}

// QMessageBox::showEvent asks the platform for the window's system menu, which the offscreen platform doesn't have: the
// question's Show event is swallowed (it is still visible, as tst_gifpanel does it).
struct SwallowBoxShow : QObject {
    bool eventFilter(QObject* watched, QEvent* event) override { return event->type() == QEvent::Show && qobject_cast<QMessageBox*>(watched); }
};

// A fake chat for the Text size and colors hooks: the zoom in tenths, what was asked.
struct FakeChat {
    int         tenths = 0;
    QList<int>  set;
    QStringList previewAsked;

    ChatTextHooks hooks()
    {
        ChatTextHooks h;
        h.zoomTenths    = [this] { return tenths; };
        h.zoomPercent   = [this] { return qRound((8.25 + tenths / 10.0) / 8.25 * 100); };
        h.setZoomTenths = [this](int v) {
            set << v;
            tenths = v;
        };
        h.linePreview = [this](const QString& choice) {
            previewAsked << choice;
            LinePreviewColors c;
            c.base    = QColor(0x1e, 0x2a, 0x38);
            c.text    = QColor(0xd8, 0xde, 0xe9);
            c.accent  = choice == QLatin1String("teal") ? QColor(0x3f, 0xc7, 0xb5) : QColor(0x94, 0x9c, 0xf7);
            c.warning = QColor(0xf0, 0xb2, 0x32);
            return c;
        };
        return h;
    }
};

} // namespace

class TestSettingsUi : public QObject {
    Q_OBJECT

    TS3Functions                   m_savedFuncs;
    Settings                       m_savedSettings;
    QFont                          m_savedFont;
    std::unique_ptr<QTemporaryDir> m_dir;

private slots:
    void init()
    {
        m_savedFuncs    = ts3::funcs;
        m_savedSettings = Settings::instance();
        m_savedFont     = QApplication::font();
        m_dir           = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        fakets3::install(m_dir->path()); // the dialog's Apply saves into this folder
        Settings::instance() = Settings();
    }

    void cleanup()
    {
        // As the plugin's shutdown does: deleting one window can delete another (the question with its dialog).
        QList<QPointer<QWidget>> windows;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName().startsWith(QLatin1String("tsmedia")))
                windows.append(w);
        }
        for (const QPointer<QWidget>& w : windows)
            delete w.data();
        QApplication::setFont(m_savedFont);
        ts3::funcs           = m_savedFuncs;
        Settings::instance() = m_savedSettings;
        m_dir.reset();
    }

    // ---- the switch (design_settings 4.1) -----------------------------------------------------------------------------
    void switchBox()
    {
        QWidget    window;
        auto*      layout = new QVBoxLayout(&window);
        QGroupBox* card   = SettingsSection::card(a("Card"), &window);
        auto*      spin   = new QSpinBox;
        SwitchBox* plain  = SettingsSection::switchRow(card, a("Show &things"), a("What it does, said plainly."));
        SwitchBox* longer = SettingsSection::switchRow(card, a("Download images, GIFs and voice messages automatically up to"), QString(), 1, spin);
        layout->addStretch(1);
        window.resize(600, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // It is a QCheckBox: found as one, with its text, the hint as its accessible description.
        QCOMPARE(window.findChildren<QCheckBox*>().size(), 2);
        QCOMPARE(plain->accessibleDescription(), a("What it does, said plainly."));
        QVERIFY(SettingsSection::hintOf(plain));
        QCOMPARE(SettingsSection::hintOf(plain)->text(), a("What it does, said plainly."));
        QCOMPARE(SettingsSection::hintOf(plain)->textFormat(), Qt::PlainText);
        // click() and a click on the title both toggle.
        QVERIFY(!plain->isChecked());
        plain->click();
        QVERIFY(plain->isChecked());
        QTest::mouseClick(plain, Qt::LeftButton, Qt::NoModifier, QPoint(5, plain->height() / 2));
        QVERIFY(!plain->isChecked());
        // The trailing field is no part of the switch: a click there doesn't toggle it, the field gets it.
        QVERIFY(spin->isVisible());
        const QPoint inSpin = longer->mapFrom(&window, spin->mapTo(&window, spin->rect().center()));
        QVERIFY(longer->rect().contains(inSpin));
        QTest::mouseClick(longer, Qt::LeftButton, Qt::NoModifier, inSpin);
        QVERIFY(!longer->isChecked());
        QVERIFY(spin->geometry().right() < longer->mapTo(spin->parentWidget(), QPoint(longer->width(), 0)).x()); // left of the track
        // The field is left of the switch's track, with a gap.
        const int trackLeft = longer->mapTo(&window, QPoint(longer->width(), 0)).x() - SwitchBox::trackSize(longer->font(), 1.0).width();
        QVERIFY2(spin->mapTo(&window, QPoint(spin->width(), 0)).x() < trackLeft - 4, "the field touches the switch");
        // A long title wraps: taller when narrow.
        QVERIFY(longer->heightForWidth(250) > longer->heightForWidth(1200));
        // Indented rows sit further right.
        QVERIFY(longer->mapTo(&window, QPoint()).x() > plain->mapTo(&window, QPoint()).x());
        // Greyed out as a row: title, helper and field, the tick kept.
        plain->setChecked(true);
        SettingsSection::setRowEnabled(plain, false);
        QVERIFY(!plain->isEnabled());
        QVERIFY(!SettingsSection::hintOf(plain)->isEnabled());
        QVERIFY(plain->isChecked());
        SettingsSection::setRowEnabled(longer, false);
        QVERIFY(!spin->isEnabled());
    }

    // ---- the tokens: contrast floors in three skins (design_settings 3.3) ---------------------------------------------
    void tokenContrast()
    {
        struct Skin {
            const char* name;
            QColor      window, text, button;
        };
        const Skin skins[] = {{"TeamSpeak light", QColor(0xf0, 0xf0, 0xf0), QColor(Qt::black), QColor()},
                              {"community dark", QColor(0x36, 0x39, 0x3e), QColor(0xdc, 0xdd, 0xde), QColor(0x72, 0x89, 0xda)},
                              {"custom dark", QColor(0x22, 0x27, 0x2e), QColor(0xc9, 0xd1, 0xd9), QColor(0x2d, 0x33, 0x3b)}};
        for (const Skin& skin : skins) {
            QPalette p;
            p.setColor(QPalette::Window, skin.window);
            p.setColor(QPalette::WindowText, skin.text);
            const settingstheme::Tokens t = settingstheme::tokens(p, skin.button, 1.0);
            QVERIFY2(ui::contrastRatio(t.muted, t.card) >= 4.5, skin.name);
            QVERIFY2(ui::contrastRatio(t.muted, t.window) >= 4.5, skin.name);
            QVERIFY2(ui::contrastRatio(t.muted, t.sidebar) >= 4.5, skin.name);
            QVERIFY2(ui::contrastRatio(t.text, t.card) >= 4.5, skin.name);
            QVERIFY2(ui::contrastRatio(t.disabled, t.card) >= 3.0, skin.name);
            QVERIFY2(ui::contrastRatio(t.control, t.card) >= 3.0, skin.name);
            QVERIFY2(ui::contrastRatio(t.accent, t.card) >= 3.0, skin.name);
            QVERIFY2(ui::contrastRatio(t.onAccent, t.accent) >= 3.0, skin.name);
            QVERIFY2(ui::contrastRatio(t.focus, t.card) >= 3.0, skin.name);
        }
        // The values measured in the prototype (design_settings 3.3).
        QPalette light;
        light.setColor(QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
        light.setColor(QPalette::WindowText, QColor(Qt::black));
        light.setColor(QPalette::Highlight, QColor(0x00, 0x78, 0xd4));
        const settingstheme::Tokens l = settingstheme::tokens(light, QColor(), 1.0);
        QCOMPARE(l.card.name(), a("#fcfcfc"));
        QCOMPARE(l.muted.name(), a("#656565"));
        QCOMPARE(l.accent.name(), a("#0078d4"));
        QPalette dark;
        dark.setColor(QPalette::Window, QColor(0x36, 0x39, 0x3e));
        dark.setColor(QPalette::WindowText, QColor(0xdc, 0xdd, 0xde));
        const settingstheme::Tokens d = settingstheme::tokens(dark, QColor(0x72, 0x89, 0xda), 1.0);
        QVERIFY(d.dark);
        QCOMPARE(d.card.name(), a("#3d4045"));
        QCOMPARE(d.accent.name(), a("#7289da")); // the skin's buttons
        // A gradient window (a custom sheet): the average of its stops, not Qt's default.
        QLinearGradient g(0, 0, 1, 1);
        g.setColorAt(0, QColor(0x10, 0x10, 0x10));
        g.setColorAt(1, QColor(0x30, 0x30, 0x30));
        QCOMPARE(settingstheme::solidOf(QBrush(g), QColor(Qt::white)).name(), a("#202020"));
        // k: the font's pixel size over 11, at least 1.
        QFont f;
        f.setPixelSize(22);
        QCOMPARE(settingstheme::sizeFactor(f, nullptr), 2.0);
        f.setPixelSize(8);
        QCOMPARE(settingstheme::sizeFactor(f, nullptr), 1.0);
        QCOMPARE(settingstheme::tokens(light, QColor(), 2.0).px(9), 18);
    }

    // ---- rows: a wide field turns stacked (M9); the zoom and colour rows always are --------------------------------------
    void fieldRowStacks()
    {
        QWidget    window;
        auto*      layout = new QVBoxLayout(&window);
        QGroupBox* card   = SettingsSection::card(a("Card"), &window);
        auto*      wide   = new QLineEdit;
        wide->setMinimumWidth(300);
        SettingsSection::fieldRow(card, a("A &field"), wide, a("Its helper."));
        auto* narrow = new QComboBox;
        narrow->addItem(a("On"));
        SettingsSection::fieldRow(card, a("A &choice"), narrow, QString());
        layout->addStretch(1);
        window.resize(1200, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(!SettingsSection::rowOf(wide)->stacked()); // 300 px of 1200: side by side
        window.resize(600, 300);
        QTRY_VERIFY(SettingsSection::rowOf(wide)->stacked()); // 300 px of 600: under its title and helper
        QApplication::processEvents();
        QVERIFY(wide->mapTo(&window, QPoint()).y() > SettingsSection::hintOf(wide)->mapTo(&window, QPoint()).y());
        QVERIFY(!SettingsSection::rowOf(narrow)->stacked());
        window.resize(1200, 300);
        QTRY_VERIFY(!SettingsSection::rowOf(wide)->stacked());
        // Text size and colors: stacked at any width.
        ChatTextSection text;
        text.resize(1600, 600);
        text.show();
        QVERIFY(QTest::qWaitForWindowExposed(&text));
        QVERIFY(SettingsSection::rowOf(text.colorBox())->stacked());
        QVERIFY(SettingsSection::rowOf(text.smallerButton())->stacked());
    }

    // ---- the layout tiles: three in a row while they fit, else one per row -------------------------------------------
    void tileRow()
    {
        ChatLayoutSection section;
        section.load(Settings());
        QList<LayoutTile*> tiles = section.findChildren<LayoutTile*>();
        QCOMPARE(tiles.size(), 3);
        auto* layout = dynamic_cast<TileLayout*>(tiles.first()->parentWidget()->layout());
        QVERIFY(layout);
        const int minimum = tiles.first()->minimumSizeHint().width();
        const int gap     = settingstheme::tokensFor(tiles.first()).px(10);
        QCOMPARE(layout->perRow(3 * minimum + 2 * gap), 3);
        QCOMPARE(layout->perRow(3 * minimum), 1);
        QVERIFY(layout->heightForWidth(3 * minimum) > 2 * layout->heightForWidth(3 * minimum + 2 * gap));
        // Still radio buttons with their 2.6 texts, one chosen.
        QVERIFY(tiles.first()->text().startsWith(a("Co&zy:")));
        int chosen = 0;
        for (LayoutTile* tile : qAsConst(tiles))
            chosen += tile->isChecked() ? 1 : 0;
        QCOMPARE(chosen, 1);
    }

    // ---- Text size and colors (design 4.3, 4.4) ---------------------------------------------------------------------
    void chatTextWithHooks()
    {
        FakeChat        chat;
        ChatTextSection section(nullptr, chat.hooks());
        QSignalSpy      changed(&section, &SettingsSection::changed);
        section.load(Settings());
        QCOMPARE(section.sizeLabel()->text(), a("100%"));
        QVERIFY(!section.resetButton()->isEnabled()); // at 0
        QVERIFY(section.smallerButton()->isEnabled() && section.largerButton()->isEnabled());
        // + and -: one step (1.0 pt) at once, through the hook; the label follows.
        section.largerButton()->click();
        QCOMPARE(chat.set, QList<int>({10}));
        QCOMPARE(section.sizeLabel()->text(), a("112%"));
        QVERIFY(section.resetButton()->isEnabled());
        QCOMPARE(section.largerButton()->accessibleDescription(), a("Chat text size: 112%"));
        section.smallerButton()->click();
        section.smallerButton()->click();
        QCOMPARE(chat.set, QList<int>({10, 0, -10}));
        // Reset: 0.
        section.resetButton()->click();
        QCOMPARE(chat.set.last(), 0);
        QCOMPARE(chat.tenths, 0);
        // Disabled at the bounds of Settings::chatZoomRange.
        chat.tenths = Settings::chatZoomRange.max;
        section.refreshZoom();
        QVERIFY(!section.largerButton()->isEnabled());
        QVERIFY(section.smallerButton()->isEnabled());
        chat.tenths = Settings::chatZoomRange.min;
        section.refreshZoom();
        QVERIFY(!section.smallerButton()->isEnabled());
        QVERIFY(section.largerButton()->isEnabled());
        chat.tenths = Settings::chatZoomRange.min + 5; // a step past the bound is clamped
        section.refreshZoom();
        section.smallerButton()->click();
        QCOMPARE(chat.set.last(), Settings::chatZoomRange.min);
        // The zoom isn't a form value: none of that marked the section changed.
        QCOMPARE(changed.count(), 0);
        // The colour: Automatic after load(Settings()); a pick marks the form changed and is stored as the preset name;
        // the preview asks the chat with the choice.
        QCOMPARE(section.colorBox()->currentText(), a("Automatic"));
        QCOMPARE(section.choice(), QString());
        QCOMPARE(section.colorBox()->count(), 7);
        const int teal = section.colorBox()->findText(a("Teal"));
        QVERIFY(teal > 0);
        chat.previewAsked.clear();
        section.colorBox()->setCurrentIndex(teal);
        QCOMPARE(changed.count(), 1);
        QVERIFY(chat.previewAsked.contains(a("teal")));
        QCOMPARE(section.preview()->accent(), QColor(0x3f, 0xc7, 0xb5));
        QCOMPARE(section.preview()->base(), QColor(0x1e, 0x2a, 0x38));
        Settings stored;
        section.store(stored, Settings());
        QCOMPARE(stored.chatLineColor, a("teal"));
        QCOMPARE(stored.chatZoom, 0); // never stored by the form
        // The last item is "Same as the chat text" (text).
        section.colorBox()->setCurrentIndex(section.colorBox()->count() - 1);
        QCOMPARE(section.colorBox()->currentText(), a("Same as the chat text"));
        QCOMPARE(section.choice(), a("text"));
        // load() shows a saved choice; any case is fine; Restore defaults goes back to Automatic and leaves the zoom.
        Settings saved;
        saved.chatLineColor = a("Orange");
        saved.chatZoom      = 30;
        chat.tenths         = 30;
        const int before    = chat.set.size();
        section.load(saved);
        QCOMPARE(section.colorBox()->currentText(), a("Orange"));
        section.restoreDefaults(Settings());
        QCOMPARE(section.colorBox()->currentText(), a("Automatic"));
        QCOMPARE(chat.set.size(), before); // the zoom wasn't touched
        QCOMPARE(section.sizeLabel()->text(), a("136%"));
        // Texts (design 4.4).
        QCOMPARE(section.smallerButton()->accessibleName(), a("Smaller chat text"));
        QCOMPARE(section.largerButton()->accessibleName(), a("Larger chat text"));
        QCOMPARE(section.resetButton()->text(), a("Re&set"));
        QCOMPARE(section.resetButton()->accessibleName(), a("Reset the chat text size to 100%"));
        QCOMPARE(section.sizeLabel()->accessibleName(), a("Chat text size"));
        QCOMPARE(section.colorBox()->accessibleName(), a("Color of messages from TS Media"));
        QCOMPARE(section.preview()->accessibleName(), a("Preview of messages from TS Media in your chat"));
        QCOMPARE(section.smallerButton()->toolTip(), a("Like Ctrl + mouse wheel down in the chat"));
        QCOMPARE(section.findChild<QGroupBox*>()->title(), a("Text size and colors"));
        bool zoomLabel = false;
        for (QLabel* label : section.findChildren<QLabel*>()) {
            if (label->text() == a("Chat te&xt size")) {
                zoomLabel = true;
                // The row, not a button (a QLabel clicks a button buddy): it hands the focus on to a button.
                QCOMPARE(label->buddy(), section.zoomRow());
                QVERIFY(!qobject_cast<QAbstractButton*>(label->buddy()));
                QCOMPARE(section.zoomRow()->focusPolicy(), Qt::ClickFocus);
            }
        }
        QVERIFY(zoomLabel);
        QVERIFY(SettingsSection::hintOf(section.colorBox())->text().contains(a("Lines already in the chat keep their color.")));
    }

    // Review fix: Alt+X (Chat te&xt size) only focuses the row's first usable button; it never clicks it (one click
    // resizes every chat at once and is saved at once).
    void chatTextAccessKeyFocusesOnly()
    {
        FakeChat chat;
        chat.tenths = 10;
        QWidget window;
        auto*   layout = new QVBoxLayout(&window);
        auto*   before = new QLineEdit(&window);
        layout->addWidget(before);
        auto* section = new ChatTextSection(&window, chat.hooks());
        layout->addWidget(section);
        section->load(Settings());
        window.resize(900, 500);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QApplication::setActiveWindow(&window);
        QVERIFY(QTest::qWaitForWindowActive(&window));
        before->setFocus();
        QTRY_VERIFY(before->hasFocus());
        QTest::keyClick(&window, Qt::Key_X, Qt::AltModifier);
        QApplication::processEvents();
        QTest::qWait(300); // animateClick would click 100 ms later
        QVERIFY(chat.set.isEmpty());
        QCOMPARE(chat.tenths, 10);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(section->smallerButton()));
        // At the lower bound the - button is disabled: the focus goes to + instead, still without a click.
        chat.tenths = Settings::chatZoomRange.min;
        section->refreshZoom();
        before->setFocus();
        QTRY_VERIFY(before->hasFocus());
        QTest::keyClick(&window, Qt::Key_X, Qt::AltModifier);
        QTest::qWait(300);
        QVERIFY(chat.set.isEmpty());
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(section->largerButton()));
        // Tab never stops on the row itself.
        QVERIFY(!(section->zoomRow()->focusPolicy() & Qt::TabFocus));
    }

    // Review fix: the size and the preview are read again when the window becomes active or the page is shown
    // (TeamSpeak's chat font changes the percentage without a zoom signal; ActivationChange reaches only the window).
    void chatTextRefreshesOnActivation()
    {
        FakeChat chat;
        QWidget  window;
        auto*    layout  = new QVBoxLayout(&window);
        auto*    section = new ChatTextSection(&window, chat.hooks());
        layout->addWidget(section);
        section->load(Settings());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(section->sizeLabel()->text(), a("100%"));
        chat.tenths = 20; // the chat changed meanwhile, no signal
        QEvent activate(QEvent::WindowActivate);
        QApplication::sendEvent(&window, &activate);
        QCOMPARE(section->sizeLabel()->text(), a("124%"));
        chat.tenths = 10;
        section->hide();
        section->show(); // the Chat page shown again
        QCOMPARE(section->sizeLabel()->text(), a("112%"));
    }

    void chatTextWithoutHooks()
    {
        ChatTextSection section;
        section.load(Settings());
        QCOMPARE(section.sizeLabel()->text(), a("100%"));
        QVERIFY(!section.smallerButton()->isEnabled());
        QVERIFY(!section.largerButton()->isEnabled());
        QVERIFY(!section.resetButton()->isEnabled());
        // The preview: TeamSpeak's light chat in the Automatic colours (today's strings).
        QCOMPARE(section.preview()->base(), QColor(Qt::white));
        QCOMPARE(section.preview()->accent(), QColor(0x47, 0x52, 0xc4));
        QCOMPARE(section.preview()->warning(), QColor(0xb5, 0x47, 0x08));
        section.largerButton()->click(); // disabled: nothing happens, no crash
        QCOMPARE(section.sizeLabel()->text(), a("100%"));
    }

    // ---- the whole window: pages, the Privacy link (M10), page keys (L10) ---------------------------------------------
    void pages()
    {
        SettingsDialog* dialog = buildDialog();
        QListWidget*    nav    = dialog->pageList();
        QCOMPARE(dialog->objectName(), a("tsmediaSettingsDialog"));
        QCOMPARE(nav->objectName(), a("tsmediaSettingsNav"));
        QCOMPARE(nav->accessibleName(), a("Settings pages"));
        QStringList names;
        for (int i = 0; i < nav->count(); ++i)
            names << nav->item(i)->text();
        QCOMPARE(names, QStringList({a("Chat"), a("Emoji & GIFs"), a("Sending"), a("Receiving & playback"), a("Link previews"), a("Privacy"),
                                     a("Servers"), a("About & updates")}));
        for (int i = 0; i < nav->count(); ++i)
            QVERIFY2(!nav->isRowHidden(i), qPrintable(names.at(i))); // every page has content
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QApplication::setActiveWindow(dialog);
        QVERIFY(QTest::qWaitForWindowActive(dialog)); // page keys work while the focus is in the window
        // Cards on their pages, in the designed order.
        // The cards of a page in layout order (hidden pages aren't laid out, so not by position).
        const auto titles = [dialog](SettingsDialog::Tab tab) {
            QStringList                          out;
            QWidget*                             page = dialog->pageArea(tab)->widget();
            std::function<void(QLayout*)> walk = [&](QLayout* layout) {
                for (int i = 0; layout && i < layout->count(); ++i) {
                    QLayoutItem* item = layout->itemAt(i);
                    if (auto* box = qobject_cast<QGroupBox*>(item->widget())) {
                        if (box->isVisibleTo(page))
                            out << box->title();
                    } else if (item->widget()) {
                        walk(item->widget()->layout());
                    } else {
                        walk(item->layout());
                    }
                }
            };
            walk(page->layout());
            return out;
        };
        QCOMPARE(titles(SettingsDialog::Tab::Chat), QStringList({a("Appearance"), a("Chat layout"), a("Messages"), a("Text size and colors")}));
        QCOMPARE(titles(SettingsDialog::Tab::EmojiGifs), QStringList({a("Emoji"), a("GIFs")}));
        QCOMPARE(titles(SettingsDialog::Tab::Sending), QStringList({a("Sending"), a("Dropping files"), a("Videos"), a("Note for people without the plugin")}));
        QCOMPARE(titles(SettingsDialog::Tab::ReceivingPlayback),
                 QStringList({a("Receiving"), a("Preview size"), a("Playback"), a("Spoilers"), a("Media cache")}));
        QCOMPARE(titles(SettingsDialog::Tab::LinkPreviews), QStringList({a("Link previews")}));
        QCOMPARE(titles(SettingsDialog::Tab::Privacy), QStringList({a("Privacy"), a("Chat proxy")}));
        QCOMPARE(titles(SettingsDialog::Tab::Servers), QStringList({a("Servers"), a("Server access")}));
        QCOMPARE(titles(SettingsDialog::Tab::About).first(), a("Updates"));
        // M10: Privacy's link opens Link previews and focuses its first switch.
        dialog->showTab(SettingsDialog::Tab::Privacy);
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::Privacy);
        auto* link = dialog->findChild<QPushButton*>(a("tsmediaPrivacyLinkPreviews"));
        QVERIFY(link);
        QCOMPARE(link->text(), a("&Link previews and your IP address…"));
        link->click();
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::LinkPreviews);
        QCOMPARE(dialog->pageStack()->currentWidget(), static_cast<QWidget*>(dialog->pageArea(SettingsDialog::Tab::LinkPreviews)));
        QTRY_COMPARE(dialog->focusWidget(), static_cast<QWidget*>(switchNamed(dialog, a("Show previews for links to &other websites"))));
        // L10: Ctrl+Tab / Ctrl+PgDown next page, Ctrl+PgUp / Ctrl+Shift+Tab previous, around.
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::Privacy);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_PageDown, Qt::ControlModifier);
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::Servers);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_PageUp, Qt::ControlModifier);
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::Privacy);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::LinkPreviews);
        dialog->showTab(SettingsDialog::Tab::About);
        QTest::keyClick(dialog, Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(dialog->currentTab(), SettingsDialog::Tab::Chat); // around
        // The page scroll areas are named by their page; nothing scrolls sideways at the default size.
        QCOMPARE(dialog->pageArea(SettingsDialog::Tab::Sending)->accessibleName(), a("Sending"));
        delete dialog;
    }

    // ---- M2: X / Esc with unsaved changes asks; the Cancel button doesn't --------------------------------------------
    void saveQuestion()
    {
        SwallowBoxShow swallow;
        qApp->installEventFilter(&swallow);
        const auto    removeFilter = qScopeGuard([&swallow] { qApp->removeEventFilter(&swallow); });
        const QString file         = ts3::dataDir() + a("/settings.ini");
        {
            // No change: closes at once.
            SettingsDialog* dialog = buildDialog();
            dialog->show();
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            QVERIFY(!dialog->hasUnsavedChanges());
            QVERIFY(dialog->unsavedNote()->isHidden());
            dialog->reject();
            QVERIFY(!dialog->isVisible());
            QVERIFY(!dialog->findChild<QMessageBox*>());
            delete dialog;
        }
        {
            // A change, then Esc: the question, the window stays; its Cancel keeps everything; Discard closes unsaved.
            SettingsDialog* dialog = buildDialog();
            dialog->show();
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            QCheckBox* loop = switchNamed(dialog, a("Loo&p videos"));
            QVERIFY(loop);
            loop->click();
            QVERIFY(dialog->hasUnsavedChanges());
            QVERIFY(!dialog->unsavedNote()->isHidden());
            QCOMPARE(dialog->unsavedNote()->text(), a("Not saved yet: OK or Apply saves your changes."));
            QTest::keyClick(dialog, Qt::Key_Escape);
            auto* box = dialog->findChild<QMessageBox*>(a("tsmediaSettingsSaveQuestion"));
            QVERIFY(box);
            QVERIFY(box->isVisible());
            QVERIFY(dialog->isVisible());
            QCOMPARE(box->text(), a("Save your changes?"));
            QCOMPARE(box->informativeText(), a("Some of your changes aren't saved yet."));
            QCOMPARE(box->windowTitle(), a("TS Media chat — Settings"));
            QStringList texts;
            for (QAbstractButton* b : box->buttons())
                texts << b->text();
            QCOMPARE(QSet<QString>(texts.begin(), texts.end()), QSet<QString>({a("Save"), a("Discard"), a("Cancel")}));
            // A second Esc / X doesn't open a second question.
            dialog->reject();
            QCOMPARE(dialog->findChildren<QMessageBox*>().size(), 1);
            const auto button = [box](const QString& text) {
                for (QAbstractButton* b : box->buttons()) {
                    if (b->text() == text)
                        return b;
                }
                return static_cast<QAbstractButton*>(nullptr);
            };
            QPointer<QMessageBox> first = box;
            button(a("Cancel"))->click();
            QTRY_VERIFY(first.isNull());
            QVERIFY(dialog->isVisible());
            QVERIFY(dialog->hasUnsavedChanges());
            dialog->close(); // the window's X
            QPointer<QMessageBox> again = dialog->findChild<QMessageBox*>(a("tsmediaSettingsSaveQuestion"));
            QTRY_VERIFY(again && again->isVisible());
            QVERIFY(dialog->isVisible());
            for (QAbstractButton* b : again->buttons()) {
                if (b->text() == a("Discard")) {
                    b->click();
                    break;
                }
            }
            QVERIFY(!dialog->isVisible());
            QVERIFY(!Settings::instance().loopVideos);
            QVERIFY(!QFile::exists(file));
            delete dialog;
        }
        {
            // Save: the OK path (saved, closed).
            SettingsDialog* dialog = buildDialog();
            dialog->show();
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            switchNamed(dialog, a("Loo&p videos"))->click();
            dialog->reject();
            auto* box = dialog->findChild<QMessageBox*>(a("tsmediaSettingsSaveQuestion"));
            QVERIFY(box);
            for (QAbstractButton* b : box->buttons()) {
                if (b->text() == a("Save")) {
                    b->click();
                    break;
                }
            }
            QVERIFY(!dialog->isVisible());
            QVERIFY(Settings::instance().loopVideos);
            QVERIFY(QFile::exists(file));
            QSettings ini(file, QSettings::IniFormat);
            QCOMPARE(ini.value(a("loopVideos")).toBool(), true);
            delete dialog;
        }
        {
            // The Cancel button discards without asking.
            SettingsDialog* dialog = buildDialog();
            dialog->show();
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            switchNamed(dialog, a("Start videos &muted"))->click();
            QVERIFY(dialog->hasUnsavedChanges());
            standardButton(dialog, QDialogButtonBox::Cancel)->click();
            QVERIFY(!dialog->isVisible());
            QVERIFY(!dialog->findChild<QMessageBox*>());
            QVERIFY(!Settings::instance().videosStartMuted);
            delete dialog;
        }
        {
            // The plugin's shutdown deletes the window with the question open: nothing stays behind.
            SettingsDialog* dialog = buildDialog();
            dialog->show();
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            switchNamed(dialog, a("Start videos &muted"))->click();
            dialog->reject();
            QPointer<QMessageBox> box = dialog->findChild<QMessageBox*>(a("tsmediaSettingsSaveQuestion"));
            QVERIFY(box && box->isVisible());
            delete dialog;
            QVERIFY(box.isNull());
            for (QWidget* w : QApplication::topLevelWidgets())
                QVERIFY(w->objectName() != a("tsmediaSettingsSaveQuestion"));
        }
    }

    // ---- the Tab order: the sidebar, the page's controls in reading order, then the bar (design_settings 4.9, 5.2) ------
    void focusChain()
    {
        SettingsDialog* dialog = buildDialog();
        // A section added after the window was built (plugin.cpp adds them after the buttons) joins the chain too.
        auto* late = new SpoilerSection;
        dialog->addSection(SettingsDialog::Tab::Chat, late);
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QPushButton* restore = standardButton(dialog, QDialogButtonBox::RestoreDefaults);
        QPushButton* ok      = standardButton(dialog, QDialogButtonBox::Ok);
        QPushButton* cancel  = standardButton(dialog, QDialogButtonBox::Cancel);
        QPushButton* apply   = standardButton(dialog, QDialogButtonBox::Apply);
        QVERIFY(restore && ok && cancel && apply);
        for (SettingsDialog::Tab tab : {SettingsDialog::Tab::Chat, SettingsDialog::Tab::Sending, SettingsDialog::Tab::LinkPreviews, SettingsDialog::Tab::Privacy}) {
            dialog->showTab(tab);
            QWidget* page = dialog->pageArea(tab)->widget();
            QList<QWidget*> expected;
            for (QWidget* w : page->findChildren<QWidget*>()) {
                if ((w->focusPolicy() & Qt::TabFocus) == Qt::TabFocus && w->isVisibleTo(page) && !qobject_cast<QAbstractSpinBox*>(w->parentWidget())
                    && !qobject_cast<QComboBox*>(w->parentWidget()))
                    expected << w; // an inner editor isn't a stop of its own
            }
            // Walk from the sidebar: every focusable control of the page comes before Restore defaults, in reading order.
            QList<QWidget*> seen;
            QWidget*        w = dialog->pageList();
            for (int i = 0; i < 2000 && w != restore; ++i) {
                w = w->nextInFocusChain();
                if (page->isAncestorOf(w))
                    seen << w;
            }
            QCOMPARE(w, static_cast<QWidget*>(restore));
            for (QWidget* control : qAsConst(expected)) {
                if (qobject_cast<QAbstractSpinBox*>(control->parentWidget()) || qobject_cast<QComboBox*>(control->parentWidget()))
                    continue; // an inner editor (its field is the stop)
                QVERIFY2(seen.contains(control), qPrintable(QStringLiteral("%1 \"%2\" on page %3")
                                                                .arg(QString::fromLatin1(control->metaObject()->className()), control->accessibleName())
                                                                .arg(static_cast<int>(tab))));
            }
            // Reading order: each one below or right of the one before (rows top to bottom).
            int lastY = -1;
            for (QWidget* control : qAsConst(seen)) {
                if (!control->isVisibleTo(page) || !expected.contains(control))
                    continue;
                const int y = control->mapTo(page, QPoint()).y();
                QVERIFY2(y + control->height() > lastY, qPrintable(control->metaObject()->className()));
                lastY = qMax(lastY, y);
            }
            if (tab == SettingsDialog::Tab::Chat) {
                QCheckBox* lateBox = late->findChild<QCheckBox*>();
                QVERIFY(seen.contains(lateBox));
            }
        }
        // Then the bar: Restore defaults, OK, Cancel, Apply.
        QCOMPARE(restore->nextInFocusChain(), static_cast<QWidget*>(ok));
        QCOMPARE(ok->nextInFocusChain(), static_cast<QWidget*>(cancel));
        QCOMPARE(cancel->nextInFocusChain(), static_cast<QWidget*>(apply));
        delete dialog;
    }

    // ---- live test 2.7 (F3-2): Tab from the bar's last button goes on to the sidebar, never round the bar again --------
    // (QDialogButtonBox makes its first button its focus proxy whenever it lays the buttons out, so Qt's own walk from
    // Apply met a button box and landed on Restore defaults: Tab cycled Restore defaults, OK, Cancel, Apply for good.)
    void tabLeavesTheBar()
    {
        SettingsDialog* dialog = buildDialog();
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QApplication::setActiveWindow(dialog);
        QVERIFY(QTest::qWaitForWindowActive(dialog));
        dialog->showTab(SettingsDialog::Tab::Chat);
        QListWidget* nav     = dialog->pageList();
        QPushButton* restore = standardButton(dialog, QDialogButtonBox::RestoreDefaults);
        QPushButton* ok      = standardButton(dialog, QDialogButtonBox::Ok);
        QPushButton* cancel  = standardButton(dialog, QDialogButtonBox::Cancel);
        QPushButton* apply   = standardButton(dialog, QDialogButtonBox::Apply);
        QVERIFY(nav && restore && ok && cancel && apply);
        const auto tab = [](bool forward) {
            QTest::keyClick(QApplication::focusWidget(), forward ? Qt::Key_Tab : Qt::Key_Backtab, forward ? Qt::NoModifier : Qt::ShiftModifier);
            return QApplication::focusWidget();
        };
        // Nothing changed: Apply is greyed out, so Cancel is the bar's last stop.
        QVERIFY(!apply->isEnabled());
        cancel->setFocus(Qt::TabFocusReason);
        QCOMPARE(tab(true), static_cast<QWidget*>(nav));
        QCOMPARE(tab(false), static_cast<QWidget*>(cancel));
        // The whole round from the sidebar: the page, then Restore defaults, OK, Cancel, and the sidebar again.
        nav->setFocus(Qt::TabFocusReason);
        QList<QWidget*> stops;
        for (int i = 0; i < 60 && (stops.isEmpty() || stops.last() != nav); ++i)
            stops << tab(true);
        QCOMPARE(stops.last(), static_cast<QWidget*>(nav));
        QVERIFY(stops.size() >= 4);
        QCOMPARE(stops.mid(stops.size() - 4), QList<QWidget*>({restore, ok, cancel, nav}));
        // A change: Apply joins the round after Cancel.
        QCheckBox* hover = switchNamed(dialog, a("Show message act&ions on hover"));
        QVERIFY(hover);
        hover->click();
        QVERIFY(apply->isEnabled());
        cancel->setFocus(Qt::TabFocusReason);
        QCOMPARE(tab(true), static_cast<QWidget*>(apply));
        QCOMPARE(tab(true), static_cast<QWidget*>(nav));
        QCOMPARE(tab(false), static_cast<QWidget*>(apply));
        // Review fix: Apply greys itself out once it saved. Space, Enter or a click on it leaves the focus on OK (Qt's own
        // walk from the greyed-out Apply ended on Restore defaults, where the next Enter reset every page), so the next
        // Enter is OK and the window closes with the change kept.
        QTest::keyClick(apply, Qt::Key_Space);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(ok));
        hover->click();
        QVERIFY(apply->isEnabled());
        apply->setFocus(Qt::TabFocusReason);
        QTest::keyClick(apply, Qt::Key_Return); // SettingsDialog::keyPressEvent presses the focused Apply
        QTRY_VERIFY(!apply->isEnabled());
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(ok));
        hover->click();
        const bool changed = hover->isChecked();
        QCOMPARE(changed, !Settings().chatHoverActions); // three clicks from the default
        QVERIFY(apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(ok));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(!dialog->isVisible());
        QCOMPARE(dialog->result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(hover->isChecked(), changed); // not Restore defaults
        QCOMPARE(Settings::instance().chatHoverActions, changed);
        delete dialog;
    }

    // Review fix: the Servers card's three choice fields share one width that holds every item (Automatic downloads,
    // Upload size limit, Note for people without the plugin, so they line up), and the server picker keeps its minimum,
    // whichever ran last: the dialog's combo-box fit (show, a skin, palette or font change) or the card's own texts
    // (Restore defaults, a changed global value).
    void serversFieldsShareOneWidth()
    {
        SettingsDialog* dialog = buildDialog();
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        dialog->showTab(SettingsDialog::Tab::Servers);
        QWidget* section = dialog->findChild<QWidget*>(QStringLiteral("tsmediaServersSection"));
        QVERIFY(section);
        QComboBox* downloads = nullptr;
        QComboBox* note      = nullptr;
        QComboBox* server    = nullptr;
        for (QComboBox* c : section->findChildren<QComboBox*>()) {
            if (c->findText(a("Paused (data saver)")) >= 0)
                downloads = c;
            else if (c->findText(a("Don't add the note")) >= 0)
                note = c;
            else
                server = c;
        }
        const QList<QSpinBox*> spins = section->findChildren<QSpinBox*>();
        QVERIFY(downloads && note && server && spins.size() == 1);
        QSpinBox* limit = spins.first();
        QList<int> first;
        const auto check = [&](const char* when) {
            const int width = limit->minimumWidth();
            QVERIFY2(downloads->minimumWidth() == width && note->minimumWidth() == width,
                     qPrintable(QStringLiteral("%1: downloads %2, note %3, limit %4").arg(QLatin1String(when)).arg(downloads->minimumWidth()).arg(note->minimumWidth()).arg(width)));
            QVERIFY2(width >= settingstheme::comboFitWidth(downloads) && width >= settingstheme::comboFitWidth(note), when);
            QVERIFY2(server->minimumWidth() >= settingstheme::tokensFor(section).px(220), when);
            QVERIFY2(server->minimumWidth() >= settingstheme::comboFitWidth(server), when);
            const QList<int> now = {width, server->minimumWidth()};
            if (first.isEmpty())
                first = now;
            QVERIFY2(now == first, when); // no jumps between the two
        };
        check("after show");
        if (QTest::currentTestFailed())
            return;
        standardButton(dialog, QDialogButtonBox::RestoreDefaults)->click(); // load() -> setGlobals -> updateTexts
        check("after Restore defaults");
        if (QTest::currentTestFailed())
            return;
        QEvent change(QEvent::PaletteChange);
        QApplication::sendEvent(dialog, &change); // applyTheme, as a skin change does
        check("after a skin change");
        if (QTest::currentTestFailed())
            return;
        delete dialog;
    }

    // ---- H1: TeamSpeak at 200 % and 150 % (its font 22 px / 16.5 px; nothing else scales) -------------------------------
    void scaledWindow_data()
    {
        QTest::addColumn<qreal>("pixels");
        QTest::addColumn<qreal>("k");
        QTest::newRow("200%") << 22.0 << 2.0; // TeamSpeak's 8.25 pt at 192 dpi
        QTest::newRow("150%") << 16.5 << 1.5; // at 144 dpi
    }

    void scaledWindow()
    {
        QFETCH(qreal, pixels);
        QFETCH(qreal, k);
        // The test platform's own fallback font is far wider than TeamSpeak's and changes with what earlier tests asked
        // for: TeamSpeak's dialog font (MS Shell Dlg 2 is Tahoma on Windows) makes the widths and line counts real.
        const int  fontId     = QFontDatabase::addApplicationFont(a("C:/Windows/Fonts/tahoma.ttf"));
        const auto removeFont = qScopeGuard([fontId] {
            if (fontId >= 0)
                QFontDatabase::removeApplicationFont(fontId);
        });
        if (fontId < 0)
            QSKIP("Tahoma, TeamSpeak's dialog font, isn't installed");
        QFont font(QFontDatabase::applicationFontFamilies(fontId).value(0));
        font.setPointSizeF(pixels * 72.0 / QGuiApplication::primaryScreen()->logicalDotsPerInchY()); // as TeamSpeak's point font
        QApplication::setFont(font);
        SettingsDialog* dialog = buildDialog();
        QVERIFY2(qAbs(dialog->themeTokens().k - k) < 0.01, qPrintable(QStringLiteral("k %1, font %2 pt %3 px, dpi %4")
                                                                         .arg(dialog->themeTokens().k)
                                                                         .arg(dialog->font().pointSizeF())
                                                                         .arg(dialog->font().pixelSize())
                                                                         .arg(dialog->logicalDpiY())));
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QVERIFY(qAbs(dialog->themeTokens().k - k) < 0.01);
        // 900 x 640 times k, within 92 % of a 1920 x 1080 screen; the smallest window is the sidebar + 460 k.
        const QSize available(1920, 1080);
        const QSize initial = dialog->initialSizeFor(available);
        QCOMPARE(initial, QSize(qRound(900 * k), qRound(640 * k)).boundedTo(available * 0.92));
        const QSize minimum = dialog->minimumSizeFor(available);
        QVERIFY(minimum.width() <= qRound(available.width() * 0.92));
        QVERIFY(minimum.width() >= dialog->pageList()->parentWidget()->width() + qRound(460 * k));
        dialog->setMinimumSize(minimum);
        for (const QSize& size : {initial, QSize(minimum.width(), initial.height())}) {
            dialog->resize(size);
            for (int page = 0; page < dialog->pageList()->count(); ++page) {
                dialog->showTab(static_cast<SettingsDialog::Tab>(page));
                QApplication::processEvents();
                QApplication::processEvents();
                QScrollArea* area  = dialog->pageArea(static_cast<SettingsDialog::Tab>(page));
                const int    right = area->viewport()->width();
                QVERIFY2(!area->horizontalScrollBar()->isVisible(), qPrintable(QStringLiteral("page %1 at %2 px scrolls sideways").arg(page).arg(size.width())));
                for (QWidget* w : area->widget()->findChildren<QWidget*>()) {
                    if (!w->isVisible())
                        continue;
                    const int r = w->mapTo(area->viewport(), QPoint(w->width(), 0)).x();
                    QVERIFY2(r <= right + 1, qPrintable(QStringLiteral("page %1 at %2 px: %3 \"%4\" ends at %5 of %6")
                                                            .arg(page)
                                                            .arg(size.width())
                                                            .arg(QString::fromLatin1(w->metaObject()->className()), w->accessibleName())
                                                            .arg(r)
                                                            .arg(right)));
                }
                // Live test 2.7 (F3-1): no combo box cuts its items short (the style's edit field holds the longest one).
                for (QComboBox* combo : area->widget()->findChildren<QComboBox*>()) {
                    if (!combo->isVisible() || combo->isEditable())
                        continue;
                    QStyleOptionComboBox opt;
                    opt.initFrom(combo);
                    opt.frame    = combo->hasFrame();
                    opt.iconSize = combo->iconSize();
                    const QRect field = combo->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, combo);
                    for (int i = 0; i < combo->count(); ++i) {
                        const int need = combo->fontMetrics().horizontalAdvance(combo->itemText(i)) + 2
                                         + (combo->itemIcon(i).isNull() ? 0 : combo->iconSize().width() + 4);
                        QVERIFY2(need <= field.width(), qPrintable(QStringLiteral("page %1 at %2 px: \"%3\" needs %4 px, the field has %5")
                                                                      .arg(page)
                                                                      .arg(size.width())
                                                                      .arg(combo->itemText(i))
                                                                      .arg(need)
                                                                      .arg(field.width())));
                    }
                }
                // Helper texts aren't squeezed into a narrow column: at most 4 lines at the initial size, or what their
                // length needs (the test platform's font is far wider than TeamSpeak's; with Windows' own every helper
                // takes 1 to 3 lines, see the rendered pages). A wide field turns its row stacked instead (fieldRowStacks).
                if (size == initial) {
                    for (QLabel* hint : area->widget()->findChildren<QLabel*>()) {
                        if (!hint->isVisible() || hint->property("role").toString() != a("hint"))
                            continue;
                        const int lines = qRound(hint->height() / static_cast<qreal>(hint->fontMetrics().lineSpacing()));
                        const int chars = hint->text().size();
                        // (lines the text needs at that width, plus what word wrapping loses)
                        QVERIFY2(lines <= qMax(4, chars * hint->fontMetrics().averageCharWidth() / qMax(1, hint->width()) + 2),
                                 qPrintable(QStringLiteral("page %1: %2 lines of \"%3\" in %4 px").arg(page).arg(lines).arg(hint->text().left(40)).arg(hint->width())));
                    }
                }
            }
        }
        // Sizes follow k: the switch, a row's padding, the sidebar item.
        QCheckBox* loop = switchNamed(dialog, a("Loo&p videos"));
        QVERIFY(loop);
        QVERIFY(loop->height() >= qRound(18 * k));
        QVERIFY(SettingsSection::rowOf(loop)->layout()->contentsMargins().left() == qRound(14 * k));
        QVERIFY(dialog->pageList()->sizeHintForRow(0) >= qRound(34 * k));
        delete dialog;
    }

    // Merge of dark mode and live test F3-1: every combo box still holds its longest item at 200 % after dark mode's
    // application sheet came and went while the window was open (applyTheme fits them on the dialog's StyleChange).
    void combosFitAcrossDarkMode()
    {
        const int  fontId     = QFontDatabase::addApplicationFont(a("C:/Windows/Fonts/tahoma.ttf"));
        const auto removeFont = qScopeGuard([fontId] {
            if (fontId >= 0)
                QFontDatabase::removeApplicationFont(fontId);
        });
        if (fontId < 0)
            QSKIP("Tahoma, TeamSpeak's dialog font, isn't installed");
        QFont font(QFontDatabase::applicationFontFamilies(fontId).value(0));
        font.setPointSizeF(22.0 * 72.0 / QGuiApplication::primaryScreen()->logicalDotsPerInchY());
        QApplication::setFont(font);
        const QString sheet = qApp->styleSheet();
        SettingsDialog* dialog = buildDialog();
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        const auto cut = [dialog]() {
            QStringList out;
            for (int page = 0; page < dialog->pageList()->count(); ++page) {
                dialog->showTab(static_cast<SettingsDialog::Tab>(page));
                QApplication::processEvents();
                QApplication::processEvents();
                for (QComboBox* combo : dialog->pageArea(static_cast<SettingsDialog::Tab>(page))->widget()->findChildren<QComboBox*>()) {
                    if (!combo->isVisible() || combo->isEditable())
                        continue;
                    QStyleOptionComboBox opt;
                    opt.initFrom(combo);
                    opt.frame    = combo->hasFrame();
                    opt.iconSize = combo->iconSize();
                    const QRect field = combo->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, combo);
                    for (int i = 0; i < combo->count(); ++i) {
                        const int need = combo->fontMetrics().horizontalAdvance(combo->itemText(i)) + 2
                                         + (combo->itemIcon(i).isNull() ? 0 : combo->iconSize().width() + 4);
                        if (need > field.width())
                            out << QStringLiteral("page %1: \"%2\" needs %3 px, the field has %4").arg(page).arg(combo->itemText(i)).arg(need).arg(field.width());
                    }
                }
            }
            return out;
        };
        QStringList failures = cut();
        QVERIFY2(failures.isEmpty(), qPrintable(QStringLiteral("light: ") + failures.join(QStringLiteral("; "))));
        {
            DarkMode mode;
            mode.setGlyphBase(m_dir->path());
            mode.setEnabled(true);
            QVERIFY(DarkMode::isActive());
            failures = cut();
            QVERIFY2(failures.isEmpty(), qPrintable(QStringLiteral("dark: ") + failures.join(QStringLiteral("; "))));
            mode.setEnabled(false);
        }
        QVERIFY(!DarkMode::isActive());
        QCOMPARE(qApp->styleSheet(), sheet);
        failures = cut();
        QVERIFY2(failures.isEmpty(), qPrintable(QStringLiteral("light again: ") + failures.join(QStringLiteral("; "))));
        delete dialog;
    }

    // Review fixes: "Play GIFs automatically" keeps its 2.6 tool tip and a non-empty description for screen readers;
    // the Privacy page's link button keeps its own border and padding with the focus (a dark skin's focus ring for
    // buttons comes later in the sheet and would make it taller).
    void gifToolTipAndLinkFocus()
    {
        SettingsDialog* dialog = buildDialog();
        QCheckBox*      gifs   = switchNamed(dialog, a("Play &GIFs automatically"));
        QVERIFY(gifs);
        QCOMPARE(gifs->toolTip(), a("When this is off, or animations are turned off in Windows, GIFs play only while the pointer is over them."));
        QVERIFY(!gifs->accessibleDescription().isEmpty());
        const QString sheet = dialog->styleSheet();
        const QRegularExpression focus(a(R"(QPushButton\[tsmediaLink="true"\]:focus\{color:#[0-9a-f]{6};border:1px solid #[0-9a-f]{6};padding:0px \d+px;\})"));
        QVERIFY2(sheet.contains(focus), qPrintable(sheet));
        delete dialog;
    }

    // ---- Restore defaults fills the form and never touches the chat text size ---------------------------------------
    void restoreDefaultsLeavesTheZoom()
    {
        Settings& s     = Settings::instance();
        s.chatZoom      = 30;
        s.chatLineColor = a("pink");
        s.loopVideos    = true;
        s.autoLoadLinkPreviewsOnlyFriends = true;
        FakeChat chat;
        chat.tenths            = 30;
        SettingsDialog* dialog = buildDialog(chat.hooks());
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        auto* text = dialog->findChild<ChatTextSection*>();
        QVERIFY(text);
        QCOMPARE(text->colorBox()->currentText(), a("Pink"));
        QCOMPARE(text->sizeLabel()->text(), a("136%"));
        standardButton(dialog, QDialogButtonBox::RestoreDefaults)->click();
        QCOMPARE(text->colorBox()->currentText(), a("Automatic"));
        QVERIFY(!switchNamed(dialog, a("Only from &friends"))->isChecked());
        QVERIFY(chat.set.isEmpty());
        QVERIFY(standardButton(dialog, QDialogButtonBox::Apply)->isEnabled());
        standardButton(dialog, QDialogButtonBox::Apply)->click();
        QCOMPARE(s.chatZoom, 30); // the chat's, never the form's
        QCOMPARE(s.chatLineColor, QString());
        QVERIFY(!s.loopVideos);
        QVERIFY(!s.autoLoadLinkPreviewsOnlyFriends);
        // A zoom changed by the chat while the window is open is never written back by Apply.
        s.chatZoom = 50;
        switchNamed(dialog, a("Loo&p videos"))->click();
        standardButton(dialog, QDialogButtonBox::Apply)->click();
        QCOMPARE(s.chatZoom, 50);
        QVERIFY(s.loopVideos);
        // The zoom buttons act at once (no Apply).
        text->largerButton()->click();
        QCOMPARE(chat.set, QList<int>({40}));
        QVERIFY(!standardButton(dialog, QDialogButtonBox::Apply)->isEnabled());
        delete dialog;
    }

    // ---- source rules (the CRASH RULE: nothing built from QStringLiteral that Qt keeps after an unload) ----------------
    void sourceRules()
    {
        for (const char* file : {"src/settingsdialog.cpp", "src/settingstheme.cpp", "src/settingswidgets.cpp", "src/chattextsection.cpp",
                                 "src/settingssection.cpp", "src/privacysection.cpp"}) {
            const QString text = sourceText(a(file));
            QVERIFY2(!text.isEmpty(), file);
            QVERIFY2(!text.contains(QRegularExpression(a("QStringLiteral\\s*\\("))), file);
            QVERIFY2(!text.contains(QRegularExpression(a("QTimer::singleShot\\s*\\("))), file); // owned timers only
        }
        // The style sheet is scoped to the dialog and always set (design_settings 3.6).
        const QString dialog = sourceText(a("src/settingsdialog.cpp"));
        QVERIFY(dialog.contains(a("QString::fromLatin1(\"#tsmediaSettingsDialog\")")));
        QVERIFY(!dialog.contains(a("tsmediaSettingsTabs")));
        // plugin.cpp builds Text size and colors and opens About for updates.
        const QString plugin = sourceText(a("src/plugin.cpp"));
        QVERIFY(plugin.contains(a("showTab(SettingsDialog::Tab::About)")));
    }

    // The Join (design 8.2 J1, J2, J3, J5): showSettings fills all four hooks from the chat and connects its zoom signal;
    // the text spoilers' bars take the chat's effective colours. Without these the row is dead and the suite stays green.
    void joinRules()
    {
        const QString plugin = sourceText(a("src/plugin.cpp"));
        const int     from   = plugin.indexOf(a("void showSettings(QWidget* parent)"));
        const int     to     = plugin.indexOf(a("\nvoid showDiagnostics("), from);
        QVERIFY(from >= 0 && to > from);
        const QString show = plugin.mid(from, to - from);
        for (const char* needle : {"hooks.zoomTenths", "hooks.zoomPercent", "hooks.setZoomTenths", "hooks.linePreview", "g_chat->chatZoom()",
                                   "g_chat->chatZoomPercent()", "g_chat->setChatZoom(tenths)", "g_chat->visibleChatColors()",
                                   "chatcolors::lineColors(chat, choice)", "new ChatTextSection(dialog, std::move(hooks))",
                                   "addSection(SettingsDialog::Tab::Chat, section)",
                                   "connect(g_chat, &ChatIntegration::chatZoomChanged, section, &ChatTextSection::refreshZoom)"})
            QVERIFY2(show.contains(a(needle)), needle);
        QVERIFY(!show.contains(a("ChatTextHooks{}")));
        QVERIFY(!show.contains(a("ChatTextHooks()")));

        const QString chat  = sourceText(a("src/chatintegration.cpp"));
        const int     begin = chat.indexOf(a("ChatTextSpoilers::Host host;"));
        const int     made  = chat.indexOf(a("new ChatTextSpoilers(host, this)"), begin);
        QVERIFY(begin >= 0 && made > begin);
        const QString block = chat.mid(begin, made - begin);
        QVERIFY(block.contains(a("host.colors")));
        QVERIFY(block.contains(a("chatColorsOf(browser)")));
        QVERIFY(!block.contains(a("paletteColors")));
    }
};

TSMEDIA_REGISTER_TEST(TestSettingsUi)
#include "tst_settingsui.moc"
