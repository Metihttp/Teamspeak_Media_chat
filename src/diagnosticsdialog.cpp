#include "diagnosticsdialog.h"

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTimer>
#include <QVBoxLayout>

#include "i18n.h"
#include "uiutil.h"
#include "version.h"

// Style sheets and the clipboard text are built at run time (QString::fromLatin1, i18n::t, arg()),
// never from QStringLiteral data: TeamSpeak's QStyleSheetStyle and the clipboard keep them after the
// plugin DLL is unloaded (see settings.cpp).

namespace {

diag::DialogOpener g_opener = nullptr; // GUI thread only

// The blurple of the plugin (Discord's accent): white text on it has 4.6:1.
const char* const kAccent        = "#5865f2";
const char* const kAccentHover   = "#4752c4";
const char* const kAccentPressed = "#3c45a5";

bool isDark(const QPalette& palette)
{
    return palette.color(QPalette::WindowText).lightness() > 170 || palette.color(QPalette::Window).lightness() < 128;
}

// The text colour moved toward the background, as far as start allows while it keeps minContrast
// (the settings window uses the same rule for its helper text).
QColor mutedText(const QPalette& palette, double start, double minContrast)
{
    const QColor text       = palette.color(QPalette::Active, QPalette::WindowText);
    const QColor background = palette.color(QPalette::Active, QPalette::Window);
    for (int step = qRound(start * 20); step > 0; --step) {
        const double t = step / 20.0;
        const QColor color(qRound(text.red() + (background.red() - text.red()) * t), qRound(text.green() + (background.green() - text.green()) * t),
                           qRound(text.blue() + (background.blue() - text.blue()) * t));
        if (ui::contrastRatio(color, background) >= minContrast)
            return color;
    }
    return text;
}

// A link colour with at least 4.5:1 on the window, else the text colour.
QColor linkColor(const QPalette& palette)
{
    const QColor background = palette.color(QPalette::Active, QPalette::Window);
    const QColor link       = isDark(palette) ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4);
    return ui::contrastRatio(link, background) >= 4.5 ? link : palette.color(QPalette::Active, QPalette::WindowText);
}

QLabel* hint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setProperty("role", QString::fromLatin1("hint"));
    return label;
}

} // namespace

// ============================================================================================
// DiagnosticsDialog
// ============================================================================================

