#include "settingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include "core.h"
#include "i18n.h"
#include "settings.h"
#include "version.h"

namespace {

// The range comes from Settings, so the dialog never offers a value Settings::load() would clamp.
QSpinBox* spin(Settings::Range range, const QString& suffix, QWidget* parent)
{
    auto* box = new QSpinBox(parent);
    box->setRange(range.min, range.max);
    box->setSuffix(suffix);
    box->setAccelerated(true);
    return box;
}

// Units stay Latin in both languages: spin boxes lay their text out left-to-right, where a Persian
// suffix would read backwards ("15 مگابایت"). formatSize() uses the same units.
QString megabytes()
{
    return QStringLiteral(" MB");
}

QString pixels()
{
    return QStringLiteral(" px");
}

// Keeps a number with its Latin unit ("12.5 MB", "4000 × 3000") in one piece inside Persian text;
// without it the bidi algorithm separates the digits from the unit.
QString ltr(const QString& text)
{
    return i18n::isPersian() ? QChar(0x200E) + text + QChar(0x200E) : text;
}

// Small grey explanation under an option.
QLabel* hint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.92);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::Disabled, QPalette::WindowText));
    label->setPalette(palette);
    return label;
}

QFormLayout* form(QGroupBox* group)
{
    auto* layout = new QFormLayout(group);
    layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    layout->setLabelAlignment(Qt::AlignLeading | Qt::AlignVCenter);
    return layout;
}

// QMessageBox's static helpers label Yes / No / OK through Qt's translations, which follow TeamSpeak's
// language (and Qt has no Persian one), not the plugin's. A message box is a window of its own, so it
// does not inherit the dialog's layout direction either. This one is in the plugin's language.
QMessageBox::StandardButton messageBox(QWidget* parent, QMessageBox::Icon icon, const QString& text, QMessageBox::StandardButtons buttons,
                                       QMessageBox::StandardButton defaultButton)
{
    QMessageBox box(icon, parent ? parent->windowTitle() : QStringLiteral(TSMEDIA_NAME), text, buttons, parent);
    box.setLayoutDirection(i18n::direction());
    if (QAbstractButton* yes = box.button(QMessageBox::Yes))
        yes->setText(i18n::t("&Yes", "بله"));
    if (QAbstractButton* no = box.button(QMessageBox::No))
        no->setText(i18n::t("&No", "خیر"));
    if (QAbstractButton* ok = box.button(QMessageBox::Ok))
        ok->setText(i18n::t("OK", "تأیید"));
    box.setDefaultButton(defaultButton);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

// Accepts "example.com/x" (assumes https) and http(s) URLs that are safe to put inside [URL=...].
QString normalizedDownloadUrl(const QString& input, bool* ok)
{
    *ok          = true;
    QString text = input.trimmed();
    if (text.isEmpty())
        return text;
    if (!text.contains(QLatin1String("://")))
        text.prepend(QStringLiteral("https://"));
    const QUrl    url(text, QUrl::StrictMode);
    const QString scheme  = url.scheme().toLower();
    const QString encoded = QString::fromLatin1(url.toEncoded());
    if (!url.isValid() || url.host().isEmpty() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")) || encoded.contains(QLatin1Char('['))
        || encoded.contains(QLatin1Char(']')) || encoded.length() > Settings::maxDownloadUrlLength) {
        *ok = false;
        return input;
    }
    return encoded;
}

} // namespace

