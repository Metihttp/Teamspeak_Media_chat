#include "updatedialog.h"

#include <QAccessible>
#include <QApplication>
#include <QBasicTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTextLayout>
#include <QTimerEvent>
#include <QVBoxLayout>

#include "i18n.h"
#include "medialink.h" // formatSize, formatProgress
#include "uiutil.h"
#include "updatemanifest.h" // kMaxNotes

namespace upd {

// Thin rounded progress line (TeamSpeak's dark skin doesn't style QProgressBar). Determinate, or
// busy: a moving segment, or a still track while Windows animations are off.
class ProgressLine : public QWidget
{
  public:
    explicit ProgressLine(QWidget* parent)
        : QWidget(parent)
    {
        setFixedHeight(4);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setProgress(double value)
    {
        m_busy = false;
        m_timer.stop();
        m_value = qBound(0.0, value, 1.0);
        update();
    }

    void setBusy()
    {
        m_busy  = true;
        m_phase = 0.0;
        if (isVisible() && ui::animationsEnabled())
            m_timer.start(30, this);
        update();
    }

    void setColors(const QColor& track, const QColor& fill)
    {
        m_track = track;
        m_fill  = fill;
        update();
    }

  protected:
    void showEvent(QShowEvent*) override
    {
        if (m_busy && ui::animationsEnabled())
            m_timer.start(30, this);
    }
    void hideEvent(QHideEvent*) override { m_timer.stop(); }
    void timerEvent(QTimerEvent* event) override
    {
        if (event->timerId() != m_timer.timerId())
            return QWidget::timerEvent(event);
        if (!ui::animationsEnabled())
            m_timer.stop();
        m_phase += 0.018;
        if (m_phase > 1.0)
            m_phase -= 1.0;
        update();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const QRectF r(rect());
        const qreal  radius = r.height() / 2.0;
        p.setBrush(m_track);
        p.drawRoundedRect(r, radius, radius);
        QRectF fill;
        if (m_busy) {
            if (!m_timer.isActive())
                return;
            const qreal segment = r.width() * 0.3;
            fill                = QRectF(-segment + (r.width() + segment) * m_phase, 0, segment, r.height()).intersected(r);
        } else {
            fill = QRectF(0, 0, r.width() * m_value, r.height());
        }
        if (fill.width() <= 0.0)
            return;
        p.setBrush(m_fill);
        p.drawRoundedRect(fill, radius, radius);
    }

  private:
    double      m_value = 0.0;
    double      m_phase = 0.0;
    bool        m_busy  = false;
    QColor      m_track = QColor(0xe3, 0xe5, 0xe8);
    QColor      m_fill  = QColor(0x58, 0x65, 0xf2);
    QBasicTimer m_timer;
};

namespace {

constexpr int kNoteLines = 2;

bool isDark(const QPalette& palette)
{
    return palette.color(QPalette::WindowText).lightness() > 170 || palette.color(QPalette::Window).lightness() < 128;
}

QLabel* label(const QString& text, QWidget* parent, const char* role = nullptr)
{
    auto* l = new QLabel(text, parent);
    l->setTextFormat(Qt::PlainText);
    l->setWordWrap(true);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    if (role)
        l->setProperty("role", QString::fromLatin1(role));
    return l;
}

void makeHeading(QLabel* heading)
{
    QFont font = heading->font();
    font.setBold(true);
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() * 1.15);
    else
        font.setPixelSize(qRound(font.pixelSize() * 1.15));
    heading->setFont(font);
}

QPushButton* button(const QString& text, QWidget* parent, bool primary = false)
{
    auto* b = new QPushButton(text, parent);
    b->setAutoDefault(false);
    if (primary)
        b->setProperty("primary", true);
    return b;
}

// text cut to at most `lines` lines of `width` px, ending in "…" if something was cut.
QString elideToLines(const QString& text, const QFont& font, int width, int lines)
{
    QTextLayout layout(text, font);
    layout.beginLayout();
    int  end  = 0;
    bool more = false;
    for (int i = 0; i < lines; ++i) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        end = line.textStart() + line.textLength();
    }
    more = layout.createLine().isValid();
    layout.endLayout();
    if (!more)
        return text;
    QString cut = text.left(end).trimmed();
    // Make room for the ellipsis on the last line.
    while (!cut.isEmpty()) {
        QTextLayout check(cut + QChar(0x2026), font);
        check.beginLayout();
        int count = 0;
        while (true) {
            QTextLine line = check.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width);
            ++count;
        }
        check.endLayout();
        if (count <= lines)
            break;
        cut.chop(1);
        if (!cut.isEmpty() && cut.at(cut.size() - 1).isHighSurrogate())
            cut.chop(1);
    }
    return cut.trimmed() + QChar(0x2026);
}

} // namespace