DiagnosticsDialog::DiagnosticsDialog(const diag::Environment& env, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QString::fromLatin1("tsmediaDiagnosticsDialog")); // closed at plugin shutdown; scopes the style sheet
    setWindowTitle(i18n::t(TSMEDIA_NAME " — Diagnostic info"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setLayoutDirection(Qt::LeftToRight); // also when TeamSpeak itself runs right-to-left
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    m_pool.setMaxThreadCount(1);

    auto* heading     = new QLabel(i18n::t("Diagnostic info"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    if (headingFont.pointSizeF() > 0)
        headingFont.setPointSizeF(headingFont.pointSizeF() * 1.15);
    else
        headingFont.setPixelSize(qRound(headingFont.pixelSize() * 1.15));
    heading->setFont(headingFont);
    heading->setTextFormat(Qt::PlainText);

    auto* intro = hint(i18n::t("Paste this into a bug report so the problem can be found faster. It lists versions, settings and recent plugin "
                               "messages. Server addresses, server and channel names, and nicknames are never included."),
                       this);
    intro->setMaximumWidth(intro->fontMetrics().averageCharWidth() * 80); // about 75 characters a line
    auto* introRow = new QHBoxLayout;
    introRow->addWidget(intro, 1);
    introRow->addStretch();

    // Exactly what Copy puts on the clipboard: monospace, not wrapped, selectable.
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setTabChangesFocus(true);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setAccessibleName(i18n::t("Diagnostic info text"));
    m_text->setMinimumHeight(m_text->fontMetrics().lineSpacing() * 18 + 2 * m_text->frameWidth() + 8);

    m_includeNames = new QCheckBox(i18n::t("Include &file names"), this);
    m_namesHint    = hint(i18n::t("Anyone who can read the report will see the names of files sent in your chats."), this);
    m_namesHint->hide();
    m_includeNames->setAccessibleDescription(i18n::t("Server names, addresses and nicknames are never included."));

    // Feedback that doesn't take the focus; one line is kept free for it so the window doesn't jump.
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(m_status->fontMetrics().lineSpacing());
    m_status->setProperty("role", QString::fromLatin1("status"));
    m_statusTimer = new QTimer(this);
    m_statusTimer->setSingleShot(true);
    connect(m_statusTimer, &QTimer::timeout, this, [this] { showStatus(QString(), false); });

    m_copy = new QPushButton(i18n::t("&Copy"), this);
    m_copy->setObjectName(QString::fromLatin1("diagCopy"));
    m_copy->setDefault(true); // Enter copies
    m_copy->setToolTip(i18n::t("Copy the text above to the clipboard"));
    m_copyAndOpen = new QPushButton(i18n::t("Copy and &open bug report"), this);
    m_copyAndOpen->setObjectName(QString::fromLatin1("diagSecondary"));
    m_copyAndOpen->setAutoDefault(false);
    m_copyAndOpen->setToolTip(i18n::t("Also opens the bug report form on GitHub in your browser, with the versions filled in"));
    m_close = new QPushButton(i18n::t("Close"), this);
    m_close->setObjectName(QString::fromLatin1("diagSecondary"));
    m_close->setAutoDefault(false);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_copy);
    buttons->addWidget(m_copyAndOpen);
    buttons->addWidget(m_close);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    auto* header = new QVBoxLayout;
    header->setSpacing(2);
    header->addWidget(heading);
    header->addLayout(introRow);
    layout->addLayout(header);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_includeNames);
    layout->addWidget(m_namesHint);
    layout->addWidget(m_status);
    layout->addLayout(buttons);

    connect(m_copy, &QPushButton::clicked, this, [this] { copy(); });
    connect(m_copyAndOpen, &QPushButton::clicked, this, [this] { copyAndOpen(); });
    connect(m_close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_includeNames, &QCheckBox::toggled, this, [this](bool on) {
        m_namesHint->setVisible(on);
        showStatus(QString(), false); // a "Copied" was about the other text
        render();
    });

    // TeamSpeak's dark skins switch off every focus indicator: like the settings window, show focus
    // rings once the keyboard has been used to move around.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (m_keyboardFocus || !now || now->window() != this || !testAttribute(Qt::WA_KeyboardFocusChange))
            return;
        m_keyboardFocus = true;
        applyTheme();
    });

    // Collecting: TeamSpeak and settings here, the slow parts (codecs, registry, cache, log) on the
    // worker. Usually done in well under a second.
    m_text->setPlainText(i18n::t("Collecting…"));
    m_copy->setEnabled(false);
    m_copyAndOpen->setEnabled(false);
    diag::WorkerInput input;
    auto              facts = std::make_shared<diag::Facts>(diag::collectOnGui(env, &input));
    m_pool.start([this, facts, input] {
        diag::collectOnWorker(facts.get(), input);
        // The destructor waits for this task, so the dialog still exists; a queued call to a dialog
        // deleted meanwhile is dropped by Qt.
        QMetaObject::invokeMethod(this, [this, facts] { onCollected(*facts); }, Qt::QueuedConnection);
    });

    resize(qMax(sizeHint().width(), 680), qMax(sizeHint().height(), 560));
    setMinimumSize(640, 520);
    m_ready = true;
    applyTheme();
}

DiagnosticsDialog::~DiagnosticsDialog()
{
    m_pool.waitForDone(); // no plugin code may run after the DLL is unloaded
}

void DiagnosticsDialog::onCollected(const diag::Facts& facts)
{
    m_facts = std::make_unique<diag::Facts>(facts);
    render();
    m_copy->setEnabled(true);
    m_copyAndOpen->setEnabled(true);
    if (m_text->hasFocus() || !focusWidget() || !focusWidget()->isEnabled())
        m_copy->setFocus(Qt::OtherFocusReason);
}

void DiagnosticsDialog::render()
{
    if (!m_facts)
        return;
    m_report               = diag::format(*m_facts, m_includeNames->isChecked(), QDir::homePath());
    const int vertical     = m_text->verticalScrollBar()->value();
    const int horizontal   = m_text->horizontalScrollBar()->value();
    m_text->setPlainText(m_report);
    m_text->verticalScrollBar()->setValue(vertical);
    m_text->horizontalScrollBar()->setValue(horizontal);
}

