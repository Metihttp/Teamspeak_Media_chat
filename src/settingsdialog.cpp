#include "settingsdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "accessgroupbox.h" // 2.2 servergroup
#include "core.h"
#include "datasaver.h" // 2.2 per-server settings
#include "diagnosticsdialog.h" // 2.2 diagnostics
#include "i18n.h"
#include "medialink.h"
#include "settingssection.h"
#include "serverssection.h" // 2.2 per-server settings
#include "uiutil.h"
#include "update/updatesettingsgroup.h" // 2.2 updater
#include "version.h"

namespace {

// The tab shown last; the dialog opens on it again while TeamSpeak runs.
int g_lastTab = 0;

// Steps land on multiples of the step (100, 256, 512, … for the cache), not on the minimum plus n steps.
class SnapSpinBox : public QSpinBox
{
  public:
    using QSpinBox::QSpinBox;

    void stepBy(int steps) override
    {
        const int step = singleStep();
        int       base = value() / step * step; // the ranges are never negative
        if (steps < 0 && base != value())
            base += step; // the first step down only goes to the multiple below
        setValue(qBound(minimum(), base + steps * step, maximum()));
        selectAll();
    }
};

// The range comes from Settings, so the dialog never offers a value Settings::load() would clamp.
QSpinBox* spin(Settings::Range range, int step, const QString& suffix, QWidget* parent)
{
    auto* box = new SnapSpinBox(parent);
    box->setRange(range.min, range.max);
    box->setSingleStep(step);
    box->setSuffix(suffix);
    box->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    box->setAccelerated(true);
    return box;
}

// Spin box suffixes; formatSize() uses the same units.
QString megabytes()
{
    return i18n::t(" MB");
}

QString pixels()
{
    return i18n::t(" px");
}

// Secondary text: the description at the top and the helpers under options. applyTheme() colours it.
// (The building blocks are shared with the feature sections: settingssection.h.)
QLabel* hint(const QString& text, QWidget* parent)
{
    return SettingsSection::hint(text, parent);
}

// A tab's page. On a screen too short for it (a small or strongly scaled laptop screen) it scrolls,
// and the header, the tabs and the buttons stay in view; otherwise this is exactly as wide and as tall
// as its groups. QScrollArea's own size hint is capped at a few lines of text, so fitToContents() sets
// the height (the same for every page, so switching tabs never changes the window's size).
class PageArea : public QScrollArea
{
  public:
    explicit PageArea(QWidget* parent)
        : QScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFocusPolicy(Qt::NoFocus); // no Tab stop of its own (Tab still scrolls the focused field into view)
        viewport()->setAutoFillBackground(false); // the dialog's background, also in TeamSpeak's skins
    }

    // The height the page gets, and whether that is less than it needs.
    void setHeight(int height, bool scrolls)
    {
        m_height  = height;
        m_scrolls = scrolls;
        setMinimumHeight(height);
        updateGeometry();
    }

    QSize sizeHint() const override
    {
        const QWidget* content = widget();
        if (!content)
            return QScrollArea::sizeHint();
        const QSize hint = content->sizeHint();
        return QSize(hint.width() + (m_scrolls ? barWidth() : 0), m_height > 0 ? m_height : hint.height());
    }

    QSize minimumSizeHint() const override
    {
        const QWidget* content = widget();
        if (!content)
            return QScrollArea::minimumSizeHint();
        // Room for the bar too: it appears when the window is made narrower (the text wraps into more lines).
        return QSize(content->minimumSizeHint().width() + barWidth(), QScrollArea::minimumSizeHint().height());
    }

  private:
    int barWidth() const { return verticalScrollBar()->sizeHint().width(); }

    int  m_height  = 0;
    bool m_scrolls = false;
};

QFormLayout* form(QWidget* parent)
{
    return SettingsSection::form(parent);
}

// A row of its own in the field column, under the field it explains.
void addUnderField(QFormLayout* layout, QWidget* widget)
{
    SettingsSection::addUnderField(layout, widget);
}

// TeamSpeak's dark skins colour everything by style sheet; once polished, the palette shows those
// colours. The same test the chat previews use.
bool isDark(const QPalette& palette)
{
    return palette.color(QPalette::WindowText).lightness() > 170 || palette.color(QPalette::Window).lightness() < 128;
}

// The text colour moved toward the background, as far as start allows while it keeps minContrast
// (4.5:1 for secondary text, 3:1 for disabled text that should stay legible).
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

QString downloadUrlProblemText(Settings::DownloadUrlProblem problem)
{
    switch (problem) {
    case Settings::DownloadUrlProblem::None:
        return {};
    case Settings::DownloadUrlProblem::NotWebAddress:
        return i18n::t("This isn't a web address. Enter one like https://example.com, or leave the field empty.");
    case Settings::DownloadUrlProblem::Scheme:
        return i18n::t("Only web links can be used here. Enter an address that starts with https:// or http://.");
    case Settings::DownloadUrlProblem::Brackets:
        return i18n::t("The link can't contain [ or ] because they would break the chat message. Remove them or write them as %5B and %5D.");
    case Settings::DownloadUrlProblem::TooLong:
        return i18n::t("The link is longer than %1 characters. Use a shorter address.").arg(Settings::maxDownloadUrlLength);
    }
    return {};
}

constexpr quint64 kMB = 1024 * 1024;

// "700 MB of 1024 MB used (68%)": in the unit of the limit field above it, so the two compare at a
// glance. Whole MB while the limit is under 10 GB, then GB with one decimal.
QString cacheUsageText(quint64 used, int limitMB)
{
    const bool inGB   = limitMB >= 10 * 1024;
    const auto amount = [inGB](quint64 bytes) {
        if (bytes < kMB)
            return formatSize(bytes); // "300 KB" rather than "0 MB"
        if (inGB && bytes >= 1024 * kMB)
            return i18n::t("%1 GB").arg(static_cast<double>(bytes) / (1024 * kMB), 0, 'f', 1);
        return i18n::t("%1 MB").arg(static_cast<qint64>((bytes + kMB / 2) / kMB));
    };
    const quint64 limit   = static_cast<quint64>(limitMB) * kMB;
    const QString total   = inGB ? i18n::t("%1 GB").arg(limitMB / 1024.0, 0, 'f', 1) : i18n::t("%1 MB").arg(limitMB);
    const int     percent = static_cast<int>(qMin<quint64>((used * 100 + limit / 2) / limit, 9999));
    if (percent == 0)
        return i18n::t("%1 of %2 used").arg(amount(used), total);
    return i18n::t("%1 of %2 used (%3%)").arg(amount(used), total).arg(percent);
}

} // namespace