void styleUpdateWindow(QWidget* window, ProgressLine* progress)
{
    window->ensurePolished();
    const QPalette pal        = window->palette();
    const bool     dark       = isDark(pal);
    const QColor   background = pal.color(QPalette::Active, QPalette::Window);
    QColor         muted      = dark ? QColor(0xb9, 0xbb, 0xbe) : QColor(0x5f, 0x63, 0x68);
    if (ui::contrastRatio(muted, background) < 4.5)
        muted = pal.color(QPalette::Active, QPalette::WindowText);
    QColor error = dark ? QColor(0xff, 0x99, 0xa4) : QColor(0xc4, 0x2b, 0x1c);
    if (ui::contrastRatio(error, background) < 4.5)
        error = pal.color(QPalette::Active, QPalette::WindowText); // the warning icon still marks it

    // Links: TeamSpeak's dark skins leave the palette's dark blue, which is unreadable there.
    const QColor link = dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4); // 4.6:1 on #36393e, 6.4:1 on white
    for (QLabel* l : window->findChildren<QLabel*>()) {
        const QString role = l->property("role").toString();
        if (role.isEmpty())
            continue;
        QPalette p = l->palette();
        if (role == QLatin1String("link")) {
            for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
                p.setColor(group, QPalette::Link, link);
                p.setColor(group, QPalette::LinkVisited, link);
            }
        } else {
            const QColor color = role == QLatin1String("error") ? error : muted;
            p.setColor(QPalette::Active, QPalette::WindowText, color);
            p.setColor(QPalette::Inactive, QPalette::WindowText, color);
        }
        l->setPalette(p);
    }
    if (progress) // passed in: ProgressLine has no meta-object, so findChildren<ProgressLine*> would match every widget
        progress->setColors(dark ? QColor(0x4e, 0x50, 0x58) : QColor(0xe3, 0xe5, 0xe8), dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x58, 0x65, 0xf2));

    // Built with fromLatin1, never QStringLiteral: TeamSpeak's QStyleSheetStyle keeps the parsed text
    // after the plugin is unloaded (see settings.cpp). Scoped to this window.
    const QString id      = QLatin1Char('#') + window->objectName();
    const QString ring    = dark ? QString::fromLatin1("#ffffff") : QString::fromLatin1("#1f2328");
    const bool    skinned = !qApp->styleSheet().isEmpty();
    QString       sheet;
    if (skinned) {
        // TeamSpeak's skins paint every button in the accent colour and switch off focus indicators. Here
        // the one primary action keeps the accent, the others are grey, the flat one is text only, and
        // the focused button gets a ring. Same 8 x 14 px box as the skin's buttons (1 px of it is border).
        sheet = QString::fromLatin1(
                    "%1 QPushButton{background-color:#4f545c;color:#ffffff;border:1px solid #4f545c;border-radius:3px;padding:7px 13px;}"
                    "%1 QPushButton:hover{background-color:#5d6269;border-color:#5d6269;}"
                    "%1 QPushButton:pressed{background-color:#454950;border-color:#454950;}"
                    "%1 QPushButton:disabled{background-color:#3f4248;border-color:#3f4248;color:#a3a6aa;}"
                    "%1 QPushButton[flat=\"true\"]{background-color:transparent;border-color:transparent;color:%3;}"
                    "%1 QPushButton[flat=\"true\"]:hover{background-color:#40444b;border-color:#40444b;}"
                    "%1 QPushButton[primary=\"true\"]{background-color:#5865f2;border-color:#5865f2;color:#ffffff;}"
                    "%1 QPushButton[primary=\"true\"]:hover{background-color:#4752c4;border-color:#4752c4;}"
                    "%1 QPushButton[primary=\"true\"]:pressed{background-color:#3c45a5;border-color:#3c45a5;}"
                    "%1 QPushButton:focus{border:2px solid %2;padding:6px 12px;}"
                    "%1 QLabel[role=\"hint\"]{color:%4;}%1 QLabel[role=\"error\"]{color:%5;}")
                    .arg(id, ring, pal.color(QPalette::Active, QPalette::WindowText).name(), muted.name(), error.name());
    } else {
        sheet = QString::fromLatin1(
                    "%1 QPushButton[primary=\"true\"]{background-color:#5865f2;color:#ffffff;border:1px solid #5865f2;border-radius:4px;padding:5px 16px;}"
                    "%1 QPushButton[primary=\"true\"]:hover{background-color:#4752c4;border-color:#4752c4;}"
                    "%1 QPushButton[primary=\"true\"]:pressed{background-color:#3c45a5;border-color:#3c45a5;}"
                    "%1 QPushButton[primary=\"true\"]:focus{border:2px solid %2;padding:4px 15px;}"
                    "%1 QPushButton[primary=\"true\"]:disabled{background-color:#8891f2;border-color:#8891f2;}")
                    .arg(id, ring);
    }
    if (sheet != window->styleSheet())
        window->setStyleSheet(sheet);
}

