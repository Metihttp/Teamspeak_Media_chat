#include "settingsdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "accessgroupbox.h" // 2.2 servergroup
#include "core.h"
#include "darkmode.h"    // 2.7 dark mode: the window in the theme's own tokens while it is on
#include "darkpalette.h"
#include "datasaver.h" // 2.2 per-server settings
#include "diagnosticsdialog.h" // 2.2 diagnostics
#include "i18n.h"
#include "medialink.h"
#include "previewscale.h" // 2.5 zoom: the Preview size presets
#include "peerhub.h" // 2.2 protocol: Clear cache removes the saved reactions too
#include "settingssection.h"
#include "settingswidgets.h" // 2.7 F3: switches, rows, the sidebar
#include "serverssection.h" // 2.2 per-server settings
#include "uiutil.h"
#include "update/updatesettingsgroup.h" // 2.2 updater
#include "proxy/proxygroup.h" // 2.6 chat proxy
#include "settingstheme.h"   // 2.6: the label roles shared with the Chat proxy window; 2.7 F3: the tokens
#include "version.h"

namespace {

// The page shown last; the dialog opens on it again while TeamSpeak runs. 2.7 F3: and the window's size.
int   g_lastTab = 0;
QSize g_lastSize;      // logical px at the size factor below (a new TeamSpeak font size starts afresh)
qreal g_lastSizeK = 0;

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

// Secondary text: the page descriptions and the helpers under options. The theme colours it.
QLabel* hint(const QString& text, QWidget* parent)
{
    return SettingsSection::hint(text, parent);
}

// A few controls side by side (a row's control: buttons, two fields).
QWidget* inRow(std::initializer_list<QWidget*> widgets, int spacing)
{
    auto* holder = new QWidget;
    auto* row    = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(spacing);
    for (QWidget* w : widgets)
        row->addWidget(w);
    return holder;
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

// Every focusable control under a layout, in layout order (rows top to bottom, left to right in a row).
void collectFocusable(QLayout* layout, QList<QWidget*>& out);

void collectFocusable(QWidget* w, QList<QWidget*>& out)
{
    if (!w || w->isWindow())
        return;
    if ((w->focusPolicy() & Qt::TabFocus) == Qt::TabFocus) {
        out << w; // a field with an inner editor (spin, combo) is one stop
        return;
    }
    if (w->layout()) {
        collectFocusable(w->layout(), out);
        return;
    }
    for (QWidget* child : w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        collectFocusable(child, out);
}

void collectFocusable(QLayout* layout, QList<QWidget*>& out)
{
    for (int i = 0; layout && i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (QWidget* w = item->widget())
            collectFocusable(w, out);
        else if (QLayout* sub = item->layout())
            collectFocusable(sub, out);
    }
}

settingsicons::Icon iconOf(SettingsDialog::Tab tab)
{
    switch (tab) {
    case SettingsDialog::Tab::Chat:
        return settingsicons::Icon::Chat;
    case SettingsDialog::Tab::EmojiGifs:
        return settingsicons::Icon::Emoji;
    case SettingsDialog::Tab::Sending:
        return settingsicons::Icon::Send;
    case SettingsDialog::Tab::ReceivingPlayback:
        return settingsicons::Icon::Receive;
    case SettingsDialog::Tab::LinkPreviews:
        return settingsicons::Icon::Link;
    case SettingsDialog::Tab::Privacy:
        return settingsicons::Icon::Shield;
    case SettingsDialog::Tab::Servers:
        return settingsicons::Icon::Server;
    case SettingsDialog::Tab::About:
        return settingsicons::Icon::Info;
    }
    return settingsicons::Icon::Info;
}

QFont scaled(QFont font, qreal factor, QFont::Weight weight)
{
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() * factor);
    else
        font.setPixelSize(qRound(font.pixelSize() * factor));
    font.setWeight(weight);
    return font;
}

// A combo box at least as wide as its longest item needs (settingstheme::comboFitWidth); else as wide as Qt makes it.
void fitComboText(QComboBox* combo)
{
    if (!combo || combo->isEditable() || combo->count() == 0)
        return;
    combo->setMinimumWidth(0);
    const int fit = settingstheme::comboFitWidth(combo);
    if (fit > combo->sizeHint().width())
        combo->setMinimumWidth(fit);
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
    // 2.7 F3 (H1): every size follows TeamSpeak's font (11 px at 100 %, 22 px at 200 %); the colours come in applyTheme().
    m_tokens = settingstheme::tokens(palette(), QColor(), settingstheme::sizeFactor(font(), this));
    const settingstheme::Tokens& t = m_tokens;

    // ---- the frame: the sidebar of pages, the page, the bar -----------------------------------------------------------
    auto* side = new QWidget(this);
    side->setObjectName(QString::fromLatin1("tsmediaSettingsSide"));
    auto* product = new QLabel(i18n::t(TSMEDIA_NAME), side);
    product->setObjectName(QString::fromLatin1("tsmediaSettingsProduct"));
    product->setFont(scaled(font(), 1.15, QFont::DemiBold));
    auto* version = hint(i18n::t("Version " TSMEDIA_VERSION), side);
    version->setWordWrap(false);
    m_nav = new QListWidget(side);
    m_nav->setObjectName(QString::fromLatin1("tsmediaSettingsNav"));
    m_nav->setAccessibleName(i18n::t("Settings pages"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setItemDelegate(new NavDelegate(m_nav));
    m_nav->setMouseTracking(true);
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nav->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_nav->setSelectionMode(QAbstractItemView::SingleSelection);
    m_nav->viewport()->setAutoFillBackground(false);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, t.px(18), 0, t.px(12));
    sideLayout->setSpacing(0);
    product->setContentsMargins(t.px(20), 0, t.px(16), 0);
    version->setContentsMargins(t.px(20), 0, t.px(16), t.px(12));
    sideLayout->addWidget(product);
    sideLayout->addWidget(version);
    sideLayout->addWidget(m_nav, 1);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QString::fromLatin1("tsmediaSettingsPages"));

    // Each page: its name, one line about it, then its cards. "&" in a page name is a plain "&" (list items have no
    // access keys).
    addPage(Tab::Chat, i18n::t("Chat"), i18n::t("How TeamSpeak and the chat look to you. Others see them as before.")); // 2.7 dark mode
    addPage(Tab::EmojiGifs, i18n::t("Emoji & GIFs"), i18n::t("Emoji in the chat, the emoji picker and the GIFs you send."));
    addPage(Tab::Sending, i18n::t("Sending"),
            i18n::t("Files you send are stored in the channel's file browser, where anyone allowed to download can get them. People with TS Media "
                    "chat see them in the chat."));
    addPage(Tab::ReceivingPlayback, i18n::t("Receiving & playback"), i18n::t("Pictures, videos and files that others send, and how they play."));
    addPage(Tab::LinkPreviews, i18n::t("Link previews"),
            i18n::t("Previews for web links. Loading a preview from a website shows that website your IP address."));
    addPage(Tab::Privacy, i18n::t("Privacy"), i18n::t("What people learn from TS Media, the chat proxy, and where to limit link previews."));
    addPage(Tab::Servers, i18n::t("Servers"), i18n::t("Settings for single servers, and TS Media's group on servers you manage."));
    addPage(Tab::About, i18n::t("About & updates"), i18n::t("Version %1. Updates, and help for bug reports.").arg(QString::fromLatin1(TSMEDIA_VERSION)));

    // ---- Receiving --------------------------------------------------------------------------------------------------
    m_receiveCard    = SettingsSection::card(i18n::t("Receiving"), nullptr);
    m_inlinePreviews = SettingsSection::switchRow(m_receiveCard, i18n::t("&Show images, videos and file cards in the chat"), QString());
    // 2.2 data saver, at the top of what the first switch covers.
    m_dataSaver = SettingsSection::switchRow(m_receiveCard, i18n::t("&Data saver: pause automatic downloads"),
                                             i18n::t("Images, videos and voice messages load when you click them; small previews still load. Your "
                                                     "limits below are kept. You can turn it off for single servers under Servers."),
                                             1);
    m_autoDownloadMax = spin(Settings::autoDownloadMaxMBRange, 5, megabytes(), nullptr);
    m_autoDownloadMax->setAccessibleName(i18n::t("Download images, GIFs and voice messages automatically up to")); // 2.2 voice
    m_autoDownload = SettingsSection::switchRow(m_receiveCard, i18n::t("Download &images, GIFs and voice messages automatically up to"), QString(), 1,
                                                m_autoDownloadMax); // 2.2 voice
    m_autoplayGifs = SettingsSection::switchRow(m_receiveCard, i18n::t("Play &GIFs automatically"),
                                                i18n::t("When this is off, or animations are turned off in Windows, GIFs play only while the pointer "
                                                        "is over them."),
                                                1);
    m_autoplayGifs->setToolTip(m_autoplayGifs->accessibleDescription()); // 2.6's tool tip, the helper text (review fix)
    m_gifHint = hint(i18n::t("Windows animations are turned off, so GIFs play only while the pointer is over them."), nullptr);
    SettingsSection::customRow(m_receiveCard, m_gifHint, 1);
    m_videoAutoDownload = spin(Settings::videoAutoDownloadMBRange, 10, megabytes(), nullptr);
    m_videoAutoDownload->setPrefix(i18n::t("up to "));
    m_videoAutoDownload->setSpecialValueText(i18n::t("Off (download when played)")); // replaces prefix and suffix at 0
    m_videoAutoDownload->setMinimumWidth(m_videoAutoDownload->fontMetrics().horizontalAdvance(m_videoAutoDownload->specialValueText()) + t.px(48));
    SettingsSection::fieldRow(m_receiveCard, i18n::t("Download &videos and audio automatically"), m_videoAutoDownload, QString(), 1); // 2.2 audio
    insertCard(Tab::ReceivingPlayback, m_receiveCard);

    // ---- 2.5 zoom: Preview size (2.7: a card of its own; it follows "Show images, videos and file cards") ------------
    m_previewCard  = SettingsSection::card(i18n::t("Preview size"), nullptr);
    m_previewScale = new QComboBox(m_previewCard);
    m_previewScale->addItem(i18n::t("Small"), 85);
    m_previewScale->addItem(i18n::t("Normal"), 100);
    m_previewScale->addItem(i18n::t("Large"), 125);
    m_previewScale->addItem(i18n::t("Larger"), 150);
    m_previewScale->setAccessibleName(i18n::t("Preview size"));
    SettingsSection::fieldRow(m_previewCard, i18n::t("Previe&w size"), m_previewScale,
                              i18n::t("Previews also grow with Windows display scaling and with the chat's text size (Chat \xE2\x86\x92 Text size and "
                                      "colors, Ctrl + mouse wheel, or TeamSpeak's chat font)."));
    m_previewScale->setToolTip(m_previewScale->accessibleDescription());
    m_previewWidth = spin(Settings::previewMaxWidthRange, 20, pixels(), nullptr);
    m_previewWidth->setAccessibleName(i18n::t("Maximum preview width"));
    m_previewHeight = spin(Settings::previewMaxHeightRange, 20, pixels(), nullptr);
    m_previewHeight->setAccessibleName(i18n::t("Maximum preview height"));
    auto* times = new QLabel(i18n::t("×"));
    QLabel* previewSizeLabel = SettingsSection::fieldRow(m_previewCard, i18n::t("Maximum preview si&ze"), inRow({m_previewWidth, times, m_previewHeight}, t.px(6)),
                                                         i18n::t("The largest picture or video at Normal size and 100% display scaling."));
    previewSizeLabel->setBuddy(m_previewWidth);
    m_previewWidth->setAccessibleDescription(i18n::t("The largest picture or video at Normal size and 100% display scaling."));
    insertCard(Tab::ReceivingPlayback, m_previewCard);

    // ---- Playback ---------------------------------------------------------------------------------------------------
    QGroupBox* playback = SettingsSection::card(i18n::t("Playback"), nullptr);
    m_volume            = new QSlider(Qt::Horizontal);
    m_volume->setRange(Settings::videoVolumeRange.min, Settings::videoVolumeRange.max);
    m_volume->setSingleStep(5);
    m_volume->setPageStep(10);
    m_volume->setAccessibleName(i18n::t("Default volume"));
    m_volume->setMinimumWidth(t.px(180));
    m_volumeLabel = new QLabel;
    m_volumeLabel->setMinimumWidth(m_volumeLabel->fontMetrics().horizontalAdvance(QString::fromLatin1("100%")) + t.px(6));
    QLabel* volumeLabel = SettingsSection::fieldRow(playback, i18n::t("Default vol&ume"), inRow({m_volume, m_volumeLabel}, t.px(8)),
                                                    i18n::t("Changing the volume in the viewer updates this too."));
    volumeLabel->setBuddy(m_volume);
    m_volume->setToolTip(i18n::t("Changing the volume in the viewer updates this too."));
    m_volume->setAccessibleDescription(m_volume->toolTip());
    m_startMuted = SettingsSection::switchRow(playback, i18n::t("Start videos &muted"), QString());
    m_loop       = SettingsSection::switchRow(playback, i18n::t("Loo&p videos"), QString());
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) { m_volumeLabel->setText(i18n::t("%1%").arg(value)); });
    insertCard(Tab::ReceivingPlayback, playback);