SettingsDialog::SettingsDialog(Core* core, QWidget* parent)
    : QDialog(parent)
    , m_core(core)
{
    setObjectName(QString::fromLatin1("tsmediaSettingsDialog")); // matched by the style sheet below
    setWindowTitle(i18n::t(TSMEDIA_NAME " — Settings"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setLayoutDirection(Qt::LeftToRight); // also when TeamSpeak itself runs right-to-left

    // 2.2: the groups live in tabs. The tab bar comes first in the Tab order, then the groups of the tab
    // shown, in reading order, then the buttons; Ctrl+Tab and Ctrl+Shift+Tab switch tabs. Each "&"
    // marks the Alt+letter access key (only the tab shown reacts); Enter is OK and Esc is Cancel.
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QString::fromLatin1("tsmediaSettingsTabs"));
    m_tabs->setDocumentMode(false);
    m_tabs->setUsesScrollButtons(false); // five short titles: never hide one behind arrows
    // "&&": a literal "&" (a single one would make the next character an access key).
    const QString tabTitles[] = {i18n::t("General"), i18n::t("Sending"), i18n::t("Receiving && playback"), i18n::t("Servers"), i18n::t("Privacy && updates")};
    for (const QString& title : tabTitles) {
        auto* area = new PageArea(m_tabs);
        auto* page = new QWidget(area);
        page->setObjectName(QString::fromLatin1("tsmediaSettingsPage")); // matched by the style sheet below
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setSpacing(10);
        pageLayout->addStretch(1); // groups go above it
        area->setWidget(page);
        page->setAutoFillBackground(false); // setWidget() turned it on
        m_tabs->addTab(area, title);
        m_pages.append(page);
        m_pageAreas.append(area);
    }

    // ---- Receiving --------------------------------------------------------------------------------
    auto* receive     = new QGroupBox(i18n::t("Receiving"), m_pages.at(static_cast<int>(Tab::ReceivingPlayback)));
    m_inlinePreviews  = new QCheckBox(i18n::t("&Show images, videos and file cards in the chat"), receive);
    m_receiveDetails  = new QWidget(receive);
    // 2.2 data saver, at the top of the tab's first group.
    m_dataSaver           = new QCheckBox(i18n::t("&Data saver: pause automatic downloads"), m_receiveDetails);
    auto* dataSaverHint   = hint(i18n::t("Images and videos load when you click them; small previews still load. Your limits below are kept. "
                                         "You can turn it off for single servers under Servers."),
                                 m_receiveDetails);
    m_dataSaver->setAccessibleDescription(dataSaverHint->text());
    m_autoDownload    = new QCheckBox(i18n::t("Download &images and GIFs automatically up to"), m_receiveDetails);
    m_autoDownloadMax = spin(Settings::autoDownloadMaxMBRange, 5, megabytes(), m_receiveDetails);
    m_autoDownloadMax->setAccessibleName(i18n::t("Download images and GIFs automatically up to"));
    m_autoplayGifs = new QCheckBox(i18n::t("Play &GIFs automatically"), m_receiveDetails);
    m_autoplayGifs->setToolTip(i18n::t("When this is off, or animations are turned off in Windows, GIFs play only while the pointer is over them."));
    m_gifHint = hint(i18n::t("Windows animations are turned off, so GIFs play only while the pointer is over them."), m_receiveDetails);
    m_videoAutoDownload = spin(Settings::videoAutoDownloadMBRange, 10, megabytes(), m_receiveDetails);
    m_videoAutoDownload->setPrefix(i18n::t("up to "));
    m_videoAutoDownload->setSpecialValueText(i18n::t("Off (download when played)")); // replaces prefix and suffix at 0
    m_videoAutoDownload->setMinimumWidth(m_videoAutoDownload->fontMetrics().horizontalAdvance(m_videoAutoDownload->specialValueText()) + 48);
    auto* previewSizeLabel = new QLabel(i18n::t("Maximum preview si&ze"), m_receiveDetails);
    m_previewWidth         = spin(Settings::previewMaxWidthRange, 20, pixels(), m_receiveDetails);
    m_previewWidth->setAccessibleName(i18n::t("Maximum preview width"));
    previewSizeLabel->setBuddy(m_previewWidth);
    m_previewHeight = spin(Settings::previewMaxHeightRange, 20, pixels(), m_receiveDetails);
    m_previewHeight->setAccessibleName(i18n::t("Maximum preview height"));

    auto* imageRow = new QHBoxLayout;
    imageRow->addWidget(m_autoDownload);
    imageRow->addWidget(m_autoDownloadMax);
    imageRow->addStretch(1);
    auto* previewSizeRow = new QHBoxLayout;
    previewSizeRow->addWidget(m_previewWidth);
    previewSizeRow->addWidget(new QLabel(i18n::t("×"), m_receiveDetails));
    previewSizeRow->addWidget(m_previewHeight);
    previewSizeRow->addStretch(1);

    auto* receiveDetailsForm = form(m_receiveDetails);
    receiveDetailsForm->setContentsMargins(0, 0, 0, 0);
    receiveDetailsForm->addRow(m_dataSaver); // 2.2 data saver
    receiveDetailsForm->addRow(dataSaverHint);
    m_indented.append(dataSaverHint);
    receiveDetailsForm->addRow(imageRow);
    // In a box of its own: a hidden form row would still take the form's row spacing.
    auto* gifRows = new QVBoxLayout;
    gifRows->setSpacing(receiveDetailsForm->verticalSpacing());
    gifRows->addWidget(m_autoplayGifs);
    gifRows->addWidget(m_gifHint);
    receiveDetailsForm->addRow(gifRows);
    receiveDetailsForm->addRow(i18n::t("Download &videos and audio automatically"), m_videoAutoDownload); // 2.2 audio: the same limit
    receiveDetailsForm->addRow(previewSizeLabel, previewSizeRow);
    auto* receiveForm = form(receive);
    receiveForm->addRow(m_inlinePreviews);
    receiveForm->addRow(m_receiveDetails);
    m_indented.append(m_receiveDetails);
    m_indented.append(m_gifHint);
    updateGifHint();

    // ---- Playback ---------------------------------------------------------------------------------
    auto* playback    = new QGroupBox(i18n::t("Playback"), m_pages.at(static_cast<int>(Tab::ReceivingPlayback)));
    auto* volumeLabel = new QLabel(i18n::t("Default vol&ume"), playback);
    m_volume          = new QSlider(Qt::Horizontal, playback);
    m_volume->setRange(Settings::videoVolumeRange.min, Settings::videoVolumeRange.max);
    m_volume->setSingleStep(5);
    m_volume->setPageStep(10);
    m_volume->setAccessibleName(i18n::t("Default volume"));
    volumeLabel->setBuddy(m_volume);
    m_volumeLabel = new QLabel(playback);
    m_volumeLabel->setMinimumWidth(m_volumeLabel->fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 6);
    m_volume->setToolTip(i18n::t("Changing the volume in the viewer updates this too."));
    m_volume->setAccessibleDescription(m_volume->toolTip());
    m_startMuted = new QCheckBox(i18n::t("Start videos &muted"), playback);
    m_loop       = new QCheckBox(i18n::t("Loo&p videos"), playback);

    auto* volumeRow = new QHBoxLayout;
    volumeRow->addWidget(m_volume, 1);
    volumeRow->addWidget(m_volumeLabel);
    auto* playbackForm = form(playback);
    playbackForm->addRow(volumeLabel, volumeRow);
    playbackForm->addRow(m_startMuted);
    playbackForm->addRow(m_loop);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) { m_volumeLabel->setText(i18n::t("%1%").arg(value)); });

    // ---- Media cache ------------------------------------------------------------------------------
    auto* cache     = new QGroupBox(i18n::t("Media cache"), m_pages.at(static_cast<int>(Tab::General)));
    m_cacheLimit    = spin(Settings::cacheLimitMBRange, 256, megabytes(), cache);
    auto* cacheHint = hint(i18n::t("Media you haven't opened for the longest time is removed first."), cache);
    m_cacheLimit->setAccessibleDescription(cacheHint->text());
    m_cacheLabel    = new QLabel(cache);
    auto* openCache = new QPushButton(i18n::t("&Open folder"), cache);
    openCache->setAccessibleName(i18n::t("Open media cache folder"));
    m_clearCache = new QPushButton(i18n::t("&Clear cache"), cache);
    m_clearCache->setAccessibleName(i18n::t("Clear media cache"));
    openCache->setAutoDefault(false);
    m_clearCache->setAutoDefault(false);
    m_cacheNotice = new QTimer(this);
    m_cacheNotice->setSingleShot(true);

    auto* cacheRow = new QHBoxLayout;
    cacheRow->addWidget(m_cacheLabel, 1);
    cacheRow->addWidget(openCache);
    cacheRow->addWidget(m_clearCache);
    auto* cacheForm = form(cache);
    cacheForm->addRow(i18n::t("Siz&e limit"), m_cacheLimit);
    cacheForm->addRow(cacheHint);
    cacheForm->addRow(cacheRow);

    connect(openCache, &QPushButton::clicked, this, [this] {
        if (m_core)
            m_core->openCacheFolder();
    });
    connect(m_clearCache, &QPushButton::clicked, this, [this] { clearCache(); });
    connect(m_cacheNotice, &QTimer::timeout, this, [this] { updateCacheLabel(); });
    connect(m_cacheLimit, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        m_cacheNotice->stop();
        updateCacheLabel();
    });

    // ---- 2.2 diagnostics: Troubleshooting ---------------------------------------------------------
    auto* troubleshooting = new QGroupBox(i18n::t("Troubleshooting"), m_pages.at(static_cast<int>(Tab::General)));
    auto* diagnostics     = new DiagnosticsButton(troubleshooting);
    diagnostics->setText(i18n::t("&Diagnostic info…"));
    auto* diagnosticsHint = hint(i18n::t("Versions, settings and recent messages to paste into a bug report. Nothing is sent by the plugin."), troubleshooting);
    diagnostics->setAccessibleDescription(diagnosticsHint->text());
    auto* troubleshootingLayout = new QVBoxLayout(troubleshooting);
    troubleshootingLayout->addWidget(diagnosticsHint);
    troubleshootingLayout->addWidget(diagnostics, 0, Qt::AlignLeft);
    troubleshooting->setVisible(diag::hasDialogOpener()); // the window only exists inside TeamSpeak

    // ---- Sending ----------------------------------------------------------------------------------
    auto* send = new QGroupBox(i18n::t("Sending"), m_pages.at(static_cast<int>(Tab::Sending)));
    m_dragDrop = new QCheckBox(i18n::t("Send &files dropped on the chat (hold Shift for TeamSpeak's own drop)"), send);
    m_paste    = new QCheckBox(i18n::t("Send screenshots and files pas&ted into the chat input (Ctrl+V)"), send);
    m_jpeg     = new QCheckBox(i18n::t("Convert pasted images over 2 MB to &JPEG"), send);
    m_jpeg->setToolTip(i18n::t("Images with transparency stay PNG."));
    m_previews        = new QCheckBox(i18n::t("Upload a small previe&w with large images and videos"), send);
    auto* previewHint = hint(i18n::t("Others see a sharp preview while the full file downloads."), send);
    m_previews->setAccessibleDescription(previewHint->text());
    m_uploadMax = spin(Settings::uploadMaxMBRange, 10, megabytes(), send);
    m_uploadDir = new QLineEdit(send);
    m_uploadDir->setPlaceholderText(QString::fromLatin1(Settings::defaultUploadDirectory));
    m_uploadDir->setMaxLength(255);
    auto* uploadDirHint = hint(i18n::t("A folder in the channel's file browser, created when needed. Type / for the top level."), send);
    m_uploadDir->setAccessibleDescription(uploadDirHint->text());

    auto* sendForm = form(send);
    sendForm->addRow(m_dragDrop);
    sendForm->addRow(m_paste);
    sendForm->addRow(m_jpeg);
    sendForm->addRow(m_previews);
    sendForm->addRow(previewHint);
    sendForm->addRow(i18n::t("Upload size &limit"), m_uploadMax);
    sendForm->addRow(i18n::t("Upload fol&der"), m_uploadDir);
    addUnderField(sendForm, uploadDirHint);
    m_indented.append(previewHint);

    // ---- Note for people without the plugin -------------------------------------------------------
    auto* note       = new QGroupBox(i18n::t("Note for people without the plugin"), m_pages.at(static_cast<int>(Tab::Sending)));
    m_notice         = new QCheckBox(i18n::t("Add a &note after files you send"), note);
    auto* noticeHint = hint(i18n::t("People without the plugin see “TS Media chat plugin required to view this in chat” after the link."), note);
    m_notice->setAccessibleDescription(noticeHint->text());
    m_noteDetails = new QWidget(note);
    m_downloadUrl = new QLineEdit(m_noteDetails);
    m_downloadUrl->setPlaceholderText(QString::fromLatin1(Settings::defaultDownloadUrl));
    m_downloadUrl->setMaxLength(Settings::maxDownloadUrlLength);
    m_downloadUrlHint = i18n::t("“TS Media chat” in the note links here. Leave empty for the official page.");

    // Under the field: a warning icon with the message, so it doesn't depend on colour, then the helper.
    auto* urlNotes      = new QWidget(m_noteDetails);
    m_downloadUrlError  = new QWidget(urlNotes);
    auto*     errorIcon = new QLabel(m_downloadUrlError);
    const int iconSize  = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    errorIcon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(iconSize, iconSize));
    errorIcon->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_downloadUrlErrorText = new QLabel(m_downloadUrlError);
    m_downloadUrlErrorText->setTextFormat(Qt::PlainText);
    m_downloadUrlErrorText->setWordWrap(true);
    m_downloadUrlErrorText->setProperty("role", QString::fromLatin1("error"));
    auto* errorRow = new QHBoxLayout(m_downloadUrlError);
    errorRow->setContentsMargins(0, 0, 0, 0);
    errorRow->addWidget(errorIcon, 0, Qt::AlignTop);
    errorRow->addWidget(m_downloadUrlErrorText, 1);
    m_downloadUrlError->hide();
    auto* urlNotesLayout = new QVBoxLayout(urlNotes);
    urlNotesLayout->setContentsMargins(0, 0, 0, 0);
    urlNotesLayout->addWidget(m_downloadUrlError);
    urlNotesLayout->addWidget(hint(m_downloadUrlHint, urlNotes));

    auto* noteDetailsForm = form(m_noteDetails);
    noteDetailsForm->setContentsMargins(0, 0, 0, 0);
    noteDetailsForm->addRow(i18n::t("Lin&k in the note"), m_downloadUrl);
    addUnderField(noteDetailsForm, urlNotes);
    auto* noteForm = form(note);
    noteForm->addRow(m_notice);
    noteForm->addRow(noticeHint);
    noteForm->addRow(m_noteDetails);
    m_indented.append(noticeHint);
    m_indented.append(m_noteDetails);

    // Checked when the field is left (or Enter is pressed); while a message shows, every edit checks
    // again so it goes away as soon as the link is fixed.
    connect(m_downloadUrl, &QLineEdit::editingFinished, this, [this] {
        if (m_notice->isChecked())
            checkDownloadUrl(true);
    });
    connect(m_downloadUrl, &QLineEdit::textEdited, this, [this] {
        if (!m_downloadUrlError->isHidden())
            checkDownloadUrl(false);
    });

    // ---- 2.2 per-server settings: the Servers section (self-contained, see serverssection.h) ------
    m_servers = new ServersSection(this);

    // ---- header, buttons, layout ------------------------------------------------------------------
    auto* title     = new QLabel(i18n::t(TSMEDIA_NAME), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    if (titleFont.pointSizeF() > 0)
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
    else
        titleFont.setPixelSize(qRound(titleFont.pixelSize() * 1.15));
    title->setFont(titleFont);
    auto* version = hint(i18n::t("Version " TSMEDIA_VERSION), this);
    version->setWordWrap(false);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    titleRow->addWidget(title, 0, Qt::AlignBottom);
    titleRow->addWidget(version, 0, Qt::AlignBottom);
    titleRow->addStretch(1);
    auto* about = hint(i18n::t("Files you send are stored in the channel's file browser, where anyone allowed to download can get them. "
                               "People with TS Media chat see them in the chat."),
                       this);
    about->setMaximumWidth(about->fontMetrics().averageCharWidth() * 80); // about 75 characters a line
    // In a row of its own, so the label wraps at its own width rather than the dialog's.
    auto* aboutRow = new QHBoxLayout;
    aboutRow->addWidget(about, 1);
    aboutRow->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply | QDialogButtonBox::RestoreDefaults, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n::t("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(i18n::t("Cancel"));
    m_applyButton = buttons->button(QDialogButtonBox::Apply);
    m_applyButton->setText(i18n::t("&Apply"));
    m_applyButton->setAutoDefault(false);
    QPushButton* restore = buttons->button(QDialogButtonBox::RestoreDefaults);
    restore->setText(i18n::t("&Restore defaults"));
    restore->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_applyButton->isEnabled() || apply())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_applyButton, &QPushButton::clicked, this, [this] { apply(); });
    // Only fills in the form: nothing is saved until OK or Apply, and Cancel undoes it. Sections keep
    // what Restore defaults must not touch (SettingsSection::restoreDefaults).
    connect(restore, &QPushButton::clicked, this, [this] {
        load(Settings(), false);
        m_loading = true;
        for (SettingsSection* section : qAsConst(m_sections))
            section->restoreDefaults(Settings());
        m_loading = false;
        setDirty(true);
    });

    // The 2.1 groups, unchanged, in their tabs.
    tabLayout(Tab::General)->insertWidget(tabLayout(Tab::General)->count() - 1, cache);
    tabLayout(Tab::General)->insertWidget(tabLayout(Tab::General)->count() - 1, troubleshooting); // 2.2 diagnostics
    tabLayout(Tab::Sending)->insertWidget(tabLayout(Tab::Sending)->count() - 1, send);
    tabLayout(Tab::Sending)->insertWidget(tabLayout(Tab::Sending)->count() - 1, note);
    tabLayout(Tab::ReceivingPlayback)->insertWidget(tabLayout(Tab::ReceivingPlayback)->count() - 1, receive);
    tabLayout(Tab::ReceivingPlayback)->insertWidget(tabLayout(Tab::ReceivingPlayback)->count() - 1, playback);
    // Nothing in these yet (per-server settings, privacy and the updater add their sections). Disabled
    // too: Ctrl+Tab and the arrow keys skip disabled tabs, not hidden ones.
    for (Tab empty : {Tab::Servers, Tab::PrivacyUpdates}) {
        m_tabs->setTabVisible(static_cast<int>(empty), false);
        m_tabs->setTabEnabled(static_cast<int>(empty), false);
    }
    // 2.2 updater: a self-contained group with its own keys (apply() below; Restore defaults leaves them
    // alone). Added before the dirty tracking below, so its checkbox enables Apply.
    addSection(Tab::PrivacyUpdates, new upd::UpdateSettingsGroup(this));
    // 2.2 per-server settings: loaded and saved with the form (load() / apply()).
    addSection(Tab::Servers, m_servers);
    // 2.2 servergroup: Server access, a self-contained box (its actions act on the server right away).
    auto* serverAccess = new AccessGroupBox(this);
    connect(serverAccess, &AccessGroupBox::contentsChanged, this, [this] {
        if (isVisible())
            QTimer::singleShot(0, this, [this] { fitToContents(); }); // once the layouts have taken in the new text
    });
    addSection(Tab::Servers, serverAccess);
    if (g_lastTab > 0 && g_lastTab < m_tabs->count() && m_tabs->isTabVisible(g_lastTab))
        m_tabs->setCurrentIndex(g_lastTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [](int index) { g_lastTab = index; });

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    auto* header = new QVBoxLayout;
    header->setSpacing(2);
    header->addLayout(titleRow);
    header->addLayout(aboutRow);
    layout->addLayout(header);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(buttons);

    connect(m_inlinePreviews, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_autoDownload, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_notice, &QCheckBox::toggled, this, [this] { updateEnabled(); });

    // Apply becomes available with the first change. (Not findChildren<QLineEdit*>: spin boxes have one inside.)
    const auto changed = [this] {
        if (!m_loading)
            setDirty(true);
    };
    for (QCheckBox* box : findChildren<QCheckBox*>())
        connect(box, &QCheckBox::toggled, this, changed);
    for (QSpinBox* box : findChildren<QSpinBox*>())
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged), this, changed);
    connect(m_volume, &QSlider::valueChanged, this, changed);
    connect(m_downloadUrl, &QLineEdit::textChanged, this, changed);
    connect(m_uploadDir, &QLineEdit::textChanged, this, changed);

    // 2.2 per-server settings: "Same as all servers (…)" follows the form, saved or not.
    connect(m_servers, &ServersSection::changed, this, changed);
    connect(m_dataSaver, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    const auto globalsChanged = [this] {
        m_servers->setGlobals(m_dataSaver->isChecked(), m_uploadDir->text(), m_uploadMax->value(), m_notice->isChecked());
    };
    connect(m_dataSaver, &QCheckBox::toggled, this, globalsChanged);
    connect(m_notice, &QCheckBox::toggled, this, globalsChanged);
    connect(m_uploadDir, &QLineEdit::textChanged, this, globalsChanged);
    connect(m_uploadMax, QOverload<int>::of(&QSpinBox::valueChanged), this, globalsChanged);

    m_numberFields = {m_autoDownloadMax, m_previewWidth, m_previewHeight, m_uploadMax};

    // TeamSpeak's dark skins switch off every focus indicator. Like Windows, the dialog shows its own
    // focus rings only once the keyboard has been used to move around in it.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (m_keyboardFocus || !now || now->window() != this || !testAttribute(Qt::WA_KeyboardFocusChange))
            return;
        m_keyboardFocus = true;
        applyTheme();
    });

    // Walks the whole cache folder: once, not on every change of the limit.
    m_cacheUsed = m_core ? m_core->cacheSize() : 0;
    m_loaded    = Settings::instance();
    load(m_loaded);
    reloadServers(); // 2.2 per-server settings
    setDirty(false);
    m_ready = true;
    applyTheme();
}