bool DiagnosticsDialog::copy()
{
    if (m_report.isEmpty())
        return false;
    // An owned copy: the clipboard keeps its data after the plugin is unloaded.
    const QString text(m_report.constData(), m_report.size());
    QClipboard*   clipboard = QGuiApplication::clipboard();
    clipboard->setText(text);
    if (clipboard->text() != text) { // another program held the clipboard
        showStatus(i18n::t("Couldn't copy to the clipboard. Try again."), true);
        return false;
    }
    showStatus(i18n::t("Copied to the clipboard. Paste it into the “Diagnostic info” box of the bug report."), false);
    return true;
}

void DiagnosticsDialog::copyAndOpen()
{
    if (!m_facts || !copy())
        return;
    // The browser opens the form; the plugin itself makes no web request.
    if (QDesktopServices::openUrl(diag::bugReportUrl(*m_facts)))
        showStatus(i18n::t("Copied. Paste it into the “Diagnostic info” box of the bug report that opened in your browser."), false);
    else
        showStatus(i18n::t("Copied, but the browser couldn't be opened. Open the bug report from the plugin's GitHub page."), true);
}

void DiagnosticsDialog::showStatus(const QString& text, bool error)
{
    m_statusTimer->stop();
    m_status->setProperty("role", QString::fromLatin1(error ? "error" : "status"));
    m_status->setText(text);
    applyTheme();
    if (!text.isEmpty()) {
        // Announced by screen readers without moving the focus.
        QAccessibleEvent event(m_status, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
        if (!error) // errors stay until the next try
            m_statusTimer->start(qMax(2500, ui::notificationDurationMs()));
    }
}

void DiagnosticsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (m_ready && (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange))
        applyTheme(); // also when TeamSpeak's skin changes while the window is open
}

void DiagnosticsDialog::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    ensurePolished();

    const QPalette pal        = palette();
    const bool     dark       = isDark(pal);
    const QColor   background = pal.color(QPalette::Active, QPalette::Window);
    const QColor   text       = pal.color(QPalette::Active, QPalette::WindowText);
    const QColor   muted      = mutedText(pal, 0.4, 4.6);
    QColor         error      = dark ? QColor(0xFF, 0x99, 0xA4) : QColor(0xC4, 0x2B, 0x1C);
    if (ui::contrastRatio(error, background) < 4.5)
        error = text;

    for (QLabel* label : findChildren<QLabel*>()) {
        const QString role = label->property("role").toString();
        if (role.isEmpty())
            continue;
        const QColor color        = role == QLatin1String("error") ? error : role == QLatin1String("hint") ? muted : text;
        QPalette     labelPalette = label->palette();
        labelPalette.setColor(QPalette::Active, QPalette::WindowText, color);
        labelPalette.setColor(QPalette::Inactive, QPalette::WindowText, color);
        label->setPalette(labelPalette);
    }

    // Copy is the one primary button (the accent); the others keep the style's look, or are outlined
    // in TeamSpeak's dark skins, which paint every button in the accent.
    const QString ring = dark ? QString::fromLatin1("#ffffff") : QString::fromLatin1("#1e1f22");
    QString       sheet = QString::fromLatin1("#tsmediaDiagnosticsDialog QPushButton#diagCopy{background:%1;color:#ffffff;border:2px solid %1;"
                                              "border-radius:3px;padding:2px 16px;font-weight:bold;}"
                                              "#tsmediaDiagnosticsDialog QPushButton#diagCopy:hover{background:%2;border-color:%2;}"
                                              "#tsmediaDiagnosticsDialog QPushButton#diagCopy:pressed{background:%3;border-color:%3;}"
                                              "#tsmediaDiagnosticsDialog QPushButton#diagCopy:disabled{background:%4;border-color:%4;color:%5;}")
                        .arg(QLatin1String(kAccent), QLatin1String(kAccentHover), QLatin1String(kAccentPressed),
                             pal.color(QPalette::Disabled, QPalette::Button).name(), mutedText(pal, 0.6, 3.0).name());
    if (m_keyboardFocus)
        sheet += QString::fromLatin1("#tsmediaDiagnosticsDialog QPushButton#diagCopy:focus{border-color:%1;}").arg(ring);
    if (!qApp->styleSheet().isEmpty()) {
        // TeamSpeak's skins colour labels by style sheet, which beats a palette.
        sheet += QString::fromLatin1("#tsmediaDiagnosticsDialog QLabel[role=\"hint\"]{color:%1;}"
                                     "#tsmediaDiagnosticsDialog QLabel[role=\"error\"]{color:%2;}")
                     .arg(muted.name(), error.name());
        if (dark) {
            sheet += QString::fromLatin1("#tsmediaDiagnosticsDialog QPushButton#diagSecondary{background:transparent;color:%1;border:1px solid %2;"
                                         "border-radius:3px;padding:6px 14px;}"
                                         "#tsmediaDiagnosticsDialog QPushButton#diagSecondary:hover{background:rgba(255,255,255,20);}"
                                         "#tsmediaDiagnosticsDialog QPushButton#diagSecondary:disabled{color:%3;}")
                         .arg(text.name(), muted.name(), mutedText(pal, 0.6, 3.0).name());
            if (m_keyboardFocus) {
                sheet += QString::fromLatin1("#tsmediaDiagnosticsDialog QPushButton#diagSecondary:focus{border:2px solid %1;padding:5px 13px;}"
                                             "#tsmediaDiagnosticsDialog QPlainTextEdit:focus{border:1px solid %1;}"
                                             "#tsmediaDiagnosticsDialog QCheckBox:focus{outline:1px solid %1;}")
                             .arg(ring);
            }
        }
    }
    if (sheet != styleSheet())
        setStyleSheet(sheet);
    // The primary button is as tall as its neighbours, whatever the style draws them with.
    m_copy->setMinimumHeight(m_close->sizeHint().height());

    // The hint under the checkbox lines up with its text.
    QStyleOptionButton option;
    option.initFrom(m_includeNames);
    option.rect      = QRect(QPoint(), m_includeNames->sizeHint());
    const int indent = qMax(0, m_includeNames->style()->subElementRect(QStyle::SE_CheckBoxContents, &option, m_includeNames).left());
    m_namesHint->setContentsMargins(indent, 0, 0, 0);

    m_applyingTheme = false;
}