    // ---- Media cache (2.7: the last card of Receiving & playback) ---------------------------------------------------
    QGroupBox* cache = SettingsSection::card(i18n::t("Media cache"), nullptr);
    m_cacheLimit     = spin(Settings::cacheLimitMBRange, 256, megabytes(), nullptr);
    SettingsSection::fieldRow(cache, i18n::t("Siz&e limit"), m_cacheLimit, i18n::t("Media you haven't opened for the longest time is removed first."));
    auto* openCache = new QPushButton(i18n::t("&Open folder"));
    openCache->setAccessibleName(i18n::t("Open media cache folder"));
    m_clearCache = new QPushButton(i18n::t("&Clear cache"));
    m_clearCache->setAccessibleName(i18n::t("Clear media cache"));
    openCache->setAutoDefault(false);
    m_clearCache->setAutoDefault(false);
    m_cacheLabel = SettingsSection::fieldRow(cache, QString(), inRow({openCache, m_clearCache}, t.px(8)), QString());
    m_cacheNotice = new QTimer(this);
    m_cacheNotice->setSingleShot(true);
    insertCard(Tab::ReceivingPlayback, cache, true);

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

    // ---- Sending ----------------------------------------------------------------------------------------------------
    QGroupBox* send = SettingsSection::card(i18n::t("Sending"), nullptr);
    m_dragDrop      = SettingsSection::switchRow(send, i18n::t("Send &files dropped on the chat (hold Shift for TeamSpeak's own drop)"), QString());
    m_dragDrop->setObjectName(QString::fromLatin1("dropSendsFiles")); // 2.2 compose: its drop-mode setting follows this
    m_paste = SettingsSection::switchRow(send, i18n::t("Send screenshots and files pas&ted into the chat input (Ctrl+V)"), QString());
    m_jpeg  = SettingsSection::switchRow(send, i18n::t("Convert pasted images over 2 MB to &JPEG"), i18n::t("Images with transparency stay PNG."));
    m_jpeg->setToolTip(m_jpeg->accessibleDescription());
    m_previews  = SettingsSection::switchRow(send, i18n::t("Upload a small previe&w with large images and videos"),
                                            i18n::t("Others see a sharp preview while the full file downloads."));
    m_uploadMax = spin(Settings::uploadMaxMBRange, 10, megabytes(), nullptr);
    m_uploadMax->setAccessibleName(i18n::t("Upload size limit"));
    SettingsSection::fieldRow(send, i18n::t("Upload size &limit"), m_uploadMax, QString());
    m_uploadDir = new QLineEdit;
    m_uploadDir->setPlaceholderText(QString::fromLatin1(Settings::defaultUploadDirectory));
    m_uploadDir->setMaxLength(255);
    SettingsSection::stackedRow(send, i18n::t("Upload fol&der"), m_uploadDir,
                                i18n::t("A folder in the channel's file browser, created when needed. Type / for the top level."));
    insertCard(Tab::Sending, send);