void SettingsDialog::addSection(Tab tab, QWidget* widget)
{
    if (!widget)
        return;
    QVBoxLayout* layout = tabLayout(tab);
    layout->insertWidget(layout->count() - 1, widget); // above the stretch, after what is there
    m_tabs->setTabEnabled(static_cast<int>(tab), true);
    m_tabs->setTabVisible(static_cast<int>(tab), true);
    if (auto* section = qobject_cast<SettingsSection*>(widget)) {
        m_sections.append(section);
        m_loading = true;
        section->load(m_loaded);
        m_loading = false;
        connect(section, &SettingsSection::changed, this, [this] {
            if (!m_loading)
                setDirty(true);
        });
    }
    if (m_ready)
        applyTheme(); // its helper texts, indents and focus rings
    if (isVisible())
        QTimer::singleShot(0, this, [this] { fitToContents(); });
}

QVBoxLayout* SettingsDialog::tabLayout(Tab tab) const
{
    return static_cast<QVBoxLayout*>(m_pages.at(static_cast<int>(tab))->layout());
}

void SettingsDialog::showTab(Tab tab)
{
    m_tabs->setCurrentIndex(static_cast<int>(tab));
}

void SettingsDialog::showTabOf(QWidget* widget)
{
    for (int i = 0; i < m_pages.size(); ++i) {
        if (widget && m_pages.at(i)->isAncestorOf(widget)) {
            showTab(static_cast<Tab>(i));
            return;
        }
    }
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    // A new window is capped at 2/3 of the screen (QWidget::adjustSize), which squeezes the rows on a
    // scaled laptop screen. Before QDialog::showEvent, which centres the dialog with this size.
    if (!event->spontaneous() && !m_sized) {
        m_sized = true;
        fitToContents();
    }
    QDialog::showEvent(event);
}

