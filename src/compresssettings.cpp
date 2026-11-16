#include "compresssettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "core.h"
#include "settingswidgets.h" // 2.7 F3: switches
#include "i18n.h"
#include "video/mfvideo.h"

CompressSettingsSection::CompressSettingsSection(QWidget* dialog, Core* core)
    : SettingsSection(dialog)
    , m_core(core)
    , m_available(mf::available())
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QGroupBox* group = card(i18n::t("Videos"), this); // 2.7 F3: rows; the tooltips that explained an option are its helper text now

    m_threshold = new QSpinBox(group);
    m_threshold->setRange(Settings::compressVideosOverMBRange.min, Settings::compressVideosOverMBRange.max);
    m_threshold->setSingleStep(5);
    m_threshold->setSuffix(i18n::t(" MB"));
    m_threshold->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_threshold->setAccelerated(true);
    m_threshold->setAccessibleName(i18n::t("Compress videos larger than"));
    m_compress = switchRow(group, i18n::t("&Compress videos larger than"),
                           m_available ? i18n::t("Done on this computer, to an MP4 that plays everywhere. A video over your upload limit is made small enough "
                                                 "when possible; if that fails, the original is sent when it fits.")
                                       : i18n::t("Video compression needs Windows Media Foundation. On Windows N editions, install the Media Feature Pack."),
                           0, m_threshold);
    m_hint = hintOf(m_compress);

    // The formats by name, so the option explains itself.
    m_convert = switchRow(group, i18n::t("Con&vert iPhone (HEVC), AV1, VP9 and camcorder videos too"),
                          i18n::t("Many people can't play these formats. They become an MP4 of any size, but only when this computer can play them."), 1);
    m_convert->setToolTip(m_convert->accessibleDescription());

    m_quality = new QComboBox(group);
    m_quality->addItem(i18n::t("Smaller (480p)"), 480);
    m_quality->addItem(i18n::t("Balanced (720p)"), 720);
    m_quality->addItem(i18n::t("High (1080p)"), 1080);
    m_quality->setAccessibleName(i18n::t("Quality"));
    m_qualityLabel = fieldRow(group, i18n::t("&Quality"), m_quality, i18n::t("Smaller files download faster. 720p and 1080p keep up to 60 frames per second."), 1);
    m_quality->setToolTip(m_quality->accessibleDescription());

    m_gpu = switchRow(group, i18n::t("Use the &graphics card when possible"),
                      i18n::t("Faster and lighter on the processor. Turn this off if compressed videos look wrong or compressing fails."), 1);
    m_gpu->setToolTip(m_gpu->accessibleDescription());

    const auto changed = [this] {
        updateEnabled();
        if (!m_loading)
            emit this->changed();
    };
    connect(m_compress, &QCheckBox::toggled, this, changed);
    connect(m_convert, &QCheckBox::toggled, this, changed);
    connect(m_gpu, &QCheckBox::toggled, this, changed);
    connect(m_threshold, QOverload<int>::of(&QSpinBox::valueChanged), this, changed);
    connect(m_quality, QOverload<int>::of(&QComboBox::currentIndexChanged), this, changed);

    if (m_core) {
        connect(m_core, &Core::encodersKnown, this, [this] { updateEncoders(); });
        m_core->queryEncoders(); // once per session, on a worker
        updateEncoders();
    }
    updateEnabled();
}

void CompressSettingsSection::load(const Settings& s)
{
    m_loading = true;
    m_compress->setChecked(s.compressVideos);
    m_threshold->setValue(s.compressVideosOverMB);
    m_convert->setChecked(s.convertUnplayableVideos);
    const int index = m_quality->findData(Settings::normalizeVideoQuality(s.compressVideoQuality));
    m_quality->setCurrentIndex(index >= 0 ? index : 1);
    m_gpu->setChecked(s.compressUseGpu);
    m_loading = false;
    updateEnabled();
}

void CompressSettingsSection::store(Settings& target, const Settings& loaded) const
{
    take(target.compressVideos, loaded.compressVideos, m_compress->isChecked());
    take(target.compressVideosOverMB, loaded.compressVideosOverMB, m_threshold->value());
    take(target.convertUnplayableVideos, loaded.convertUnplayableVideos, m_convert->isChecked());
    take(target.compressVideoQuality, loaded.compressVideoQuality, m_quality->currentData().toInt());
    take(target.compressUseGpu, loaded.compressUseGpu, m_gpu->isChecked());
}

void CompressSettingsSection::updateEnabled()
{
    const bool any = m_available && (m_compress->isChecked() || m_convert->isChecked());
    setRowEnabled(m_compress, m_available);
    setRowEnabled(m_convert, m_available);
    m_threshold->setEnabled(m_available && m_compress->isChecked());
    setRowEnabled(m_quality, any); // with its label and helper
    setRowEnabled(m_gpu, any && !m_noGpuEncoder);
}

void CompressSettingsSection::updateEncoders()
{
    if (!m_core || !m_core->encodersQueried())
        return;
    m_noGpuEncoder = m_core->hardwareEncoders().isEmpty();
    if (m_noGpuEncoder) {
        const QString text = i18n::t("No graphics card video encoder was found. The processor is used.");
        m_gpu->setToolTip(text);
        m_gpu->setAccessibleDescription(text);
        if (QLabel* helper = hintOf(m_gpu)) // 2.7 F3: the helper text says it too
            helper->setText(text);
    }
    updateEnabled();
}