    // ---- Note for people without the plugin (2.7: the last card of Sending) ------------------------------------------
    QGroupBox* note = SettingsSection::card(i18n::t("Note for people without the plugin"), nullptr);
    m_notice        = SettingsSection::switchRow(note, i18n::t("Add a &note after files you send"),
                                          i18n::t("People without the plugin see “TS Media chat plugin required to view this in chat” after the link."));
    m_downloadUrl   = new QLineEdit;
    m_downloadUrl->setPlaceholderText(QString::fromLatin1(Settings::defaultDownloadUrl));
    m_downloadUrl->setMaxLength(Settings::maxDownloadUrlLength);
    m_downloadUrlHint = i18n::t("“TS Media chat” in the note links here. Leave empty for the official page.");
    // Under the field: a warning icon with the message, so it doesn't depend on colour, then the helper.
    m_downloadUrlError  = new QWidget;
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
    SettingsSection::stackedRow(note, i18n::t("Lin&k in the note"), m_downloadUrl, m_downloadUrlHint, 1, m_downloadUrlError);
    insertCard(Tab::Sending, note, true);

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

    // ---- 2.2 per-server settings: the Servers section (self-contained, see serverssection.h) ------------------------
    m_servers = new ServersSection(this);

    // ---- the bar: Restore defaults on the left, the unsaved note and OK / Cancel / Apply on the right ----------------
    auto* bar = new QWidget(this);
    bar->setObjectName(QString::fromLatin1("tsmediaSettingsBar"));
    // Two boxes: a button box takes its buttons back into its own layout, so the note could not sit between them.
    m_restoreBox = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults, bar);
    m_restoreBox->setObjectName(QString::fromLatin1("tsmediaSettingsRestore"));
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, bar);
    m_buttons->setObjectName(QString::fromLatin1("tsmediaSettingsButtons"));
    m_buttons->button(QDialogButtonBox::Ok)->setText(i18n::t("OK"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(i18n::t("Cancel"));
    m_applyButton = m_buttons->button(QDialogButtonBox::Apply);
    m_applyButton->setText(i18n::t("&Apply"));
    m_applyButton->setAutoDefault(false);
    QPushButton* restore = m_restoreBox->button(QDialogButtonBox::RestoreDefaults);
    restore->setText(i18n::t("&Restore defaults"));
    restore->setAutoDefault(false);
    restore->setToolTip(i18n::t("Sets every page back to its defaults. Nothing is saved until you click OK or Apply."));
    m_unsaved = hint(i18n::t("Not saved yet: OK or Apply saves your changes."), bar);
    m_unsaved->setWordWrap(false);
    m_unsaved->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_unsaved->hide();
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(t.px(16), t.px(12), t.px(16), t.px(14));
    barLayout->addWidget(m_restoreBox);
    barLayout->addStretch(1);
    barLayout->addWidget(m_unsaved);
    barLayout->addSpacing(t.px(12));
    barLayout->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] { okClicked(); });
    connect(m_buttons, &QDialogButtonBox::rejected, this, [this] { discardAndClose(); }); // Cancel never asks (M2)
    connect(m_applyButton, &QPushButton::clicked, this, [this] { apply(); });
    // Only fills in the form: nothing is saved until OK or Apply, and Cancel undoes it. Sections keep
    // what Restore defaults must not touch (SettingsSection::restoreDefaults; the chat text size is no form value).
    connect(restore, &QPushButton::clicked, this, [this] {
        load(Settings(), false);
        m_loading = true;
        for (SettingsSection* section : qAsConst(m_sections))
            section->restoreDefaults(Settings());
        m_loading = false;
        setDirty(true);
    });

    auto* middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(0);
    middle->addWidget(side);
    middle->addWidget(m_stack, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(middle, 1);
    layout->addWidget(bar);

    // 2.2 updater: a self-contained group with its own keys (apply() below; Restore defaults leaves them alone). 2.6
    // chat proxy (design 1.3.10): its own keys too (OK, Apply and Restore defaults never touch them; its button acts at
    // once). Both added before the dirty tracking below, so the updates switch enables Apply.
    addSection(Tab::Privacy, new proxy::ui::ProxyGroup(this));
    addSection(Tab::About, new upd::UpdateSettingsGroup(this));
    // ---- 2.2 diagnostics: Troubleshooting (2.7: on About & updates, below Updates) -----------------------------------
    QGroupBox* troubleshooting = SettingsSection::card(i18n::t("Troubleshooting"), nullptr);
    auto*      diagnostics     = new DiagnosticsButton;
    diagnostics->setText(i18n::t("&Diagnostic info…"));
    SettingsSection::fieldRow(troubleshooting, i18n::t("Information for a bug report"), diagnostics,
                              i18n::t("Versions, settings and recent messages to paste into a bug report. Nothing is sent by the plugin."));
    insertCard(Tab::About, troubleshooting, true);
    troubleshooting->setVisible(diag::hasDialogOpener()); // the window only exists inside TeamSpeak
    // 2.2 per-server settings: loaded and saved with the form (load() / apply()).
    addSection(Tab::Servers, m_servers);
    // 2.2 servergroup: Server access, a self-contained box (its actions act on the server right away).
    addSection(Tab::Servers, new AccessGroupBox(this));

    connect(m_nav, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        m_stack->setCurrentIndex(row);
        g_lastTab = row;
    });
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
    // 2.5 zoom live test: the Preview size combo box makes the form changed too (OK applies only a changed form, so a
    // new size alone was lost).
    connect(m_previewScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this, changed);
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

    // 2.7 F3 (L10): Ctrl+Tab / Ctrl+PgDown and Ctrl+Shift+Tab / Ctrl+PgUp switch pages from anywhere in the window.
    const auto pageKey = [this](const QKeySequence& keys, int direction) {
        auto* shortcut = new QShortcut(keys, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, direction] { stepPage(direction); });
    };
    pageKey(QKeySequence(static_cast<int>(Qt::CTRL) | static_cast<int>(Qt::Key_Tab)), +1);
    pageKey(QKeySequence(static_cast<int>(Qt::CTRL) | static_cast<int>(Qt::Key_PageDown)), +1);
    pageKey(QKeySequence(static_cast<int>(Qt::CTRL) | static_cast<int>(Qt::SHIFT) | static_cast<int>(Qt::Key_Backtab)), -1);
    pageKey(QKeySequence(static_cast<int>(Qt::CTRL) | static_cast<int>(Qt::Key_PageUp)), -1);

    // TeamSpeak's dark skins switch off every focus indicator. Like Windows, the dialog shows its own
    // focus rings only once the keyboard has been used to move around in it. 2.7 F3: and a control reached with Tab
    // scrolls into view on its page.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!now || now->window() != this)
            return;
        for (QScrollArea* area : qAsConst(m_pageAreas)) {
            if (area->widget() && area->widget()->isAncestorOf(now))
                area->ensureWidgetVisible(now, m_tokens.px(16), m_tokens.px(16));
        }
        if (m_keyboardFocus || !testAttribute(Qt::WA_KeyboardFocusChange))
            return;
        m_keyboardFocus = true;
        applyTheme();
    });

    // Walks the whole cache folder: once, not on every change of the limit.
    m_cacheUsed = m_core ? m_core->cacheSize() : 0;
    m_loaded    = Settings::instance();
    load(m_loaded);
    reloadServers(); // 2.2 per-server settings
    updateGifHint();
    setDirty(false);
    m_ready = true;
    applyTheme();
    rebuildTabOrder();
}