// Grows the window to what its contents need at the current width, as far as the screen allows. Qt's
// minimum size leaves out wrapped text, so a message appearing under a field would squeeze the rows.
// Every page gets the height of the tallest one, so switching tabs never resizes the window. On a
// screen too short for that (1366 x 768 at 125 %, for example) the pages scroll, and the tabs and the
// buttons stay on screen.
void SettingsDialog::fitToContents()
{
    constexpr int kMinPageHeight = 120;

    QSize avail;
    if (const QScreen* screen = parentWidget() ? parentWidget()->screen() : this->screen())
        avail = screen->availableGeometry().size() - QSize(16, 48); // the frame and title bar
    QLayout* top = layout();

    // First the height the pages need at the width the window gets, then as much of it as fits.
    int areaWidth = 0;
    for (QScrollArea* area : qAsConst(m_pageAreas)) {
        static_cast<PageArea*>(area)->setHeight(0, false);
        areaWidth = qMax(areaWidth, area->sizeHint().width());
    }
    int width = qMax(sizeHint().width(), this->width());
    if (avail.isValid())
        width = qMin(width, avail.width());
    const QMargins margins = top->contentsMargins();
    const int      frame   = qMax(0, m_tabs->sizeHint().width() - areaWidth); // the tab widget's frame around a page
    const int      inner   = width - margins.left() - margins.right() - frame;
    int            needed  = 0;
    for (QWidget* page : qAsConst(m_pages)) {
        QLayout* pageLayout = page->layout();
        needed              = qMax(needed, pageLayout->hasHeightForWidth() ? pageLayout->totalHeightForWidth(inner) : page->sizeHint().height());
    }
    for (QScrollArea* area : qAsConst(m_pageAreas))
        static_cast<PageArea*>(area)->setHeight(needed, false);
    if (avail.isValid()) {
        const int total  = top->hasHeightForWidth() ? top->totalHeightForWidth(width) : sizeHint().height();
        const int excess = total - avail.height();
        if (excess > 0) {
            for (QScrollArea* area : qAsConst(m_pageAreas))
                static_cast<PageArea*>(area)->setHeight(qMax(kMinPageHeight, needed - excess), true);
        }
    }

    QSize wanted = sizeHint().expandedTo(QSize(this->width(), 0));
    if (top->hasHeightForWidth())
        wanted.setHeight(top->totalHeightForWidth(wanted.width()));
    if (avail.isValid())
        wanted = wanted.boundedTo(avail);
    if (wanted.width() > this->width() || wanted.height() > height())
        resize(wanted.expandedTo(size()));
}