// ---- ConsentWindow -------------------------------------------------------------------------------

ConsentWindow::ConsentWindow(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QString::fromLatin1("tsmediaUpdateConsent"));
    setWindowTitle(i18n::t("TS Media chat"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setLayoutDirection(Qt::LeftToRight);
    setFixedWidth(420);

    auto* heading = label(i18n::t("Keep TS Media chat up to date?"), this);
    makeHeading(heading);
    auto* body = label(i18n::t("TS Media chat can check GitHub once a day for a new version. If there is one, it shows you what's new and asks "
                               "before installing anything."),
                       this);
    auto* hint = label(i18n::t("This is the only internet connection the plugin makes. GitHub sees your IP address, and Windows may check "
                               "GitHub's certificate with its issuer. No names, servers or chats are sent. You can change this later in "
                               "Settings → Updates."),
                       this, "hint");
    auto* no = button(i18n::t("&No thanks"), this);
    auto* on = button(i18n::t("&Turn on"), this, true);
    // Not the default: Enter does nothing until a button has the focus (the window appears unasked).
    on->setDefault(false);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(no);
    buttons->addWidget(on);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);
    layout->addWidget(heading);
    layout->addWidget(body);
    layout->addWidget(hint);
    layout->addSpacing(4);
    layout->addLayout(buttons);
    setAccessibleDescription(body->text());

    connect(on, &QPushButton::clicked, this, [this] {
        emit turnOn();
        close();
    });
    connect(no, &QPushButton::clicked, this, [this] {
        emit noThanks();
        close();
    });
    styleUpdateWindow(this);
    layout->activate();
    setFixedHeight(layout->totalHeightForWidth(width())); // wrapped text: the height its width needs, no gaps
    m_ready = true;
}

void ConsentWindow::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    // Not during the constructor: setWindowFlags() under a style sheet sends a StyleChange there.
    if (m_ready && (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange))
        styleUpdateWindow(this);
}

// ---- UpdateDialog --------------------------------------------------------------------------------