// ============================================================================================
// DiagnosticsButton
// ============================================================================================

DiagnosticsButton::DiagnosticsButton(QWidget* parent)
    : QPushButton(i18n::t("Diagnostic info…"), parent) // no access key: the settings window uses all of its letters
{
    setObjectName(QString::fromLatin1("diagLinkButton"));
    setToolTip(i18n::t("Versions, settings and recent messages to paste into a bug report"));
    setAccessibleDescription(toolTip());
    setCursor(Qt::PointingHandCursor);
    setAutoDefault(false);  // Enter stays OK in the settings window
    setFocusPolicy(Qt::TabFocus); // a click doesn't leave a focus ring behind
    setHidden(!diag::hasDialogOpener());
    connect(this, &QPushButton::clicked, this, [this] { diag::openDialog(window()); });
    applyTheme();
}

void DiagnosticsButton::changeEvent(QEvent* event)
{
    QPushButton::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange)
        applyTheme();
}

void DiagnosticsButton::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    // Looks like a link in every skin: no button face, the link colour, underlined on hover, and a
    // ring around it when it has the keyboard focus.
    const QPalette pal   = window() ? window()->palette() : palette();
    const QString  sheet = QString::fromLatin1("QPushButton#diagLinkButton{background:transparent;border:1px solid transparent;border-radius:3px;"
                                               "padding:0 4px;margin:0;color:%1;font-weight:normal;min-width:0;min-height:0;}"
                                               "QPushButton#diagLinkButton:hover{text-decoration:underline;}"
                                               "QPushButton#diagLinkButton:pressed{color:%2;}"
                                               "QPushButton#diagLinkButton:focus{border-color:%2;}")
                               .arg(linkColor(pal).name(), pal.color(QPalette::Active, QPalette::WindowText).name());
    if (sheet != styleSheet())
        setStyleSheet(sheet);
    m_applyingTheme = false;
}

// ============================================================================================
// Opener
// ============================================================================================

namespace diag {

void setDialogOpener(DialogOpener opener)
{
    g_opener = opener;
}

bool hasDialogOpener()
{
    return g_opener != nullptr;
}

bool openDialog(QWidget* parent)
{
    if (!g_opener)
        return false;
    g_opener(parent);
    return true;
}

} // namespace diag