// Enter on a focused button that isn't the default one (Apply, Clear cache, Open folder, Restore
// defaults) presses that button, as in Windows dialogs, instead of OK. In a field Enter is still OK.
void SettingsDialog::keyPressEvent(QKeyEvent* event)
{
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        auto* button = qobject_cast<QPushButton*>(focusWidget());
        if (button && button->isVisible() && button->isEnabled() && !button->isDefault()) {
            button->animateClick();
            event->accept();
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

void SettingsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (!m_ready) // still in the constructor
        return;
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange)
        applyTheme(); // also when TeamSpeak's skin changes while the dialog is open
    else if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        syncVolume();
        updateGifHint(); // the Windows setting may have changed meanwhile
        // Media may have been downloaded since: an empty cache must not stay "empty" with Clear cache
        // greyed out. (A cache that isn't empty is measured again by Clear cache itself.)
        if (m_core && m_cacheUsed == 0) {
            m_cacheUsed = m_core->cacheSize();
            updateCacheLabel();
        }
    }
}

// Windows animations off: inline GIFs play only on hover, whatever "Play GIFs automatically" says.
void SettingsDialog::updateGifHint()
{
    const bool show = !ui::animationsEnabled();
    m_autoplayGifs->setAccessibleDescription(show ? m_gifHint->text() : QString());
    if (show == !m_gifHint->isHidden())
        return;
    m_gifHint->setVisible(show);
    if (show && isVisible())
        QTimer::singleShot(0, this, [this] { fitToContents(); }); // once the layouts have taken in the new row
}