UpdateDialog::UpdateDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QString::fromLatin1("tsmediaUpdateDialog"));
    setWindowTitle(i18n::t("TS Media chat"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setLayoutDirection(Qt::LeftToRight);
    setFixedWidth(440);

    m_icon = new QLabel(this);
    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this) * 3 / 2;
    m_icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(iconSize, iconSize));
    m_icon->setAlignment(Qt::AlignTop);
    m_icon->hide();
    m_heading = label(QString(), this);
    makeHeading(m_heading);
    auto* headingRow = new QHBoxLayout;
    headingRow->setSpacing(8);
    headingRow->addWidget(m_icon, 0, Qt::AlignTop);
    headingRow->addWidget(m_heading, 1);

    m_stack = new QStackedWidget(this);

    // Available
    auto* available = new QWidget(m_stack);
    m_meta          = label(QString(), available, "hint");
    m_skippedHint   = label(QString(), available, "hint");
    m_notesTitle    = label(i18n::t("What's new"), available);
    QFont small     = m_notesTitle->font();
    small.setBold(true);
    m_notesTitle->setFont(small);
    auto* notes       = new QWidget(available);
    auto* notesLayout = new QVBoxLayout(notes);
    notesLayout->setContentsMargins(0, 0, 0, 0);
    notesLayout->setSpacing(4);
    for (int i = 0; i < kMaxNotes; ++i) {
        auto* note = label(QString(), notes);
        note->setWordWrap(true);
        notesLayout->addWidget(note);
        m_notes.append(note);
    }
    m_notesLink = new QLabel(notes);
    m_notesLink->setProperty("role", QString::fromLatin1("link"));
    m_notesLink->setTextFormat(Qt::RichText);
    m_notesLink->setOpenExternalLinks(true);
    m_notesLink->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    notesLayout->addSpacing(2);
    notesLayout->addWidget(m_notesLink);
    notesLayout->addStretch(1);
    // Room for 5 notes of 2 lines each and the link, so the dialog doesn't change size between its states.
    const int lineHeight = notes->fontMetrics().lineSpacing();
    notes->setMinimumHeight((kMaxNotes * kNoteLines + 1) * lineHeight + kMaxNotes * notesLayout->spacing() + 2);
    auto* signatureHint = label(i18n::t("Downloaded from GitHub and checked against the author's signature before anything is changed. "
                                        "The new version starts when TeamSpeak restarts."),
                                available, "hint");
    auto* availableLayout = new QVBoxLayout(available);
    availableLayout->setContentsMargins(0, 0, 0, 0);
    availableLayout->setSpacing(6);
    availableLayout->addWidget(m_meta);
    availableLayout->addWidget(m_skippedHint);
    availableLayout->addSpacing(4);
    availableLayout->addWidget(m_notesTitle);
    availableLayout->addWidget(notes, 1);
    availableLayout->addSpacing(4);
    availableLayout->addWidget(signatureHint);
    m_stack->addWidget(available);

    // Downloading / working
    auto* progress = new QWidget(m_stack);
    m_status       = label(QString(), progress);
    m_progress     = new ProgressLine(progress);
    auto* progressLayout = new QVBoxLayout(progress);
    progressLayout->setContentsMargins(0, 8, 0, 0);
    progressLayout->setSpacing(10);
    progressLayout->addWidget(m_status);
    progressLayout->addWidget(m_progress);
    progressLayout->addStretch(1);
    m_stack->addWidget(progress);

    // Installed / error
    auto* message = new QWidget(m_stack);
    m_body        = label(QString(), message);
    m_serverHint  = label(QString(), message, "hint");
    m_uploadHint  = label(QString(), message, "hint");
    auto* messageLayout = new QVBoxLayout(message);
    messageLayout->setContentsMargins(0, 0, 0, 0);
    messageLayout->setSpacing(8);
    messageLayout->addWidget(m_body);
    messageLayout->addWidget(m_serverHint);
    messageLayout->addWidget(m_uploadHint);
    messageLayout->addStretch(1);
    m_stack->addWidget(message);

    // Buttons
    m_skip = button(i18n::t("&Skip this version"), this);
    m_skip->setFlat(true);
    m_later        = button(i18n::t("&Later"), this);
    m_update       = button(i18n::t("&Update"), this, true);
    m_cancel       = button(i18n::t("&Cancel"), this);
    m_restartLater = button(i18n::t("Restart &later"), this);
    m_restartNow   = button(i18n::t("&Restart TeamSpeak now"), this, true);
    m_close        = button(i18n::t("&Close"), this);
    m_tryAgain     = button(i18n::t("&Try again"), this, true);
    m_openPage     = button(i18n::t("&Open download page"), this, true);
    m_showFolder   = button(i18n::t("&Show folder"), this, true);
    m_allButtons   = {m_skip, m_later, m_update, m_cancel, m_restartLater, m_restartNow, m_close, m_tryAgain, m_openPage, m_showFolder};
    auto* buttons  = new QHBoxLayout;
    buttons->addWidget(m_skip);
    buttons->addStretch(1);
    for (QPushButton* b : {m_later, m_update, m_cancel, m_restartLater, m_restartNow, m_close, m_tryAgain, m_openPage, m_showFolder})
        buttons->addWidget(b);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);
    layout->addLayout(headingRow);
    layout->addWidget(m_stack, 1);
    layout->addLayout(buttons);

    connect(m_update, &QPushButton::clicked, this, &UpdateDialog::updateClicked);
    connect(m_later, &QPushButton::clicked, this, &UpdateDialog::laterClicked);
    connect(m_skip, &QPushButton::clicked, this, &UpdateDialog::skipClicked);
    connect(m_cancel, &QPushButton::clicked, this, &UpdateDialog::cancelClicked);
    connect(m_restartNow, &QPushButton::clicked, this, &UpdateDialog::restartNowClicked);
    connect(m_restartLater, &QPushButton::clicked, this, &UpdateDialog::restartLaterClicked);
    connect(m_close, &QPushButton::clicked, this, &UpdateDialog::closeClicked);
    connect(m_tryAgain, &QPushButton::clicked, this, &UpdateDialog::tryAgainClicked);
    connect(m_openPage, &QPushButton::clicked, this, &UpdateDialog::openDownloadPageClicked);
    connect(m_showFolder, &QPushButton::clicked, this, &UpdateDialog::showFolderClicked);

    // The stack is as tall as its tallest page, so switching pages never changes the window's size.
    setPage(Page::Available);
    styleUpdateWindow(this, m_progress);
    m_ready = true;
    fitHeight();
}