QWidget* SettingsDialog::addPage(Tab tab, const QString& title, const QString& description)
{
    const settingstheme::Tokens& t    = m_tokens;
    auto*                        area = new QScrollArea(m_stack);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); // H1: only a safety net (rows wrap and stack)
    area->setFocusPolicy(Qt::NoFocus); // no Tab stop of its own
    area->setAccessibleName(title);    // a screen reader names the page when the focus enters it
    area->viewport()->setAutoFillBackground(false);
    auto* page = new QWidget(area);
    page->setObjectName(QString::fromLatin1("tsmediaSettingsPage")); // matched by the style sheet
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(t.px(28), t.px(22), t.px(28), t.px(24));
    pageLayout->setSpacing(t.px(18));
    auto* header       = new QWidget(page);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(t.px(4));
    auto* name = new QLabel(title, header);
    name->setObjectName(QString::fromLatin1("tsmediaSettingsPageTitle"));
    name->setTextFormat(Qt::PlainText);
    name->setFont(scaled(font(), 1.6, QFont::DemiBold));
    name->setAccessibleName(title);
    headerLayout->addWidget(name);
    headerLayout->addWidget(hint(description, header));
    pageLayout->addWidget(header);
    pageLayout->addStretch(1); // cards go above it
    area->setWidget(page);
    page->setAutoFillBackground(false); // setWidget() turned it on
    m_stack->addWidget(area);
    m_pages.append(page);
    m_pageAreas.append(area);
    m_lastCards.append(nullptr);

    auto* item = new QListWidgetItem(title, m_nav);
    item->setData(Qt::UserRole, static_cast<int>(iconOf(tab)));
    m_nav->setRowHidden(m_nav->count() - 1, true); // shown when it gets content
    return page;
}