void SettingsDialog::load(const Settings& s, bool sections)
{
    m_loading = true;
    m_inlinePreviews->setChecked(s.inlinePreviews);
    m_autoDownload->setChecked(s.autoDownloadImages);
    m_autoDownloadMax->setValue(s.autoDownloadMaxMB);
    m_autoplayGifs->setChecked(s.autoplayGifs);
    m_videoAutoDownload->setValue(s.videoAutoDownloadMB);
    m_previewWidth->setValue(s.previewMaxWidth);
    m_previewHeight->setValue(s.previewMaxHeight);
    m_dataSaver->setChecked(s.dataSaver); // 2.2 data saver

    m_volume->setValue(s.videoVolume);
    m_volumeLabel->setText(i18n::t("%1%").arg(m_volume->value()));
    m_startMuted->setChecked(s.videosStartMuted);
    m_loop->setChecked(s.loopVideos);

    m_cacheLimit->setValue(s.cacheLimitMB);

    m_dragDrop->setChecked(s.interceptDragDrop);
    m_paste->setChecked(s.interceptPaste);
    m_jpeg->setChecked(s.convertLargePngToJpeg);
    m_previews->setChecked(s.generatePreviews);
    m_uploadMax->setValue(s.uploadMaxMB);
    m_uploadDir->setText(s.uploadDirectory);
    m_uploadDir->setCursorPosition(0);

    m_notice->setChecked(s.addRequiredNotice);
    // The default link is the placeholder, so the field is empty as its helper says.
    m_downloadUrl->setText(s.pluginDownloadUrl == QLatin1String(Settings::defaultDownloadUrl) ? QString() : s.pluginDownloadUrl);
    m_downloadUrl->setCursorPosition(0); // the start of a long link matters most

    if (sections) {
        for (SettingsSection* section : qAsConst(m_sections))
            section->load(s);
    }
    m_loading = false;

    // 2.2 per-server settings: only the "Same as all servers" texts; Restore defaults never forgets servers.
    m_servers->setGlobals(s.dataSaver, s.uploadDirectory, s.uploadMaxMB, s.addRequiredNotice);
    updateEnabled();
    updateCacheLabel();
}

// 2.2 per-server settings
void SettingsDialog::reloadServers()
{
    m_servers->reload(datasaver::connectedServers());
}

void SettingsDialog::setDirty(bool dirty)
{
    m_applyButton->setEnabled(dirty);
}

void SettingsDialog::updateEnabled()
{
    // Disabling a container disables the labels and helpers in it too.
    m_receiveDetails->setEnabled(m_inlinePreviews->isChecked());
    // 2.2 data saver: the automatic-download limits are kept, they just don't apply while it is on.
    const bool    saving = m_dataSaver->isChecked();
    const QString paused = saving ? i18n::t("Paused by data saver") : QString();
    m_autoDownload->setEnabled(!saving);
    m_autoDownloadMax->setEnabled(!saving && m_autoDownload->isChecked());
    m_videoAutoDownload->setEnabled(!saving);
    for (QWidget* field : {static_cast<QWidget*>(m_autoDownload), static_cast<QWidget*>(m_autoDownloadMax), static_cast<QWidget*>(m_videoAutoDownload)}) {
        if (field->toolTip() != paused)
            field->setToolTip(paused);
    }
    m_noteDetails->setEnabled(m_notice->isChecked());
    // The link only matters with the note on: no message about it otherwise.
    if (m_notice->isChecked() && !m_downloadUrl->text().trimmed().isEmpty())
        checkDownloadUrl(false);
    else
        showDownloadUrlError(QString());
}

// The viewer saves its volume as it changes. Follow it, unless the slider was moved here.
void SettingsDialog::syncVolume()
{
    const int saved = Settings::instance().videoVolume;
    if (saved == m_loaded.videoVolume || m_volume->value() != m_loaded.videoVolume)
        return;
    m_loaded.videoVolume = saved;
    m_loading            = true;
    m_volume->setValue(saved);
    m_loading = false;
}