// The link's colour is part of its markup: TeamSpeak's skins reset label palettes when they repolish,
// and leave links in a blue that is unreadable on their dark background.
void UpdateDialog::refreshLink()
{
    if (m_notesUrl.isEmpty()) {
        m_notesLink->clear();
        return;
    }
    const QString color = QString::fromLatin1(isDark(palette()) ? "#949cf7" : "#4752c4"); // 4.6:1 on #36393e, 6.4:1 on white
    // The URL is built by the updater from the validated version only; escaped anyway.
    m_notesLink->setText(QString::fromLatin1("<a href=\"%1\" style=\"color:%2;\">").arg(m_notesUrl.toHtmlEscaped(), color)
                         + i18n::t("Full release notes on GitHub").toHtmlEscaped() + QString::fromLatin1("</a>"));
}

void UpdateDialog::fitHeight()
{
    QLayout* l = layout();
    if (!l)
        return;
    l->activate();
    const int needed = l->hasHeightForWidth() ? l->totalHeightForWidth(width()) : sizeHint().height();
    if (needed > m_height) {
        m_height = needed;
        setFixedHeight(m_height);
    }
}

void UpdateDialog::setPage(Page page)
{
    m_page = page;
    m_icon->setVisible(page == Page::Error);
    switch (page) {
    case Page::Available:
        m_stack->setCurrentIndex(0);
        break;
    case Page::Downloading:
    case Page::Working:
        m_stack->setCurrentIndex(1);
        break;
    case Page::Installed:
    case Page::Error:
        m_stack->setCurrentIndex(2);
        break;
    }
}

void UpdateDialog::setButtons(std::initializer_list<QPushButton*> visible, QPushButton* primary, QPushButton* defaultButton)
{
    for (QPushButton* b : qAsConst(m_allButtons)) {
        bool shown = false;
        for (QPushButton* v : visible)
            shown = shown || v == b;
        b->setVisible(shown);
        b->setDefault(false);
        b->setEnabled(true);
        if (b->property("primary").toBool() != (b == primary)) {
            // A property selector changed: the style sheet applies again, and the button's size may change.
            b->setProperty("primary", b == primary);
            b->style()->unpolish(b);
            b->style()->polish(b);
            b->updateGeometry();
        }
    }
    if (defaultButton)
        defaultButton->setDefault(true);
}

void UpdateDialog::showAvailable(const Available& info)
{
    setPage(Page::Available);
    m_heading->setText(i18n::t("Update to %1?").arg(info.version));
    QStringList meta;
    meta << i18n::t("You have %1").arg(info.installed);
    if (info.published.isValid())
        meta << i18n::t("Released %1").arg(QLocale(QLocale::English).toString(info.published, QString::fromLatin1("MMM d, yyyy")));
    if (info.bytes > 0)
        meta << formatSize(static_cast<quint64>(info.bytes));
    m_meta->setText(meta.join(QString::fromUtf8(" · ")));
    m_skippedHint->setText(i18n::t("You skipped this version earlier."));
    m_skippedHint->setVisible(info.skipped);

    const QMargins margins = layout()->contentsMargins();
    const int      width   = this->width() - margins.left() - margins.right(); // the notes' width (fixed window width)
    m_notesTitle->setVisible(!info.notes.isEmpty());
    for (int i = 0; i < m_notes.size(); ++i) {
        QLabel* note = m_notes.at(i);
        if (i < info.notes.size()) {
            const QString text = info.notes.at(i);
            note->setText(elideToLines(QString::fromUtf8("•  ") + text, note->font(), width, kNoteLines));
            note->setToolTip(text.toHtmlEscaped()); // escaped: notes are shown literally, never as markup
            note->setAccessibleName(text);
            note->show();
        } else {
            note->clear();
            note->setToolTip(QString());
            note->hide();
        }
    }
    m_notesUrl = info.notesUrl;
    refreshLink();
    m_notesLink->setVisible(!info.notesUrl.isEmpty());
    setButtons({m_skip, m_later, m_update}, m_update, m_update);
    m_skip->setVisible(!info.skipped);
    setAccessibleDescription(m_heading->text());
    fitHeight();
}