QVBoxLayout* SettingsDialog::pageLayout(Tab tab) const
{
    return static_cast<QVBoxLayout*>(m_pages.at(static_cast<int>(tab))->layout());
}

void SettingsDialog::insertCard(Tab tab, QWidget* card, bool last)
{
    QVBoxLayout* layout = pageLayout(tab);
    const int    page   = static_cast<int>(tab);
    int          at     = layout->count() - 1; // above the stretch
    if (QWidget* tail = m_lastCards.at(page))
        at = layout->indexOf(tail);
    layout->insertWidget(at, card);
    if (last)
        m_lastCards[page] = card;
    m_nav->setRowHidden(page, false);
}

void SettingsDialog::addSection(Tab tab, QWidget* widget, bool atTop)
{
    if (!widget)
        return;
    if (atTop) {
        pageLayout(tab)->insertWidget(1, widget); // right under the page's header
        m_nav->setRowHidden(static_cast<int>(tab), false);
    } else {
        insertCard(tab, widget);
    }
    if (auto* section = qobject_cast<SettingsSection*>(widget)) {
        m_sections.append(section);
        m_loading = true;
        section->load(m_loaded);
        m_loading = false;
        connect(section, &SettingsSection::changed, this, [this] {
            if (!m_loading)
                setDirty(true);
        });
        // 2.7 F3 (M10): a section's link to another page (Privacy → Link previews) selects it and focuses its first control.
        connect(section, &SettingsSection::showPage, this, [this](int page) {
            if (page < 0 || page >= m_pages.size())
                return;
            showTab(static_cast<Tab>(page));
            QList<QWidget*> controls;
            collectFocusable(m_pages.at(page)->layout(), controls);
            for (QWidget* w : qAsConst(controls)) {
                if (w->isVisibleTo(m_pages.at(page)) && w->isEnabled()) {
                    w->setFocus(Qt::OtherFocusReason);
                    break;
                }
            }
        });
    }
    if (m_ready) {
        applyTheme(); // its helper texts and focus rings
        rebuildTabOrder();
    }
}

void SettingsDialog::showTab(Tab tab)
{
    const int row = static_cast<int>(tab);
    if (row >= 0 && row < m_nav->count() && !m_nav->isRowHidden(row))
        m_nav->setCurrentRow(row);
}