bool SettingsDialog::apply()
{
    QString                            url;
    const Settings::DownloadUrlProblem problem = Settings::checkDownloadUrl(m_downloadUrl->text(), &url);
    if (problem != Settings::DownloadUrlProblem::None && m_notice->isChecked()) {
        showDownloadUrlError(downloadUrlProblemText(problem));
        showTabOf(m_downloadUrl); // OK may have been pressed on another tab
        m_downloadUrl->setFocus(Qt::OtherFocusReason);
        m_downloadUrl->selectAll();
        return false;
    }
    for (SettingsSection* section : qAsConst(m_sections)) {
        if (QWidget* problemField = section->validate()) {
            showTabOf(problemField);
            problemField->setFocus(Qt::OtherFocusReason);
            return false;
        }
    }

    Settings form            = m_loaded;
    form.inlinePreviews      = m_inlinePreviews->isChecked();
    form.autoDownloadImages  = m_autoDownload->isChecked();
    form.autoDownloadMaxMB   = m_autoDownloadMax->value();
    form.autoplayGifs        = m_autoplayGifs->isChecked();
    form.videoAutoDownloadMB = m_videoAutoDownload->value();
    form.previewMaxWidth     = m_previewWidth->value();
    form.previewMaxHeight    = m_previewHeight->value();
    form.dataSaver           = m_dataSaver->isChecked(); // 2.2 data saver

    form.videoVolume      = m_volume->value();
    form.videosStartMuted = m_startMuted->isChecked();
    form.loopVideos       = m_loop->isChecked();

    form.cacheLimitMB = m_cacheLimit->value();

    form.interceptDragDrop     = m_dragDrop->isChecked();
    form.interceptPaste        = m_paste->isChecked();
    form.convertLargePngToJpeg = m_jpeg->isChecked();
    form.generatePreviews      = m_previews->isChecked();
    form.uploadMaxMB           = m_uploadMax->value();
    form.uploadDirectory       = Settings::normalizeUploadDirectory(m_uploadDir->text()); // empty: the default folder
    form.addRequiredNotice     = m_notice->isChecked();
    if (problem == Settings::DownloadUrlProblem::None) // with the note off, an invalid link is not saved; empty means the default link
        form.pluginDownloadUrl = url.isEmpty() ? QString::fromLatin1(Settings::defaultDownloadUrl) : url;

    // Only what was changed here is written: the viewer saves the volume whenever it changes, and the
    // value from when this dialog opened must not undo that.
    Settings&  s    = Settings::instance();
    const auto take = [](auto& current, const auto& loaded, const auto& value) {
        if (value != loaded)
            current = value;
    };
    take(s.inlinePreviews, m_loaded.inlinePreviews, form.inlinePreviews);
    take(s.autoDownloadImages, m_loaded.autoDownloadImages, form.autoDownloadImages);
    take(s.autoDownloadMaxMB, m_loaded.autoDownloadMaxMB, form.autoDownloadMaxMB);
    take(s.autoplayGifs, m_loaded.autoplayGifs, form.autoplayGifs);
    take(s.videoAutoDownloadMB, m_loaded.videoAutoDownloadMB, form.videoAutoDownloadMB);
    take(s.previewMaxWidth, m_loaded.previewMaxWidth, form.previewMaxWidth);
    take(s.previewMaxHeight, m_loaded.previewMaxHeight, form.previewMaxHeight);
    take(s.videoVolume, m_loaded.videoVolume, form.videoVolume);
    take(s.videosStartMuted, m_loaded.videosStartMuted, form.videosStartMuted);
    take(s.loopVideos, m_loaded.loopVideos, form.loopVideos);
    take(s.cacheLimitMB, m_loaded.cacheLimitMB, form.cacheLimitMB);
    take(s.interceptDragDrop, m_loaded.interceptDragDrop, form.interceptDragDrop);
    take(s.interceptPaste, m_loaded.interceptPaste, form.interceptPaste);
    take(s.convertLargePngToJpeg, m_loaded.convertLargePngToJpeg, form.convertLargePngToJpeg);
    take(s.generatePreviews, m_loaded.generatePreviews, form.generatePreviews);
    take(s.uploadMaxMB, m_loaded.uploadMaxMB, form.uploadMaxMB);
    take(s.uploadDirectory, m_loaded.uploadDirectory, form.uploadDirectory);
    take(s.addRequiredNotice, m_loaded.addRequiredNotice, form.addRequiredNotice);
    take(s.pluginDownloadUrl, m_loaded.pluginDownloadUrl, form.pluginDownloadUrl);
    for (SettingsSection* section : qAsConst(m_sections)) {
        section->store(s, m_loaded);
        section->store(form, m_loaded);
    }
    take(s.dataSaver, m_loaded.dataSaver, form.dataSaver); // 2.2 data saver
    m_servers->applyTo(s);                                 // 2.2 per-server settings: the servers edited here
    form.servers = s.servers;
    s.save();
    m_loaded = form;
    if (auto* updates = findChild<upd::UpdateSettingsGroup*>()) // 2.2 updater: its own keys; Restore defaults leaves them alone
        updates->apply();

    // After Apply the dialog stays open: show what was saved ("/tsmedia" for an empty folder, the
    // link with its https://).
    m_loading = true;
    if (m_uploadDir->text() != form.uploadDirectory) {
        m_uploadDir->setText(form.uploadDirectory);
        m_uploadDir->setCursorPosition(0);
    }
    if (problem == Settings::DownloadUrlProblem::None && !url.isEmpty() && m_downloadUrl->text() != url) {
        m_downloadUrl->setText(url);
        m_downloadUrl->setCursorPosition(0);
    }
    m_loading = false;

    setDirty(false);
    emit settingsChanged();
    return true;
}

bool SettingsDialog::checkDownloadUrl(bool normalize)
{
    QString                            url;
    const Settings::DownloadUrlProblem problem = Settings::checkDownloadUrl(m_downloadUrl->text(), &url);
    showDownloadUrlError(downloadUrlProblemText(problem));
    if (problem != Settings::DownloadUrlProblem::None)
        return false;
    if (normalize && !url.isEmpty() && m_downloadUrl->text() != url) {
        m_downloadUrl->setText(url); // shows the https:// that was added
        m_downloadUrl->setCursorPosition(0);
    }
    return true;
}

void SettingsDialog::showDownloadUrlError(const QString& text)
{
    m_downloadUrlErrorText->setText(text);
    m_downloadUrlError->setVisible(!text.isEmpty());
    if (!text.isEmpty() && isVisible())
        QTimer::singleShot(0, this, [this] { fitToContents(); }); // once the layouts have taken in the new row
    // Screen readers get the message with the field, otherwise its helper text.
    m_downloadUrl->setAccessibleDescription(text.isEmpty() ? m_downloadUrlHint : text);
}

void SettingsDialog::updateCacheLabel()
{
    m_clearCache->setEnabled(m_core && m_cacheUsed > 0);
    if (m_cacheNotice->isActive())
        return; // "Cleared …" stays until its timer ends
    m_cacheLabel->setText(m_cacheUsed == 0 ? i18n::t("The cache is empty") : cacheUsageText(m_cacheUsed, m_cacheLimit->value()));
}