void UpdateDialog::showDownloading(const QString& version, qint64 done, qint64 total)
{
    const bool entering = m_page != Page::Downloading;
    setPage(Page::Downloading);
    m_heading->setText(i18n::t("Updating to %1").arg(version));
    const QString status = total > 0 ? i18n::t("Downloading… %1").arg(formatProgress(static_cast<quint64>(qMax<qint64>(0, done)), static_cast<quint64>(total)))
                                     : i18n::t("Downloading… %1").arg(formatSize(static_cast<quint64>(qMax<qint64>(0, done))));
    m_status->setText(status);
    m_progress->setProgress(total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0);
    m_progress->show();
    m_status->setAccessibleDescription(status);
    if (entering) {
        setButtons({m_cancel}, nullptr, nullptr);
        setAccessibleDescription(status);
        fitHeight();
    }
}

void UpdateDialog::showWorking(const QString& version, const QString& status)
{
    setPage(Page::Working);
    m_heading->setText(i18n::t("Updating to %1").arg(version));
    m_status->setText(status);
    m_progress->setBusy();
    m_progress->show();
    setButtons({m_cancel}, nullptr, nullptr);
    m_cancel->setEnabled(false); // installing takes milliseconds and can't be interrupted halfway
    setAccessibleDescription(status);
    fitHeight();
}

void UpdateDialog::showInstalled(const QString& version, int connectedServers, int runningUploads)
{
    setPage(Page::Installed);
    m_heading->setText(i18n::t("TS Media chat %1 is installed").arg(version));
    m_body->setText(i18n::t("Restart TeamSpeak to start using it."));
    m_serverHint->setText(connectedServers == 1 ? i18n::t("You'll be disconnected from 1 server for a moment.")
                                                : i18n::t("You'll be disconnected from %1 servers for a moment.").arg(connectedServers));
    m_serverHint->setVisible(connectedServers > 0);
    m_uploadHint->setText(runningUploads == 1 ? i18n::t("1 upload still running will be canceled.")
                                              : i18n::t("%1 uploads still running will be canceled.").arg(runningUploads));
    m_uploadHint->setVisible(runningUploads > 0);
    // While connected, Enter must not drop the voice call: Restart later is the default then.
    setButtons({m_restartLater, m_restartNow}, m_restartNow, connectedServers > 0 ? m_restartLater : m_restartNow);
    setAccessibleDescription(m_heading->text());
    styleUpdateWindow(this, m_progress);
    fitHeight();
}

void UpdateDialog::showError(const Error& error)
{
    setPage(Page::Error);
    m_heading->setText(error.heading);
    m_body->setText(error.body);
    m_serverHint->hide();
    m_uploadHint->hide();
    QPushButton* action = nullptr;
    switch (error.action) {
    case ErrorAction::TryAgain:
        action = m_tryAgain;
        break;
    case ErrorAction::OpenDownloadPage:
        action = m_openPage;
        break;
    case ErrorAction::ShowFolder:
        action = m_showFolder;
        break;
    case ErrorAction::None:
        break;
    }
    if (action)
        setButtons({m_close, action}, action, m_close);
    else
        setButtons({m_close}, nullptr, m_close);
    setAccessibleDescription(error.heading + QLatin1String(". ") + error.body);
    fitHeight();
    if (isVisible()) {
        QAccessibleEvent alert(this, QAccessible::Alert);
        QAccessible::updateAccessibility(&alert);
    }
}

void UpdateDialog::reject()
{
    switch (m_page) {
    case Page::Available:
        emit laterClicked();
        break;
    case Page::Downloading:
        emit cancelClicked();
        break;
    case Page::Working:
        return; // milliseconds; nothing to cancel safely
    case Page::Installed:
        emit restartLaterClicked();
        break;
    case Page::Error:
        emit closeClicked();
        break;
    }
}

void UpdateDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    // Not during the constructor: setWindowFlags() under a style sheet sends a StyleChange there.
    if (m_ready && (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange)) {
        styleUpdateWindow(this, m_progress);
        refreshLink();
        fitHeight();
    }
}

} // namespace upd