SettingsDialog::Tab SettingsDialog::currentTab() const
{
    return static_cast<Tab>(qMax(0, m_nav->currentRow()));
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

void SettingsDialog::stepPage(int direction)
{
    const int count = m_nav->count();
    int       row   = m_nav->currentRow();
    for (int i = 0; i < count; ++i) {
        row = ((row < 0 ? (direction > 0 ? -1 : 0) : row) + direction + count) % count;
        if (!m_nav->isRowHidden(row)) {
            m_nav->setCurrentRow(row);
            return;
        }
    }
}

// Tab goes: the sidebar, the page's controls in reading order, Restore defaults, OK, Cancel, Apply. Rebuilt whenever a
// section is added: Qt's own order is the order the widgets were made, and plugin.cpp adds sections after the buttons.
void SettingsDialog::rebuildTabOrder()
{
    QList<QWidget*> chain;
    chain << m_nav;
    for (QWidget* page : qAsConst(m_pages))
        collectFocusable(page->layout(), chain);
    chain << m_restoreBox->button(QDialogButtonBox::RestoreDefaults) << m_buttons->button(QDialogButtonBox::Ok)
          << m_buttons->button(QDialogButtonBox::Cancel) << m_applyButton;
    for (int i = 1; i < chain.size(); ++i)
        QWidget::setTabOrder(chain.at(i - 1), chain.at(i));
}

QSize SettingsDialog::minimumSizeFor(const QSize& available) const
{
    const settingstheme::Tokens& t       = m_tokens;
    QSize                        minimum = QSize(m_nav->parentWidget()->minimumWidth() + t.px(460), t.px(420)).expandedTo(layout()->minimumSize());
    if (available.isValid())
        minimum = minimum.boundedTo(available * 0.92);
    return minimum;
}

QSize SettingsDialog::initialSizeFor(const QSize& available) const
{
    // 2.7 F3 (H1): 900 x 640 at 100 %, in TeamSpeak's font size, within 92 % of the screen TeamSpeak is on. Every page
    // scrolls on its own, so the window never changes size when the page does.
    QSize size = g_lastSize.isValid() && qFuzzyCompare(g_lastSizeK, m_tokens.k) ? g_lastSize : QSize(m_tokens.px(900), m_tokens.px(640));
    size       = size.expandedTo(minimumSizeFor(available));
    if (available.isValid())
        size = size.boundedTo(available * 0.92);
    return size;
}

void SettingsDialog::initialSize()
{
    QSize available;
    if (const QScreen* screen = parentWidget() ? parentWidget()->screen() : this->screen())
        available = screen->availableGeometry().size();
    setMinimumSize(minimumSizeFor(available));
    resize(initialSizeFor(available));
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    // Before QDialog::showEvent, which centres the dialog with this size.
    if (!event->spontaneous() && !m_sized) {
        m_sized = true;
        applyTheme();
        initialSize();
        // The page used last (or the one showTab() asked for), if it has content; else the first that has.
        if (m_nav->currentRow() < 0)
            showTab(static_cast<Tab>(g_lastTab));
        if (m_nav->currentRow() < 0 || m_nav->isRowHidden(m_nav->currentRow()))
            stepPage(+1);
    }
    QDialog::showEvent(event);
}

void SettingsDialog::hideEvent(QHideEvent* event)
{
    if (!event->spontaneous() && m_sized && !isMinimized()) {
        g_lastSize  = size(); // the next window opens at it (this session)
        g_lastSizeK = m_tokens.k;
    }
    QDialog::hideEvent(event);
}

// Tab from the bar's last button (Apply, or Cancel while Apply is greyed out) goes on to the sidebar, and Shift+Tab
// from the sidebar back to it. Qt's own walk can't be trusted there: QDialogButtonBox makes its first button its focus
// proxy whenever it lays its buttons out, so the walk from Apply met a button box and landed on Restore defaults, and
// Tab went round the bar for good (live test 2.7, F3-2).
bool SettingsDialog::focusNextPrevChild(bool next)
{
    QWidget* last = m_applyButton && m_applyButton->isVisible() && m_applyButton->isEnabled() ? m_applyButton : nullptr;
    if (!last && m_buttons)
        last = m_buttons->button(QDialogButtonBox::Cancel);
    QWidget* const focus = focusWidget();
    if (m_nav && last && focus) {
        // A focused Apply that was just greyed out counts as the last stop too (Qt moves the focus on with this walk).
        if (next && (focus == last || focus == m_applyButton)) {
            m_nav->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (!next && focus == m_nav) {
            last->setFocus(Qt::BacktabFocusReason);
            return true;
        }
    }
    return QDialog::focusNextPrevChild(next);
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
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
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

// ---- 2.7 F3 (M2): closing with unsaved changes asks -------------------------------------------------------------------
bool SettingsDialog::hasUnsavedChanges() const
{
    return m_applyButton && m_applyButton->isEnabled();
}

void SettingsDialog::reject()
{
    if (!hasUnsavedChanges()) {
        QDialog::reject();
        return;
    }
    if (m_question && m_question->isVisible()) {
        m_question->raise();
        m_question->activateWindow();
        return;
    }
    // Not QMessageBox's static helpers (their buttons follow TeamSpeak's language) and open(), not exec(): no nested
    // event loop, and the box goes away with the dialog when the plugin shuts down.
    auto* box = new QMessageBox(QMessageBox::Question, windowTitle(), i18n::t("Save your changes?"), QMessageBox::NoButton, this);
    box->setObjectName(QString::fromLatin1("tsmediaSettingsSaveQuestion"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setLayoutDirection(Qt::LeftToRight);
    box->setInformativeText(i18n::t("Some of your changes aren't saved yet."));
    QPushButton* save    = box->addButton(i18n::t("Save"), QMessageBox::AcceptRole);
    QPushButton* discard = box->addButton(i18n::t("Discard"), QMessageBox::DestructiveRole);
    QPushButton* cancel  = box->addButton(i18n::t("Cancel"), QMessageBox::RejectRole);
    box->setDefaultButton(save);
    box->setEscapeButton(cancel);
    connect(save, &QPushButton::clicked, this, [this] { okClicked(); }); // as OK: a problem in a field is shown, the window stays
    connect(discard, &QPushButton::clicked, this, [this] { discardAndClose(); });
    m_question = box;
    box->open();
}

void SettingsDialog::discardAndClose()
{
    if (m_question)
        m_question->close();
    QDialog::reject();
}

void SettingsDialog::okClicked()
{
    if (!m_applyButton->isEnabled() || apply())
        accept();
}

// Windows animations off: inline GIFs play only on hover, whatever "Play GIFs automatically" says.
void SettingsDialog::updateGifHint()
{
    const bool show = !ui::animationsEnabled();
    m_autoplayGifs->setAccessibleDescription(show ? m_gifHint->text() : m_autoplayGifs->toolTip());
    if (SettingRow* row = SettingsSection::rowOf(m_gifHint))
        row->setVisible(show);
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
    m_previewScale->setCurrentIndex(qMax(0, m_previewScale->findData(previewscale::nearestPreset(s.previewScale)))); // 2.5 zoom
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
    // Review fix: Apply greys itself out when it saved, mostly while it has the focus (a click, Space, Enter). Qt would
    // then walk on from it through the button box to Restore defaults, where the next Enter (the habit "Apply, then
    // Enter for OK") reset every page. OK takes the focus first, so that Enter is OK.
    if (!dirty && focusWidget() == m_applyButton) {
        if (QPushButton* ok = m_buttons->button(QDialogButtonBox::Ok))
            ok->setFocus(Qt::OtherFocusReason);
    }
    m_applyButton->setEnabled(dirty);
    m_unsaved->setVisible(dirty); // 2.7 F3: switches usually act at once; here OK or Apply saves them
}

void SettingsDialog::updateEnabled()
{
    // 2.7 F3: a row greys out as a whole (title, helper, control); ticks and values are kept.
    const bool show = m_inlinePreviews->isChecked();
    // 2.2 data saver: the automatic-download limits are kept, they just don't apply while it is on.
    const bool    saving = m_dataSaver->isChecked();
    const QString paused = saving ? i18n::t("Paused by data saver") : QString();
    SettingsSection::setRowEnabled(m_dataSaver, show);
    SettingsSection::setRowEnabled(m_autoDownload, show && !saving);
    m_autoDownloadMax->setEnabled(show && !saving && m_autoDownload->isChecked());
    SettingsSection::setRowEnabled(m_autoplayGifs, show);
    SettingsSection::setRowEnabled(m_gifHint, show);
    SettingsSection::setRowEnabled(m_videoAutoDownload, show && !saving);
    m_previewCard->setEnabled(show);
    for (QWidget* field : {static_cast<QWidget*>(m_autoDownload), static_cast<QWidget*>(m_autoDownloadMax), static_cast<QWidget*>(m_videoAutoDownload)}) {
        if (field->toolTip() != paused)
            field->setToolTip(paused);
    }
    SettingsSection::setRowEnabled(m_downloadUrl, m_notice->isChecked());
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
        showTabOf(m_downloadUrl); // OK may have been pressed on another page
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
    form.previewScale        = previewscale::nearestPreset(m_previewScale->currentData().toInt()); // 2.5 zoom
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
    // value from when this dialog opened must not undo that. (2.7: the chat text size isn't in the form at all: the
    // chat saves it.)
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
    take(s.previewScale, m_loaded.previewScale, form.previewScale); // 2.5 zoom
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
    QString details = i18n::t("Files on the server aren't affected: media downloads again when you view it, unless it was deleted "
                              "from the server. Media that's playing or open in the viewer is kept.");
    // 2.2 protocol: Clear cache also forgets the reactions kept on this computer (Core::cacheCleared).
    if (PeerHub::instance())
        details += QLatin1Char(' ') + i18n::t("The reactions saved on this computer are removed too; people with TS Media share theirs again "
                                              "when you are in a channel with them.");
    // 2.5 GIFs: the GIFs tab's list is outside the cache (nobody clearing traces should be surprised that it stays).
    details += QLatin1Char(' ') + i18n::t("Your saved GIFs are kept; remove them in the emoji picker's GIFs tab or in Settings → Emoji & GIFs.");
    box->setInformativeText(details);
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

// 2.7 F3 (design_settings 3.3, 3.6): the tokens from the polished palette and the skin's button colour, and one style
// sheet scoped to this dialog, always set: cards, the sidebar, label roles, transparent containers (a dark skin paints
// every QWidget grey), and the 2.6 dark-skin fixes for fields and buttons. Built with fromLatin1 and arg() only, never
// QStringLiteral: TeamSpeak's QStyleSheetStyle keeps the parsed text after the plugin is unloaded (see settings.cpp).
void SettingsDialog::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    ensurePolished();

    QColor skinButton;
    if (QPushButton* ok = m_buttons ? m_buttons->button(QDialogButtonBox::Ok) : nullptr) {
        ok->ensurePolished();
        if (!qApp->styleSheet().isEmpty()) // a skin's own button colour (the native style's grey isn't one)
            skinButton = ok->palette().color(QPalette::Active, QPalette::Button);
    }
    // 2.7 dark mode (design_dark 1.11): its own tokens while it is on; the application sheet gives fields, buttons and focus.
    const bool darkMode = DarkMode::isActive();
    m_tokens            = darkMode ? darkpalette::settingsTokens(settingstheme::sizeFactor(font(), this))
                                   : settingstheme::tokens(palette(), skinButton, settingstheme::sizeFactor(font(), this));
    const settingstheme::Tokens& t = m_tokens;

    QColor error = t.dark ? QColor(0xFF, 0x99, 0xA4) : QColor(0xC4, 0x2B, 0x1C);
    if (ui::contrastRatio(error, t.card) < 4.5 || ui::contrastRatio(error, t.window) < 4.5)
        error = t.text; // the icon still marks it
    QColor ok = settingstheme::roleColors(palette()).ok;
    if (ui::contrastRatio(ok, t.card) < 4.5)
        ok = t.text; // the check mark still marks it

    // The new rules are scoped to the window's three parts (the pages, the sidebar, the bar), so they don't reach the
    // windows the dialog opens (the Chat proxy window, Server access details, the message boxes): those keep their own
    // look. The 2.6 dark-skin rules for fields and buttons below stay dialog-wide, as in 2.6.
    const QString n     = QString::fromLatin1("#tsmediaSettingsDialog");
    const QString pages = QString::fromLatin1("#tsmediaSettingsPages");
    const QString side  = QString::fromLatin1("#tsmediaSettingsSide");
    const QString bar   = QString::fromLatin1("#tsmediaSettingsBar");
    const auto    parts = [&](const char* rule) { // "rule" under each of the three parts
        const QString r = QString::fromLatin1(rule);
        return QString::fromLatin1("%1 %4,%2 %4,%3 %4").arg(pages, side, bar, r);
    };
    const int em = QFontMetrics(font()).height();
    QString   sheet;
    sheet += QString::fromLatin1("QStackedWidget%1{background:transparent;}"
                                 "%1 .QWidget,%1 QLabel,%1 QScrollArea,%1 QScrollArea>QWidget>QWidget{background:transparent;}"
                                 "%1 QScrollArea{border:none;}")
                 .arg(pages);
    sheet += QString::fromLatin1("QWidget%1{background:%3;}%1 QLabel{background:transparent;}"
                                 "QWidget%2{background:%4;border-top:1px solid %5;}%2 QLabel{background:transparent;}"
                                 "%1 QListWidget#tsmediaSettingsNav{background:transparent;border:none;outline:none;}")
                 .arg(side, bar, t.sidebar.name(), t.window.name(), t.border.name());
    sheet += QString::fromLatin1("%1 QGroupBox{background:%2;border:1px solid %3;border-radius:%4px;margin-top:%5px;padding:%6px %7px;font-weight:600;}"
                                 "%1 QGroupBox[tsmediaRows=\"true\"]{padding:0px;}"
                                 "%1 QGroupBox::title{subcontrol-origin:margin;subcontrol-position:top left;left:%8px;top:0px;padding:0px;color:%9;}"
                                 "%1 QGroupBox QWidget{font-weight:normal;}")
                 .arg(pages, t.card.name(), t.border.name())
                 .arg(t.px(8))
                 .arg(em + t.px(10))
                 .arg(t.px(10))
                 .arg(t.px(14))
                 .arg(t.px(2))
                 .arg(t.text.name());
    sheet += parts("QLabel") + QString::fromLatin1("{color:%1;}").arg(t.text.name());
    sheet += parts("QLabel[role=\"hint\"]") + QString::fromLatin1("{color:%1;}").arg(t.muted.name());
    sheet += parts("QLabel[role=\"error\"]") + QString::fromLatin1("{color:%1;}").arg(error.name());
    sheet += parts("QLabel[role=\"ok\"]") + QString::fromLatin1("{color:%1;}").arg(ok.name());
    sheet += parts("QLabel:disabled") + QString::fromLatin1(",") + parts("QLabel[role=\"hint\"]:disabled")
             + QString::fromLatin1("{color:%1;}").arg(t.disabled.name());
    // A button that leads elsewhere (Privacy: "Link previews and your IP address…") looks like a link, as Diagnostic info…
    // does: the accent made 4.5:1 on the card, underlined on hover, a ring with the keyboard focus.
    const QColor link = ui::readableOn(t.accent, {t.card}, 4.5);
    sheet += QString::fromLatin1("%1 QPushButton[tsmediaLink=\"true\"]{background:transparent;border:1px solid transparent;border-radius:3px;"
                                 "padding:0px %2px;margin:0px;color:%3;font-weight:normal;min-width:0px;min-height:0px;}"
                                 "%1 QPushButton[tsmediaLink=\"true\"]:hover{text-decoration:underline;}"
                                 // Its own border and padding with the focus too: the dark skins' focus ring for buttons (below, as
                                 // specific as the rule above and later) would make it 12 px taller and move the card (review fix).
                                 "%1 QPushButton[tsmediaLink=\"true\"]:pressed,%1 QPushButton[tsmediaLink=\"true\"]:focus{color:%4;border:1px solid %4;padding:0px %2px;}")
                 .arg(pages)
                 .arg(t.px(4))
                 .arg(link.name(), t.text.name());
    if (t.dark && !qApp->styleSheet().isEmpty() && !darkMode) { // 2.7: dark mode's own sheet needs none of these
        // Dark skins draw disabled text at about 1.2:1. Keep it dimmed, but readable.
        sheet += QString::fromLatin1("%1 QAbstractSpinBox:disabled,%1 QLineEdit:disabled,%1 QComboBox:disabled{color:%2;}").arg(n, t.disabled.name());
        // They also turn every focus indicator off. The field being typed in gets an accent border (the colour of the
        // skin's buttons, if it stands out from the field).
        const QColor field  = m_uploadDir->palette().color(QPalette::Active, QPalette::Base);
        QColor       accent = t.accent;
        if (ui::contrastRatio(accent, field) < 3.0)
            accent = t.text;
        sheet += QString::fromLatin1("%1 QLineEdit:focus,%1 QAbstractSpinBox:focus,%1 QComboBox:focus{border-color:%2;}"
                                     "%1 QSlider{border:1px solid transparent;border-radius:4px;}")
                     .arg(n, accent.name());
        if (m_keyboardFocus) {
            // Rings for the skin's buttons and the slider, sized so nothing moves: the button's border takes 2 px of the
            // skin's 8 x 14 px padding (the skin's own sizes: they don't follow the font).
            sheet += QString::fromLatin1("%1 QPushButton:focus{border:2px solid %2;padding:6px 12px;}%1 QSlider:focus{border-color:%2;}")
                         .arg(n, t.text.name());
        }
    }
    if (sheet != styleSheet())
        setStyleSheet(sheet);

    // The sidebar: as wide as its longest page name (semibold when chosen) and the product name.
    if (QWidget* sideWidget = m_nav ? m_nav->parentWidget() : nullptr) {
        QFont bold = font();
        bold.setWeight(QFont::DemiBold);
        const QFontMetrics fm(bold);
        int                widest = 0;
        for (int i = 0; i < m_nav->count(); ++i)
            widest = qMax(widest, fm.horizontalAdvance(m_nav->item(i)->text()));
        int width = widest + t.px(8 + 12 + 18 + 10 + 6 + 8 + 16);
        if (auto* product = sideWidget->findChild<QLabel*>(QString::fromLatin1("tsmediaSettingsProduct")))
            width = qMax(width, product->fontMetrics().horizontalAdvance(product->text()) + t.px(36));
        if (sideWidget->width() != width || sideWidget->minimumWidth() != width)
            sideWidget->setFixedWidth(width);
        m_nav->viewport()->update();
    }
    // Painted widgets take the new tokens.
    for (QWidget* w : findChildren<QWidget*>()) {
        if (qobject_cast<SwitchBox*>(w) || qobject_cast<LayoutTile*>(w) || qobject_cast<IconButton*>(w) || qobject_cast<LinePreview*>(w)
            || qobject_cast<SettingRow*>(w))
            w->update();
    }

    // The number fields share one width, so they line up.
    int fieldWidth = 0;
    for (QSpinBox* box : qAsConst(m_numberFields))
        fieldWidth = qMax(fieldWidth, box->sizeHint().width());
    for (QSpinBox* box : qAsConst(m_numberFields))
        box->setMinimumWidth(fieldWidth);

    // Live test 2.7 (F3-1): every combo box wide enough for its longest item. At 200 % (TeamSpeak's font 22 px) the
    // Windows style's arrow grows with the DPI more than QComboBox's size hint allows for, which cut "Open the send
    // window", "Balanced (720p)" and "Normal" short. The Servers card sizes its own fields (one shared width with the
    // fit in it, the server picker's own minimum): the reset here took those away (review fix).
    for (QComboBox* combo : findChildren<QComboBox*>()) {
        if (!m_servers || !m_servers->isAncestorOf(combo))
            fitComboText(combo);
    }
    if (m_servers)
        m_servers->updateWidths();

    m_applyingTheme = false;
}