SettingsDialog::SettingsDialog(Core* core, QWidget* parent)
    : QDialog(parent)
    , m_core(core)
{
    setObjectName(QStringLiteral("tsmediaSettingsDialog"));
    setWindowTitle(i18n::t(TSMEDIA_NAME " — Settings", TSMEDIA_NAME " — تنظیمات"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setLayoutDirection(i18n::direction());

    // ---- Receiving --------------------------------------------------------------------------------
    auto* receive       = new QGroupBox(i18n::t("Receiving", "دریافت"), this);
    m_inlinePreviews    = new QCheckBox(i18n::t("Show images, videos and file cards inside the chat", "نمایش تصاویر، ویدیوها و کارت فایل‌ها داخل چت"), receive);
    m_autoDownload      = new QCheckBox(i18n::t("Load images and GIFs automatically", "بارگذاری خودکار تصاویر و گیف‌ها"), receive);
    m_autoDownloadMax   = spin(Settings::autoDownloadMaxMBRange, megabytes(), receive);
    m_autoplayGifs      = new QCheckBox(i18n::t("Play GIFs automatically (otherwise while hovered)", "پخش خودکار گیف‌ها (وگرنه با بردن ماوس روی آن‌ها)"), receive);
    m_videoAutoDownload = spin(Settings::videoAutoDownloadMBRange, megabytes(), receive);
    m_videoAutoDownload->setSpecialValueText(i18n::t("Never (when I press play)", "فقط با زدن دکمهٔ پخش"));
    m_videoAutoDownload->setMinimumWidth(m_videoAutoDownload->fontMetrics().horizontalAdvance(m_videoAutoDownload->specialValueText()) + 48);
    m_previewWidth  = spin(Settings::previewMaxWidthRange, pixels(), receive);
    m_previewHeight = spin(Settings::previewMaxHeightRange, pixels(), receive);

    auto* receiveForm = form(receive);
    receiveForm->addRow(m_inlinePreviews);
    receiveForm->addRow(m_autoDownload);
    receiveForm->addRow(i18n::t("Load images automatically up to", "بارگذاری خودکار تصاویر تا حجم"), m_autoDownloadMax);
    receiveForm->addRow(m_autoplayGifs);
    receiveForm->addRow(i18n::t("Download videos automatically up to", "دانلود خودکار ویدیوها تا حجم"), m_videoAutoDownload);
    receiveForm->addRow(i18n::t("Preview max width", "حداکثر عرض پیش‌نمایش"), m_previewWidth);
    receiveForm->addRow(i18n::t("Preview max height", "حداکثر ارتفاع پیش‌نمایش"), m_previewHeight);

    // ---- Playback ---------------------------------------------------------------------------------
    auto* playback = new QGroupBox(i18n::t("Playback", "پخش"), this);
    m_volume       = new QSlider(Qt::Horizontal, playback);
    m_volume->setRange(Settings::videoVolumeRange.min, Settings::videoVolumeRange.max);
    m_volume->setSingleStep(5);
    m_volume->setPageStep(10);
    m_volumeLabel = new QLabel(playback);
    m_volumeLabel->setMinimumWidth(m_volumeLabel->fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 6);
    m_startMuted = new QCheckBox(i18n::t("Start videos muted", "شروع ویدیوها بدون صدا"), playback);
    m_loop       = new QCheckBox(i18n::t("Loop videos", "پخش تکراری ویدیوها"), playback);

    auto* volumeRow = new QHBoxLayout;
    volumeRow->addWidget(m_volume, 1);
    volumeRow->addWidget(m_volumeLabel);
    auto* playbackForm = form(playback);
    playbackForm->addRow(i18n::t("Volume", "بلندی صدا"), volumeRow);
    playbackForm->addRow(m_startMuted);
    playbackForm->addRow(m_loop);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) { m_volumeLabel->setText(QStringLiteral("%1%").arg(value)); });

    // ---- Sending ----------------------------------------------------------------------------------
    auto* send = new QGroupBox(i18n::t("Sending", "ارسال"), this);
    m_dragDrop = new QCheckBox(i18n::t("Drop files on the chat to send them (hold Shift to skip)", "ارسال با رها کردن فایل روی چت (برای رد شدن Shift را نگه دارید)"), send);
    m_paste    = new QCheckBox(i18n::t("Ctrl+V a screenshot or copied files in the chat line to send them", "ارسال اسکرین‌شات یا فایل‌های کپی‌شده با Ctrl+V در خط چت"), send);
    m_jpeg     = new QCheckBox(i18n::t("Convert large pasted screenshots to JPEG", "تبدیل اسکرین‌شات‌های بزرگ به JPEG"), send);
    m_previews = new QCheckBox(i18n::t("Upload a small preview with large images and videos", "ارسال پیش‌نمایش کوچک همراه تصاویر و ویدیوهای بزرگ"), send);
    m_previews->setToolTip(i18n::t("Others see a sharp preview right away while the full file loads.", "دیگران تا بارگذاری کامل فایل، فوراً یک پیش‌نمایش واضح می‌بینند."));
    m_notice = new QCheckBox(i18n::t("Tell people without the plugin that it is needed", "اطلاع به افراد بدون پلاگین که برای دیدن، پلاگین لازم است"), send);
    m_downloadUrl = new QLineEdit(send);
    m_downloadUrl->setPlaceholderText(QString::fromLatin1(Settings::defaultDownloadUrl));
    m_downloadUrl->setMaxLength(Settings::maxDownloadUrlLength);
    m_downloadUrl->setLayoutDirection(Qt::LeftToRight);
    m_uploadMax = spin(Settings::uploadMaxMBRange, megabytes(), send);
    m_uploadDir = new QLineEdit(send);
    m_uploadDir->setPlaceholderText(QStringLiteral("/tsmedia"));
    m_uploadDir->setLayoutDirection(Qt::LeftToRight);

    auto* sendForm = form(send);
    sendForm->addRow(m_dragDrop);
    sendForm->addRow(m_paste);
    sendForm->addRow(m_jpeg);
    sendForm->addRow(m_previews);
    sendForm->addRow(m_notice);
    sendForm->addRow(hint(i18n::t("A short note under your file tells them to install TS Media chat; the name links to the page below "
                                  "(leave it empty for the official page).",
                                  "یک یادداشت کوتاه زیر فایل شما می‌گوید که TS Media chat را نصب کنند؛ اسم پلاگین به صفحهٔ زیر لینک می‌شود "
                                  "(برای صفحهٔ رسمی خالی بگذارید)."),
                          send));
    sendForm->addRow(i18n::t("Download link", "لینک دانلود"), m_downloadUrl);
    sendForm->addRow(i18n::t("Max upload size", "حداکثر حجم ارسال"), m_uploadMax);
    sendForm->addRow(i18n::t("Folder in channel files", "پوشه در فایل‌های کانال"), m_uploadDir);

    // ---- General ----------------------------------------------------------------------------------
    auto* general = new QGroupBox(i18n::t("General", "عمومی"), this);
    m_language    = new QComboBox(general);
    m_language->addItem(i18n::t("Automatic (Windows language)", "خودکار (زبان ویندوز)"), static_cast<int>(Language::Auto));
    m_language->addItem(QStringLiteral("English"), static_cast<int>(Language::English));
    m_language->addItem(QStringLiteral("فارسی"), static_cast<int>(Language::Persian));
    m_cacheLimit = spin(Settings::cacheLimitMBRange, megabytes(), general);
    m_cacheLimit->setSingleStep(256);
    m_cacheLimit->setToolTip(i18n::t("The oldest downloaded media is deleted when the cache grows beyond this.", "وقتی حجم کش از این مقدار بیشتر شود، قدیمی‌ترین رسانه‌های دانلودشده پاک می‌شوند."));
    m_cacheLabel     = new QLabel(general);
    auto* openCache  = new QPushButton(i18n::t("Open folder", "باز کردن پوشه"), general);
    auto* clearCache = new QPushButton(i18n::t("Clear", "پاک کردن"), general);
    openCache->setAutoDefault(false);
    clearCache->setAutoDefault(false);

    auto* cacheRow = new QHBoxLayout;
    cacheRow->addWidget(m_cacheLabel, 1);
    cacheRow->addWidget(openCache);
    cacheRow->addWidget(clearCache);

    auto* generalForm = form(general);
    generalForm->addRow(i18n::t("Language", "زبان"), m_language);
    generalForm->addRow(hint(i18n::t("Some texts (menus, hotkeys) change after restarting TeamSpeak.", "برخی متن‌ها (منوها و کلیدهای میان‌بر) پس از اجرای دوبارهٔ TeamSpeak تغییر می‌کنند."), general));
    generalForm->addRow(i18n::t("Cache size limit", "حداکثر حجم کش"), m_cacheLimit);
    generalForm->addRow(cacheRow);

    connect(openCache, &QPushButton::clicked, this, [this] {
        if (m_core)
            m_core->openCacheFolder();
    });
    connect(clearCache, &QPushButton::clicked, this, [this] {
        if (!m_core)
            return;
        if (messageBox(this, QMessageBox::Question, i18n::t("Delete all downloaded media from the cache?", "همهٔ رسانه‌های دانلودشده از کش پاک شوند؟"),
                       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
            != QMessageBox::Yes)
            return;
        m_core->clearCache();
        m_cacheUsed = m_core->cacheSize();
        updateCacheLabel();
    });
    connect(m_cacheLimit, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { updateCacheLabel(); });

    // ---- header, buttons, layout ------------------------------------------------------------------
    auto* about = new QLabel(QStringLiteral("<b>" TSMEDIA_NAME "</b>&nbsp; " TSMEDIA_VERSION "<br><span style='color:gray'>%1</span>")
                                 .arg(i18n::t("Files are stored in the channel's file browser, so everyone can download them. "
                                              "People with this plugin see images, GIFs and videos right in the chat.",
                                              "فایل‌ها در بخش فایل‌های کانال ذخیره می‌شوند تا همه بتوانند دانلودشان کنند. "
                                              "کسانی که این پلاگین را دارند تصاویر، گیف‌ها و ویدیوها را مستقیم داخل چت می‌بینند.")
                                          .toHtmlEscaped()),
                             this);
    about->setWordWrap(true);
    about->setAlignment((i18n::direction() == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignAbsolute | Qt::AlignTop);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n::t("OK", "تأیید"));
    buttons->button(QDialogButtonBox::Cancel)->setText(i18n::t("Cancel", "انصراف"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(i18n::t("Restore defaults", "بازگردانی پیش‌فرض‌ها"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] { load(Settings()); });

    auto* left = new QVBoxLayout;
    left->addWidget(receive);
    left->addWidget(playback);
    left->addStretch(1);
    auto* right = new QVBoxLayout;
    right->addWidget(send);
    right->addWidget(general);
    right->addStretch(1);
    auto* columns = new QHBoxLayout;
    columns->setSpacing(12);
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->addWidget(about);
    layout->addLayout(columns, 1);
    layout->addWidget(buttons);

    connect(m_inlinePreviews, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_autoDownload, &QCheckBox::toggled, this, [this] { updateEnabled(); });
    connect(m_notice, &QCheckBox::toggled, this, [this] { updateEnabled(); });

    // Walks the whole cache folder: once, not on every change of the limit.
    m_cacheUsed = m_core ? m_core->cacheSize() : 0;
    load(Settings::instance());
}

void SettingsDialog::load(const Settings& s)
{
    m_inlinePreviews->setChecked(s.inlinePreviews);
    m_autoDownload->setChecked(s.autoDownloadImages);
    m_autoDownloadMax->setValue(s.autoDownloadMaxMB);
    m_autoplayGifs->setChecked(s.autoplayGifs);
    m_videoAutoDownload->setValue(s.videoAutoDownloadMB);
    m_previewWidth->setValue(s.previewMaxWidth);
    m_previewHeight->setValue(s.previewMaxHeight);

    m_volume->setValue(s.videoVolume);
    m_volumeLabel->setText(QStringLiteral("%1%").arg(m_volume->value()));
    m_startMuted->setChecked(s.videosStartMuted);
    m_loop->setChecked(s.loopVideos);

    m_dragDrop->setChecked(s.interceptDragDrop);
    m_paste->setChecked(s.interceptPaste);
    m_jpeg->setChecked(s.convertLargePngToJpeg);
    m_previews->setChecked(s.generatePreviews);
    m_notice->setChecked(s.addRequiredNotice);
    m_downloadUrl->setText(s.pluginDownloadUrl);
    m_uploadMax->setValue(s.uploadMaxMB);
    m_uploadDir->setText(s.uploadDirectory);

    m_language->setCurrentIndex(qMax(0, m_language->findData(static_cast<int>(s.language))));
    m_cacheLimit->setValue(s.cacheLimitMB);

    updateEnabled();
    updateCacheLabel();
}

void SettingsDialog::updateEnabled()
{
    const bool inlineOn = m_inlinePreviews->isChecked();
    m_autoDownload->setEnabled(inlineOn);
    m_autoDownloadMax->setEnabled(inlineOn && m_autoDownload->isChecked());
    m_autoplayGifs->setEnabled(inlineOn);
    m_videoAutoDownload->setEnabled(inlineOn);
    m_previewWidth->setEnabled(inlineOn);
    m_previewHeight->setEnabled(inlineOn);
    m_downloadUrl->setEnabled(m_notice->isChecked());
}

bool SettingsDialog::apply()
{
    bool          urlOk = true;
    const QString url   = normalizedDownloadUrl(m_downloadUrl->text(), &urlOk);
    if (!urlOk && m_notice->isChecked()) {
        messageBox(this, QMessageBox::Warning,
                   i18n::t("The download link must be a web address starting with http:// or https://.",
                           "لینک دانلود باید یک آدرس وب باشد که با http:// یا https:// شروع می‌شود."),
                   QMessageBox::Ok, QMessageBox::Ok);
        m_downloadUrl->setFocus();
        m_downloadUrl->selectAll();
        return false;
    }

    Settings& s           = Settings::instance();
    s.inlinePreviews      = m_inlinePreviews->isChecked();
    s.autoDownloadImages  = m_autoDownload->isChecked();
    s.autoDownloadMaxMB   = m_autoDownloadMax->value();
    s.autoplayGifs        = m_autoplayGifs->isChecked();
    s.videoAutoDownloadMB = m_videoAutoDownload->value();
    s.previewMaxWidth     = m_previewWidth->value();
    s.previewMaxHeight    = m_previewHeight->value();

    s.videoVolume      = m_volume->value();
    s.videosStartMuted = m_startMuted->isChecked();
    s.loopVideos       = m_loop->isChecked();

    s.interceptDragDrop     = m_dragDrop->isChecked();
    s.interceptPaste        = m_paste->isChecked();
    s.convertLargePngToJpeg = m_jpeg->isChecked();
    s.generatePreviews      = m_previews->isChecked();
    s.addRequiredNotice     = m_notice->isChecked();
    if (urlOk) // with the notice off, an invalid link is simply not saved; empty means the default link
        s.pluginDownloadUrl = url.isEmpty() ? QString::fromLatin1(Settings::defaultDownloadUrl) : url;
    s.uploadMaxMB = m_uploadMax->value();

    QString dir = m_uploadDir->text().trimmed();
    dir.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (dir.isEmpty())
        dir = QString::fromLatin1("/"); // ends up in Settings (see settings.cpp: no literal data in Qt's caches)
    if (!dir.startsWith(QLatin1Char('/')))
        dir.prepend(QLatin1Char('/'));
    while (dir.length() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    s.uploadDirectory = dir;

    s.language     = static_cast<Language>(m_language->currentData().toInt());
    s.cacheLimitMB = m_cacheLimit->value();
    i18n::setLanguage(s.language);

    s.save();
    emit settingsChanged();
    return true;
}

void SettingsDialog::updateCacheLabel()
{
    const quint64 limit = static_cast<quint64>(m_cacheLimit->value()) * 1024 * 1024;
    m_cacheLabel->setText(i18n::t("Using %1 of %2", "فضای استفاده‌شده: %1 از %2").arg(ltr(formatSize(m_cacheUsed)), ltr(formatSize(limit))));
}