void SettingsDialog::clearCache()
{
    if (!m_core)
        return;
    // Measured again: downloads may have added to it since the dialog opened.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_cacheUsed = m_core->cacheSize();
    QApplication::restoreOverrideCursor();
    m_cacheNotice->stop();
    updateCacheLabel();
    if (m_cacheUsed == 0)
        return;

    // Not QMessageBox's static helpers: they label their buttons through Qt's translations, which
    // follow TeamSpeak's language, not the plugin's English. Left-to-right like the dialog: a message
    // box is a window of its own and doesn't inherit the dialog's layout direction. open() instead of
    // exec(): no nested event loop, and the box goes away with the dialog when the plugin shuts down.
    auto* box = new QMessageBox(QMessageBox::Warning, windowTitle(), i18n::t("Delete %1 of downloaded media from this computer?").arg(formatSize(m_cacheUsed)),
                                QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setLayoutDirection(Qt::LeftToRight);
    box->setInformativeText(i18n::t("Files on the server aren't affected: media downloads again when you view it, unless it was deleted "
                                    "from the server. Media that's playing or open in the viewer is kept."));
    QPushButton* clear  = box->addButton(i18n::t("Clear cache"), QMessageBox::DestructiveRole);
    QPushButton* cancel = box->addButton(i18n::t("Cancel"), QMessageBox::RejectRole);
    box->setDefaultButton(cancel); // Enter must not delete anything
    box->setEscapeButton(cancel);
    connect(clear, &QPushButton::clicked, this, [this] {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_clearCache->setEnabled(false);
        const quint64 before = m_cacheUsed;
        m_core->clearCache();
        m_cacheUsed = m_core->cacheSize();
        QApplication::restoreOverrideCursor();

        // Media in use is kept, so the readout alone could look as if nothing happened.
        const quint64 cleared = before > m_cacheUsed ? before - m_cacheUsed : 0;
        m_cacheLabel->setText(m_cacheUsed > 0 ? i18n::t("Cleared %1, kept %2 in use").arg(formatSize(cleared), formatSize(m_cacheUsed))
                                              : i18n::t("Cleared %1").arg(formatSize(cleared)));
        m_cacheNotice->start(qMax(4000, ui::notificationDurationMs()));
        updateCacheLabel();
    });
    box->open();
}

void SettingsDialog::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    ensurePolished();

    const QPalette pal        = palette();
    const bool     dark       = isDark(pal);
    const QColor   background = pal.color(QPalette::Active, QPalette::Window);
    const QColor   muted      = mutedText(pal, 0.4, 4.6);
    QColor         error      = dark ? QColor(0xFF, 0x99, 0xA4) : QColor(0xC4, 0x2B, 0x1C);
    if (ui::contrastRatio(error, background) < 4.5)
        error = pal.color(QPalette::Active, QPalette::WindowText); // the icon still marks it

    // Without a style sheet (and in TeamSpeak's light skin, which leaves labels alone) a palette is
    // enough. Disabled text keeps the style's own colour.
    for (QLabel* label : findChildren<QLabel*>()) {
        const QString role = label->property("role").toString();
        if (role.isEmpty())
            continue;
        const QColor color        = role == QLatin1String("error") ? error : muted;
        QPalette     labelPalette = label->palette();
        labelPalette.setColor(QPalette::Active, QPalette::WindowText, color);
        labelPalette.setColor(QPalette::Inactive, QPalette::WindowText, color);
        label->setPalette(labelPalette);
    }

    QString sheet;
    if (!qApp->styleSheet().isEmpty()) {
        // TeamSpeak's skins colour labels by style sheet, which beats a palette. Built with fromLatin1,
        // never QStringLiteral: TeamSpeak's QStyleSheetStyle keeps the parsed text after the plugin is
        // unloaded (see settings.cpp). Every rule is scoped to this dialog and the boxes it opens.
        sheet = QString::fromLatin1("#tsmediaSettingsDialog QLabel[role=\"hint\"]{color:%1;}"
                                    "#tsmediaSettingsDialog QLabel[role=\"error\"]{color:%2;}")
                    .arg(muted.name(), error.name());
        // The pages' scroll areas stay invisible: no background or frame from a skin's list views.
        sheet += QString::fromLatin1("#tsmediaSettingsDialog QScrollArea,#tsmediaSettingsDialog QScrollArea>QWidget#qt_scrollarea_viewport,"
                                     "#tsmediaSettingsPage{background:transparent;border:none;}");
        if (dark) {
            // Dark skins draw disabled text at about 1.2:1. Keep it dimmed, but readable.
            sheet += QString::fromLatin1("#tsmediaSettingsDialog QCheckBox:disabled,#tsmediaSettingsDialog QLabel:disabled,"
                                         "#tsmediaSettingsDialog QAbstractSpinBox:disabled,#tsmediaSettingsDialog QLineEdit:disabled{color:%1;}")
                         .arg(mutedText(pal, 0.6, 3.0).name());
            // They also turn every focus indicator off. The field being typed in gets an accent border
            // (the colour of the skin's buttons, if it stands out from the field).
            const QColor field  = m_uploadDir->palette().color(QPalette::Active, QPalette::Base);
            QColor       accent = m_applyButton->palette().color(QPalette::Active, QPalette::Button);
            if (ui::contrastRatio(accent, field) < 3.0)
                accent = pal.color(QPalette::Active, QPalette::WindowText);
            sheet += QString::fromLatin1("#tsmediaSettingsDialog QLineEdit:focus,#tsmediaSettingsDialog QAbstractSpinBox:focus{border-color:%1;}"
                                         "#tsmediaSettingsDialog QSlider{border:1px solid transparent;border-radius:4px;}")
                         .arg(accent.name());
            // 2.2 tabs: a dark skin may colour the text without styling tabs, which leaves light text
            // on the native light tabs. Tabs drawn from the skin's own colours: the current one in the
            // full text colour, joined to the page, the others in the secondary text colour (4.5:1).
            const QColor tabText   = pal.color(QPalette::Active, QPalette::WindowText);
            const QColor tabBorder = mutedText(pal, 0.75, 1.0);
            sheet += QString::fromLatin1("#tsmediaSettingsDialog QTabWidget::pane{border:1px solid %1;border-radius:4px;top:-1px;background:%2;}"
                                         "#tsmediaSettingsDialog QTabBar::tab{background:transparent;color:%3;border:1px solid transparent;border-bottom:none;"
                                         "border-top-left-radius:4px;border-top-right-radius:4px;padding:6px 14px;margin-right:2px;}"
                                         "#tsmediaSettingsDialog QTabBar::tab:selected{background:%2;color:%4;border-color:%1;}"
                                         "#tsmediaSettingsDialog QTabBar::tab:hover:!selected{color:%4;}")
                         .arg(tabBorder.name(), background.name(), muted.name(), tabText.name());
            if (m_keyboardFocus) {
                // Rings for the rest, sized so nothing moves: the button's border takes 2 px of the skin's
                // 8 × 14 px padding, and the switch keeps its outer size.
                const QString text = pal.color(QPalette::Active, QPalette::WindowText).name();
                sheet += QString::fromLatin1("#tsmediaSettingsDialog QPushButton:focus{border:2px solid #ffffff;padding:6px 12px;}"
                                             "#tsmediaSettingsDialog QSlider:focus{border-color:%1;}")
                             .arg(text);
                const int switchWidth  = style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, m_loop);
                const int switchHeight = style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, m_loop);
                if (switchWidth >= 20 && switchHeight >= 12) // a switch drawn by the skin
                    sheet += QString::fromLatin1("#tsmediaSettingsDialog QCheckBox::indicator:focus{border:2px solid %1;width:%2px;height:%3px;}")
                                 .arg(text)
                                 .arg(switchWidth - 4)
                                 .arg(switchHeight - 4);
                else // the native box: a ring around the label, where the style puts its focus rectangle
                    sheet += QString::fromLatin1("#tsmediaSettingsDialog QCheckBox:focus{outline:1px solid %1;}").arg(text);
            }
        }
        // 2.2 tabs: skins hide the focus rectangle of the tab bar too. Underline the current tab's title
        // while the tab bar has the keyboard focus (no size change, no colour needed).
        if (m_keyboardFocus)
            sheet += QString::fromLatin1("#tsmediaSettingsDialog QTabBar::tab:selected:focus{text-decoration:underline;}");
    }
    if (sheet != styleSheet())
        setStyleSheet(sheet);

    // Dependent options line up with the text of the checkbox above them; TeamSpeak's skins draw a
    // wider switch than the native checkbox.
    QStyleOptionButton option;
    option.initFrom(m_inlinePreviews);
    option.rect      = QRect(QPoint(), m_inlinePreviews->sizeHint());
    const int indent = qMax(0, m_inlinePreviews->style()->subElementRect(QStyle::SE_CheckBoxContents, &option, m_inlinePreviews).left());
    for (QWidget* widget : qAsConst(m_indented))
        widget->setContentsMargins(indent, 0, 0, 0);
    for (SettingsSection* section : qAsConst(m_sections)) {
        for (QWidget* widget : section->indented())
            widget->setContentsMargins(indent, 0, 0, 0);
    }

    // The number fields share one width, so they line up.
    int fieldWidth = 0;
    for (QSpinBox* box : qAsConst(m_numberFields))
        fieldWidth = qMax(fieldWidth, box->sizeHint().width());
    for (QSpinBox* box : qAsConst(m_numberFields))
        box->setMinimumWidth(fieldWidth);

    m_applyingTheme = false;
}
