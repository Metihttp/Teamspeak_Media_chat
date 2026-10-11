#include "composedialog.h"
#include "emojiinput.h" // 2.2 emoji

#include <QAbstractButton>
#include <QAccessible>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageIOHandler>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QToolButton>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyle>
#include <QTextOption>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "composehooks.h"
#include "composewidgets.h" // 2.3 compose-v2: the window's own controls and colours
#include "filedrag.h" // 2.2 drag-out: the own-drag marker
#include "i18n.h"
#include "spoiler.h" // 2.2 spoiler: the receivers' cover on spoiler thumbnails
#include "uiutil.h"
#include "video/mfvideo.h"

#include "video/mftranscode.h" // 2.4 compress: mf::measuredFrameRate
// 2.2 editor
#include <QRandomGenerator>
#include <windows.h>

#include "imageeditor.h"

using compose::Item;
using compose::Problem;
using composeui::Glyph;
using composeui::Theme;

namespace {

// 2.3 compose-v2: a 4 / 8 px grid.
constexpr int kMargin        = 20;
constexpr int kSection       = 16;  // between the window's parts
constexpr int kWidth         = 560; // logical px; kNarrowWidth on screens under 760 px
constexpr int kNarrowWidth   = 460;
constexpr int kGridGap       = 12;
constexpr int kTileMax       = 176; // the largest grid tile (what thumbnails are decoded to cover)
constexpr int kPreviewWidth  = kWidth - 2 * kMargin; // the large preview of a single picture or video
constexpr int kPreviewHeight = 320;
constexpr int kMinStage      = 168;
constexpr int kFileCardH     = 76;
constexpr int kMinGridView   = 120; // the least of the grid shown on a small screen
constexpr int kTargetCheckMs = 1000; // connected? partner still there?

// Virtual keys for shortcuts on non-Latin keyboard layouts (as in the viewer).
constexpr quint32 kVkO = 0x4F;
constexpr quint32 kVkS = 0x53;
constexpr quint32 kVkV = 0x56;
constexpr quint32 kVkE = 0x45; // 2.2 editor

using composeui::withAlpha;

QFont scaledFont(const QFont& base, qreal factor)
{
    QFont font(base);
    if (base.pixelSize() > 0)
        font.setPixelSize(qMax(1, qRound(base.pixelSize() * factor)));
    else
        font.setPointSizeF(qMax(1.0, (base.pointSizeF() > 0 ? base.pointSizeF() : 9.0) * factor));
    return font;
}

QFont demiBold(const QFont& base)
{
    QFont font(base);
    font.setBold(false);
    font.setWeight(QFont::DemiBold);
    return font;
}

QPixmap alertPixmap(int size, qreal dpr, const QColor& fill, const QColor& mark)
{
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    composeui::drawAlert(p, QRectF(0, 0, size, size), fill, mark);
    return pixmap;
}

// Plain text on one line, shortened with "…" to width, at the left also for a Persian or Arabic name
// (its own direction still applies inside the line).
void drawLine(QPainter& p, const QRectF& rect, const QString& text, Qt::TextElideMode mode)
{
    const QString shown = QFontMetricsF(p.font()).elidedText(text, mode, rect.width());
    QTextOption   option(Qt::AlignLeft | Qt::AlignAbsolute | Qt::AlignVCenter);
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(text.isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight);
    p.drawText(rect, shown, option);
}

// One line of plain text, shortened with "…" to the width it gets (files from the user's disk: the
// full name is in the tooltip and the accessible name).
class ElidedLabel : public QLabel
{
  public:
    ElidedLabel(Qt::TextElideMode mode, QWidget* parent)
        : QLabel(parent)
        , m_mode(mode)
    {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        // At the left like the rest of the window, also for a Persian or Arabic name (its own direction
        // still applies inside the line).
        setAlignment(Qt::AlignLeft | Qt::AlignAbsolute | Qt::AlignVCenter);
    }

    void setFullText(const QString& text)
    {
        if (text == m_full)
            return;
        m_full = text;
        refresh();
    }

    const QString& fullText() const { return m_full; }

  protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        refresh();
    }

  private:
    void refresh()
    {
        QLabel::setText(fontMetrics().elidedText(m_full, m_mode, qMax(0, width())));
        setAccessibleName(m_full);
    }

    Qt::TextElideMode m_mode;
    QString           m_full;
};

// A label for secondary text ("hint") or problems ("error"): applyTheme() colours them.
QLabel* roleLabel(const QString& text, const char* role, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setProperty("role", QString::fromLatin1(role));
    return label;
}

// A label takes its role's colour: now (palette), and in skins that colour labels by style sheet (the
// dialog's role rules, applied again after a change).
void setRole(QLabel* label, const char* role, const Theme& theme)
{
    const QString value = QString::fromLatin1(role);
    if (label->property("role").toString() != value) {
        label->setProperty("role", value);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    const QColor color = value == QLatin1String("error") ? theme.error : theme.muted;
    QPalette     pal   = label->palette();
    pal.setColor(QPalette::Active, QPalette::WindowText, color);
    pal.setColor(QPalette::Inactive, QPalette::WindowText, color);
    label->setPalette(pal);
}

// The size a worker decodes a picture at: fits the large preview, but covers a grid tile; never larger
// than the picture itself.
QSize decodeSize(const QSize& pixels, const QSize& maxBox, int minSide)
{
    if (!pixels.isValid() || pixels.isEmpty())
        return {};
    QSize       size  = pixels.scaled(maxBox, Qt::KeepAspectRatio);
    const QSize cover = pixels.scaled(QSize(minSide, minSide), Qt::KeepAspectRatioByExpanding);
    if (cover.width() > size.width() || cover.height() > size.height())
        size = cover;
    if (size.width() > pixels.width() || size.height() > pixels.height())
        size = pixels;
    return size.expandedTo(QSize(1, 1));
}

// What a worker found out about a file (runs off the GUI thread: no widgets, no TeamSpeak).
struct ProbeData {
    bool   readable = true;
    QSize  pixels;
    qint64 durationMs = 0;
    QImage thumb;
    bool                      hasFacts = false; // 2.4 compress: the planner's facts of a video
    videocompress::VideoFacts facts;
};

ProbeData probeFile(const QString& path, MediaKind kind, const QSize& maxBox, int minSide, const std::atomic_bool& closing, bool videoFacts)
{
    ProbeData data;
    {
        QFile file(path);
        data.readable = file.open(QIODevice::ReadOnly); // fails while another program holds it exclusively
    }
    if (!data.readable || closing.load())
        return data;
    if (isPreviewableImage(kind)) {
        QImageReader reader(path);
        reader.setDecideFormatFromContent(true);
        reader.setAutoTransform(true);
        const QSize raw     = reader.size(); // the header only
        const bool  rotated = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
        if (!raw.isValid())
            return data;
        data.pixels         = rotated ? raw.transposed() : raw;
        const qint64 pixels = static_cast<qint64>(raw.width()) * raw.height();
        const bool   jpeg   = reader.format() == "jpeg";
        if (QFileInfo(path).size() > compose::kMaxThumbnailFileBytes || pixels > (jpeg ? compose::kMaxJpegThumbnailPixels : compose::kMaxThumbnailPixels))
            return data; // the type art stays
        const QSize target = decodeSize(data.pixels, maxBox, minSide);
        reader.setScaledSize(rotated ? target.transposed() : target); // applied before the rotation
        data.thumb = reader.read();
    } else if (kind == MediaKind::Video || kind == MediaKind::Audio || videoFacts) {
        const mf::ProbeResult probe = mf::probe(path, qMax(maxBox.width(), maxBox.height()));
        if (videoFacts) {
            // 2.4 compress: what Core's planner will see (the same rules, so the chip matches the send).
            data.hasFacts                  = true;
            videocompress::VideoFacts& f   = data.facts;
            f.bytes                        = static_cast<quint64>(qMax<qint64>(0, QFileInfo(path).size()));
            f.durationMs                   = qMax<qint64>(0, probe.durationMs);
            f.display                      = probe.size;
            const double measured          = probe.ok && probe.hasVideo && !closing.load() ? mf::measuredFrameRate(path) : 0.0;
            f.fps                          = measured > 0.0 ? measured : probe.frameRate;
            f.probed                       = probe.ok;
            f.hasVideo                     = probe.hasVideo;
            f.decodable                    = !probe.poster.isNull();
            f.undecodableText              = probe.error;
            f.hasAudio                     = probe.hasAudio;
            f.audioChannels                = probe.audioChannels;
            f.videoCodec                   = videocompress::codecId(probe.videoCodec);
            f.extension                    = QFileInfo(path).suffix().toLower();
        }
        if (!probe.ok)
            return data;
        data.durationMs = qMax<qint64>(0, probe.durationMs);
        if (probe.hasVideo && probe.size.isValid())
            data.pixels = probe.size;
        if (!probe.poster.isNull()) {
            const QSize target = decodeSize(probe.poster.size(), maxBox, minSide);
            data.thumb         = target == probe.poster.size() ? probe.poster : probe.poster.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }
    return data;
}

// 2.4 compress: the Quality entry that is in effect (picked, or the planner's default).
int effectiveQuality(const Item& item)
{
    if (item.quality >= 0 && item.quality < item.choices.size() && item.choices.at(item.quality).enabled)
        return item.quality;
    for (int i = 0; i < item.choices.size(); ++i) {
        if (item.choices.at(i).selected)
            return i;
    }
    return -1;
}

// 2.3 compose-v2: the Quality chip's text: "720p · about 18 MB" (longLabels: "Balanced (720p) · about
// 18 MB"); "Checking video…" until the probe answered.
QString qualityText(const Item& item, bool longLabels)
{
    if (!item.factsKnown)
        return i18n::t("Checking video…");
    const int current = effectiveQuality(item);
    if (current < 0)
        return i18n::t("Quality");
    const videocompress::Choice& choice = item.choices.at(current);
    return longLabels ? choice.longLabel : choice.label;
}

SendQuality sendQualityFor(videocompress::Request request)
{
    switch (request) {
    case videocompress::Request::Original:
        return SendQuality::Original;
    case videocompress::Request::P1080:
        return SendQuality::P1080;
    case videocompress::Request::P720:
        return SendQuality::P720;
    case videocompress::Request::P480:
        return SendQuality::P480;
    default:
        return SendQuality::Auto;
    }
}

QString samePathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower(); // Windows paths ignore case
}

bool isPicture(const Item& item)
{
    return item.isPasted() || isPreviewableImage(item.kind);
}

bool isMedia(const Item& item)
{
    return isPicture(item) || item.kind == MediaKind::Video;
}

QString dot()
{
    return QStringLiteral(" · ");
}

// 2.3 compose-v2: a tile's second line, short (the type is on the badge): "580 KB · 4032 × 3024",
// "0:42 · 18 MB", "ZIP archive · 24 KB", "Edited · …".
QString tileMeta(const Item& item)
{
    QStringList parts;
    if (item.edited)
        parts << i18n::t("Edited");
    const QString size = formatSize(static_cast<quint64>(qMax<qint64>(0, item.size)));
    if (item.isPasted()) {
        if (item.pixels.isValid() && !item.pixels.isEmpty())
            parts << i18n::t("%1 × %2").arg(item.pixels.width()).arg(item.pixels.height());
        else
            parts << compose::typeText(item);
    } else if (isPicture(item)) {
        parts << size;
        if (item.pixels.isValid() && !item.pixels.isEmpty())
            parts << i18n::t("%1 × %2").arg(item.pixels.width()).arg(item.pixels.height());
    } else if (item.kind == MediaKind::Video) {
        if (item.durationMs > 0)
            parts << formatDuration(item.durationMs);
        parts << size;
    } else {
        parts << compose::typeText(item) << size;
    }
    return parts.join(dot());
}

// The problem a tile or the large preview shows instead of its details; empty when it can be sent.
QString problemFor(const Item& item, int limitMB, bool full)
{
    if (item.canSend())
        return {};
    QString problem = compose::problemText(item.problem, limitMB);
    if (item.problem == Problem::TooLarge && !item.compressProblem.isEmpty())
        return item.compressProblem; // 2.4 compress: why compressing can't help
    if (!full)
        return item.problem == Problem::TooLarge ? problem + dot() + formatSize(static_cast<quint64>(qMax<qint64>(0, item.size))) : problem;
    if (item.problem == Problem::TooLarge)
        problem += QStringLiteral(". ") + i18n::t("You can raise the limit in Settings → Sending.");
    else if (item.problem == Problem::Empty)
        problem += QLatin1Char('.');
    return problem;
}

// 2.3 compose-v2: under the header: what goes out, "4 images · 12.3 MB" (no size while a video is made
// smaller first: it isn't known yet).
QString summaryText(const QVector<Item>& items)
{
    int    count     = 0;
    int    pictures  = 0;
    int    videos    = 0;
    qint64 bytes     = 0;
    bool   estimated = false;
    for (const Item& item : items) {
        if (!item.canSend())
            continue;
        ++count;
        pictures += isPicture(item) ? 1 : 0;
        videos += item.kind == MediaKind::Video ? 1 : 0;
        bytes += item.isPasted() ? 0 : qMax<qint64>(0, item.size);
        const int quality = effectiveQuality(item);
        if (item.compressible && quality >= 0 && item.choices.at(quality).request != videocompress::Request::Original)
            estimated = true;
    }
    if (count == 0)
        return items.isEmpty() ? i18n::t("Nothing added yet") : i18n::t("Nothing here can be sent");
    QString what;
    if (pictures == count)
        what = count == 1 ? i18n::t("1 image") : i18n::t("%1 images").arg(count);
    else if (videos == count)
        what = count == 1 ? i18n::t("1 video") : i18n::t("%1 videos").arg(count);
    else
        what = count == 1 ? i18n::t("1 file") : i18n::t("%1 files").arg(count);
    return bytes > 0 && !estimated ? what + dot() + formatSize(static_cast<quint64>(bytes)) : what;
}

// The type badge on a picture: "JPG", "GIF", "0:42" (videos); empty for a paste.
QString badgeText(const Item& item)
{
    if (item.isPasted())
        return {};
    if (item.kind == MediaKind::Video)
        return item.durationMs > 0 ? formatDuration(item.durationMs) : QFileInfo(item.fileName).suffix().toUpper();
    if (isPreviewableImage(item.kind))
        return QFileInfo(item.fileName).suffix().toUpper().left(5);
    return {};
}

// key: how to toggle it from the keyboard ("S" on a focused tile, "Alt+S" for the single item).
QString spoilerTip(bool checked, bool single)
{
    const QString why = i18n::t("People on TS Media 2.1 or older see it unblurred.");
    const QString key = single ? i18n::t("Alt+S") : i18n::t("S");
    return (checked ? i18n::t("Marked as spoiler: shown blurred until someone clicks it (%1).") : i18n::t("Mark as spoiler: shown blurred until someone clicks it (%1)."))
               .arg(key)
           + QLatin1Char(' ') + why;
}

// 2.3 compose-v2 review: where a picture of content's size covers box (cropped, centred).
QRectF coverRect(const QSizeF& content, const QRectF& box)
{
    if (content.isEmpty())
        return box;
    QRectF rect(QPointF(0, 0), content.scaled(box.size(), Qt::KeepAspectRatioByExpanding));
    rect.moveCenter(box.center());
    return rect;
}

// Holds the icon buttons over a picture: a dark pill that fades in with the tile's hover or focus.
class PillBar : public QWidget
{
  public:
    PillBar(QWidget* parent, const composeui::Fader* reveal)
        : QWidget(parent)
        , m_reveal(reveal)
    {
        setProperty("tsmFlat", true);
        row = new QHBoxLayout(this);
        row->setContentsMargins(2, 2, 2, 2);
        row->setSpacing(0);
    }

    QHBoxLayout* row = nullptr;

  protected:
    void paintEvent(QPaintEvent*) override
    {
        const qreal opacity = m_reveal ? m_reveal->value() : 1.0;
        if (opacity <= 0.001)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(opacity);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        const QRectF r = QRectF(rect());
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    }

  private:
    const composeui::Fader* m_reveal;
};

} // namespace

// ============================================================================================
// Parts of the window
// ============================================================================================

// 2.3 compose-v2: one item. Grid: a square thumbnail (cover-cropped), its type badge, the actions on a
// pill at the top right (shown on hover or keyboard focus), the name and its details under it, and the
// video's Quality chip. Stage: the single item's large preview (the picture fitted on a rounded stage,
// spoiler and remove always shown). FileCard: a file without a picture (its type art, name, details,
// remove). Spoilers show the receivers' cover (spoiler::drawCover); items that can't be sent are faded,
// with the alert mark. Keys on a focused tile: Delete removes it, S marks it as a spoiler, E edits it.
class ComposeDialog::Tile : public QWidget
{
  public:
    enum class Mode { Grid, Stage, FileCard };

    Tile(ComposeDialog* dialog, const Item& item, Mode mode, const QSize& size, bool withChip, QWidget* parent)
        : QWidget(parent)
        , m_dialog(dialog)
        , m_id(item.id)
        , m_mode(mode)
    {
        setObjectName(QString::fromLatin1("tsmComposeTile"));
        setProperty("tsmFlat", true);
        setFixedSize(size);
        naturalHeight = size.height();
        setAttribute(Qt::WA_Hover);
        m_reveal = new composeui::Fader(this, 140);
        const bool fileCard = mode == Mode::FileCard;
        const auto look     = fileCard ? composeui::IconButton::Look::Plain : composeui::IconButton::Look::OnMedia;
        const int  side     = fileCard ? 32 : 28;
        if (!fileCard) {
            m_pill = new PillBar(this, m_reveal);
        }
        const auto add = [&](Glyph glyph, const char* name) {
            auto* button = new composeui::IconButton(glyph, look, side, m_pill ? static_cast<QWidget*>(m_pill) : this);
            button->setObjectName(QString::fromLatin1(name));
            button->setReveal(m_pill ? m_reveal : nullptr);
            button->installEventFilter(this);
            if (m_pill)
                m_pill->row->addWidget(button);
            return button;
        };
        // 2.2 editor: Edit (and, once edited, Revert to original) for pictures; the large preview has chips.
        if (mode == Mode::Grid && m_dialog->editingOffered() && isPicture(item)) {
            revert = add(Glyph::Revert, "tsmTileRevert");
            edit   = add(Glyph::Edit, "tsmTileEdit");
            QObject::connect(edit, &QAbstractButton::clicked, m_dialog, [d = m_dialog, id = m_id] { d->openEditor(id); });
            QObject::connect(revert, &QAbstractButton::clicked, m_dialog, [d = m_dialog, id = m_id] { d->askRevert(id); });
        }
        if (compose::spoilerAllowed(item.kind)) { // a video without a picture yet has a file card, and may be a spoiler
            spoiler = add(Glyph::Spoiler, "tsmTileSpoiler");
            spoiler->setCheckable(true);
            // 2.3 compose-v2 review: the single item keeps 2.2's Alt+S ("Mark as &spoiler").
            if (mode != Mode::Grid)
                spoiler->setShortcut(QKeySequence(Qt::ALT | Qt::Key_S));
            QObject::connect(spoiler, &QAbstractButton::clicked, m_dialog, [d = m_dialog, id = m_id] { d->toggleSpoiler(id); });
        }
        remove = add(Glyph::Remove, "tsmTileRemove");
        QObject::connect(remove, &QAbstractButton::clicked, m_dialog, [d = m_dialog, id = m_id] { d->removeItem(id); });
        if (mode == Mode::Grid && withChip && item.compressible) {
            // 2.4 compress: the video's Quality, from the planner.
            quality = new composeui::Chip(Glyph::Quality, QString(), this);
            quality->setObjectName(QString::fromLatin1("tsmComposeQuality"));
            quality->setMenuIndicator(true);
            quality->installEventFilter(this);
            quality->setStepHandler([d = m_dialog, id = m_id](int delta) { d->stepQuality(id, delta); });
            QObject::connect(quality, &QAbstractButton::clicked, m_dialog, [d = m_dialog, id = m_id, chip = quality] { d->openQualityMenu(id, chip); });
        }
        if (mode == Mode::Stage)
            m_reveal->jumpTo(1.0); // the single item's actions are always there
        layoutChildren();
    }

    int    id() const { return m_id; }
    Mode   mode() const { return m_mode; }
    QSize  pictureSize() const { return pictureRect().size().toSize(); }

    // The height of a grid tile of this width: the square, the name, the details (and the Quality chip).
    static int gridHeight(int side, bool withChip, const QFont& font)
    {
        return side + 8 + QFontMetrics(demiBold(font)).height() + 2 + QFontMetrics(font).height() + (withChip ? 6 + 28 : 0);
    }

    void setImage(const QImage& image)
    {
        m_image = image;
        // 2.3 compose-v2 review: the large preview's bands show the picture blurred (made once here,
        // not on every paint of a hover fade).
        m_backdrop = m_mode == Mode::Stage && !image.isNull() ? spoiler::blurredStill(image, false) : QImage();
        update();
    }

    void setItem(const Item& item, int limitMB, const Theme& theme, const QString& editBlocker)
    {
        m_theme      = theme;
        m_name       = compose::displayName(item);
        m_problem    = problemFor(item, limitMB, false);
        m_meta       = m_problem.isEmpty() ? tileMeta(item) : m_problem;
        if (m_mode == Mode::FileCard) {
            // The single item's card: its details; the window says the problem right under it.
            m_meta    = compose::metaText(item);
            m_problem.clear();
        }
        m_badge      = badgeText(item);
        m_kind       = item.kind;
        m_pasted     = item.isPasted();
        m_spoiler    = item.spoiler;
        m_canSend    = item.canSend();
        m_probed     = item.probed;
        m_extension  = item.isPasted() ? QStringLiteral("png") : QFileInfo(item.fileName).suffix();
        const QString fullProblem = problemFor(item, limitMB, true);
        setToolTip(QString::fromLatin1("<p style='white-space:pre'>%1</p>").arg(m_name.toHtmlEscaped())
                   + (fullProblem.isEmpty() ? QString() : QString::fromLatin1("<p>%1</p>").arg(fullProblem.toHtmlEscaped())));
        if (spoiler) {
            spoiler->setEnabled(item.canSend()); // nothing to mark on what isn't sent
            spoiler->setChecked(item.spoiler);
            spoiler->setAccessibleName(i18n::t("Mark %1 as spoiler").arg(m_name));
            spoiler->setAccessibleDescription(i18n::t("People on TS Media 2.1 or older see it unblurred."));
            spoiler->setToolTip(spoilerTip(item.spoiler, m_mode != Mode::Grid));
            spoiler->setTheme(theme);
        }
        remove->setAccessibleName(i18n::t("Remove %1").arg(m_name));
        remove->setToolTip(i18n::t("Remove (Delete)"));
        remove->setTheme(theme);
        if (edit) { // 2.2 editor
            edit->setEnabled(editBlocker.isEmpty());
            edit->setAccessibleName(i18n::t("Edit %1").arg(m_name));
            edit->setAccessibleDescription(editBlocker);
            edit->setToolTip(editBlocker.isEmpty() ? i18n::t("Crop, draw or hide details (E)") : editBlocker);
            edit->setTheme(theme);
            revert->setVisible(item.edited);
            revert->setAccessibleName(i18n::t("Revert %1 to the original").arg(m_name));
            revert->setToolTip(i18n::t("Revert to the original picture"));
            revert->setTheme(theme);
        }
        if (quality) {
            const bool shown = !item.factsKnown || !item.choices.isEmpty();
            quality->setVisible(shown && item.canSend());
            quality->setText(qualityText(item, false));
            quality->setEnabled(item.factsKnown && effectiveQuality(item) >= 0);
            quality->setAccessibleName(i18n::t("Quality of %1").arg(m_name));
            quality->setToolTip(i18n::t("Compressed on this computer to an MP4 that plays everywhere. Smaller downloads faster."));
            quality->setTheme(theme);
        }
        setAccessibleName(compose::accessibleName(item));
        setAccessibleDescription(m_problem);
        layoutChildren();
        update();
    }

    composeui::IconButton* spoiler = nullptr;
    composeui::IconButton* remove  = nullptr;
    composeui::IconButton* edit    = nullptr; // 2.2 editor (grid tiles)
    composeui::IconButton* revert  = nullptr;
    composeui::Chip*       quality = nullptr; // 2.4 compress: videos (grid tiles)
    int                    naturalHeight = 0; // the large preview is lower on a small screen (fitHeight)

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (m_mode == Mode::FileCard)
            paintFileCard(p);
        else
            paintPicture(p);
    }

    void enterEvent(QEvent* event) override
    {
        QWidget::enterEvent(event);
        m_hovered = true;
        updateReveal();
    }

    void leaveEvent(QEvent* event) override
    {
        QWidget::leaveEvent(event);
        m_hovered = false;
        updateReveal();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        layoutChildren();
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::FocusIn) {
            m_keyboardFocus = composeui::FocusTracker::byKeyboard(static_cast<QFocusEvent*>(event), qobject_cast<QWidget*>(watched));
            updateReveal();
        } else if (event->type() == QEvent::FocusOut) {
            updateReveal(); // the focus widget is the next one already
        }
        return QWidget::eventFilter(watched, event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        const bool plain = (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if (plain && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
            m_dialog->removeItem(m_id);
            return;
        }
        if (plain && spoiler && spoiler->isEnabled() && (event->key() == Qt::Key_S || event->nativeVirtualKey() == kVkS)) {
            m_dialog->toggleSpoiler(m_id);
            return;
        }
        if (plain && (event->key() == Qt::Key_E || event->nativeVirtualKey() == kVkE)) { // 2.2 editor
            if (edit && edit->isEnabled()) {
                m_dialog->openEditor(m_id);
                return;
            }
            if (m_mode == Mode::Stage && m_dialog->editBlocker(m_id).isEmpty() && m_dialog->editingOffered() && (m_pasted || isPreviewableImage(m_kind))) {
                m_dialog->openEditor(m_id);
                return;
            }
        }
        QWidget::keyPressEvent(event);
    }

  private:
    QRectF pictureRect() const
    {
        if (m_mode == Mode::Grid)
            return QRectF(0, 0, width(), width());
        return QRectF(rect());
    }

    qreal radius() const { return m_mode == Mode::Grid ? 10.0 : 12.0; }

    void layoutChildren()
    {
        const QRectF pic = pictureRect();
        if (m_pill) {
            m_pill->adjustSize();
            const int inset = m_mode == Mode::Stage ? 10 : 6;
            m_pill->move(qRound(pic.right()) - inset - m_pill->width(), qRound(pic.top()) + inset);
        } else if (remove) {
            remove->move(width() - 14 - remove->width(), (height() - remove->height()) / 2);
            if (spoiler)
                spoiler->move(remove->x() - 4 - spoiler->width(), remove->y());
        }
        if (quality) {
            const int top = qRound(pic.bottom()) + 8 + QFontMetrics(demiBold(font())).height() + 2 + fontMetrics().height() + 6;
            quality->setGeometry(0, top, width(), 28);
        }
    }

    void updateReveal()
    {
        QWidget* focus = QApplication::focusWidget();
        m_focusWithin  = focus && focus != this && isAncestorOf(focus) && focus->hasFocus();
        m_reveal->setTarget(m_mode == Mode::Stage || m_hovered || m_focusWithin ? 1.0 : 0.0);
        update();
    }

    bool showsPicture() const { return m_pasted || isPreviewableImage(m_kind) || m_kind == MediaKind::Video; }

    void paintPicture(QPainter& p)
    {
        const QRectF pic = pictureRect();
        const qreal  r   = radius();
        QPainterPath clip;
        clip.addRoundedRect(pic, r, r);
        p.save();
        p.setClipPath(clip);
        p.fillRect(pic, m_theme.surface);
        if (m_spoiler && showsPicture()) {
            // 2.2 spoiler: what receivers see. Nothing sharp of the picture is drawn; without a picture
            // yet, the placeholder and the pill.
            bool               blurHash = false;
            const QImage       source   = spoiler::coverSource(QString(), pic.size(), m_image, false, &blurHash);
            spoiler::CoverLook look;
            look.font = font();
            look.font.setPixelSize(m_mode == Mode::Grid ? 11 : 13);
            look.font.setBold(true);
            look.placeholder = m_theme.surface;
            spoiler::drawCover(p, pic, source, blurHash, look);
        } else if (!m_image.isNull()) {
            const QSizeF size = QSizeF(m_image.size()) / m_image.devicePixelRatio();
            QRectF       target(QPointF(0, 0), size);
            target.moveCenter(pic.center());
            if (m_mode == Mode::Stage && (target.width() < pic.width() - 1 || target.height() < pic.height() - 1)) {
                // 2.3 compose-v2 review: a picture that fills the stage's width or height has bands at
                // the sides (or above and below): they show it blurred and toned down, so the badge and
                // the buttons in the corners sit on the picture, not on an empty band. A small picture
                // that fills neither keeps the plain stage.
                if (!m_backdrop.isNull() && (target.width() >= pic.width() - 1 || target.height() >= pic.height() - 1)) {
                    p.drawImage(coverRect(QSizeF(m_backdrop.size()), pic), m_backdrop);
                    p.fillRect(pic, m_theme.dark ? QColor(0, 0, 0, 96) : QColor(255, 255, 255, 84));
                }
                // Fitted inside the stage: its own rounded corners.
                QPainterPath inner;
                inner.addRoundedRect(target, 8, 8);
                p.setClipPath(inner, Qt::IntersectClip);
            }
            p.drawImage(target, m_image);
            p.setClipPath(clip);
        } else {
            // No picture (yet, or none): the file's type art.
            const qreal  artH = m_mode == Mode::Grid ? qMin(pic.height() * 0.40, 72.0) : 96.0;
            const QRectF art(pic.center().x() - artH / 2, pic.center().y() - artH / 2, artH, artH);
            composeui::drawFileArt(p, art, m_extension, composeui::fileTint(m_extension, m_theme.dark), m_theme);
        }
        if (m_kind == MediaKind::Video && !m_spoiler && !m_image.isNull()) {
            const qreal  d = m_mode == Mode::Grid ? 40.0 : 56.0;
            const QRectF disc(pic.center().x() - d / 2, pic.center().y() - d / 2, d, d);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 140));
            p.drawEllipse(disc);
            composeui::drawGlyph(p, Glyph::Play, disc.adjusted(d * 0.30, d * 0.28, -d * 0.26, -d * 0.28), Qt::white);
        }
        if (!m_canSend)
            p.fillRect(pic, withAlpha(m_theme.body, 0.55)); // about 45% strength
        p.restore();
        // A hairline, so a picture with the window's colour still has an edge.
        p.setPen(QPen(withAlpha(m_theme.text, m_theme.dark ? 0.07 : 0.10), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(pic.adjusted(0.5, 0.5, -0.5, -0.5), r, r);

        // The type badge (bottom left; the type art says it without a picture) and the alert mark (top left).
        if (!m_badge.isEmpty() && !m_spoiler && !m_image.isNull()) {
            QFont badge = font();
            badge.setPixelSize(m_mode == Mode::Grid ? 10 : 11);
            badge.setBold(true);
            p.setFont(badge);
            const QFontMetricsF fm(badge);
            const bool          video = m_kind == MediaKind::Video;
            const qreal         h     = m_mode == Mode::Grid ? 18 : 20;
            const qreal         w     = fm.horizontalAdvance(m_badge) + 12 + (video ? 10 : 0);
            const QRectF        box(pic.left() + 8, pic.bottom() - 8 - h, w, h);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 160));
            p.drawRoundedRect(box, h / 2, h / 2);
            QRectF text = box.adjusted(6, 0, -6, 0);
            if (video) {
                composeui::drawGlyph(p, Glyph::Play, QRectF(text.left() - 1, box.center().y() - 4.5, 9, 9), Qt::white);
                text.setLeft(text.left() + 10);
            }
            p.setPen(Qt::white);
            p.drawText(text, Qt::AlignCenter, m_badge);
        }
        if (!m_problem.isEmpty()) {
            const qreal s = m_mode == Mode::Grid ? 20 : 24;
            composeui::drawAlert(p, QRectF(pic.left() + 8, pic.top() + 8, s, s), m_theme.error, m_theme.dark ? QColor(0x20, 0x22, 0x25) : QColor(Qt::white));
        }
        if (m_focusWithin && m_keyboardFocus && m_mode == Mode::Grid)
            composeui::drawFocusRing(p, pic, r, m_theme.accent);

        if (m_mode == Mode::Grid) {
            QFont name = demiBold(font());
            p.setFont(name);
            p.setPen(m_theme.text);
            const int nameH = QFontMetrics(name).height();
            drawLine(p, QRectF(2, pic.bottom() + 8, width() - 4, nameH), m_name, Qt::ElideMiddle);
            p.setFont(font());
            p.setPen(m_problem.isEmpty() ? m_theme.muted : m_theme.error);
            drawLine(p, QRectF(2, pic.bottom() + 8 + nameH + 2, width() - 4, fontMetrics().height()), m_meta, Qt::ElideRight);
        }
    }

    void paintFileCard(QPainter& p)
    {
        const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(withAlpha(m_theme.text, m_theme.dark ? 0.06 : 0.10), 1.0));
        p.setBrush(m_theme.surface);
        p.drawRoundedRect(card, 12, 12);
        const QRectF art(14, (height() - 48) / 2.0, 44, 48);
        p.save();
        if (!m_canSend)
            p.setOpacity(0.5);
        composeui::drawFileArt(p, art, m_extension, composeui::fileTint(m_extension, m_theme.dark), m_theme);
        p.restore();
        const qreal  left  = art.right() + 14;
        const qreal  right = (spoiler ? spoiler->x() : remove ? remove->x() : width()) - 12;
        QFont        name  = demiBold(font());
        const int    nameH = QFontMetrics(name).height();
        const int    metaH = fontMetrics().height();
        const qreal  top   = (height() - nameH - 2 - metaH) / 2.0;
        p.setFont(name);
        p.setPen(m_theme.text);
        drawLine(p, QRectF(left, top, right - left, nameH), m_name, Qt::ElideMiddle);
        p.setFont(font());
        qreal metaLeft = left;
        if (!m_problem.isEmpty()) {
            composeui::drawAlert(p, QRectF(left, top + nameH + 2 + (metaH - 14) / 2.0, 14, 14), m_theme.error, m_theme.dark ? QColor(0x20, 0x22, 0x25) : QColor(Qt::white));
            metaLeft += 20;
        }
        p.setPen(m_problem.isEmpty() ? m_theme.muted : m_theme.error);
        drawLine(p, QRectF(metaLeft, top + nameH + 2, right - metaLeft, metaH), m_meta, Qt::ElideRight);
    }

    ComposeDialog*    m_dialog;
    int               m_id;
    Mode              m_mode;
    PillBar*          m_pill   = nullptr;
    composeui::Fader* m_reveal = nullptr;
    Theme             m_theme;
    QImage            m_image;    // device pixels, sized for the box
    QImage            m_backdrop; // 2.3 compose-v2 review: the stage's bands (spoiler::blurredStill)
    QString           m_name;
    QString           m_meta;
    QString           m_problem;
    QString           m_badge;
    QString           m_extension;
    MediaKind         m_kind          = MediaKind::Other;
    bool              m_pasted        = false;
    bool              m_spoiler       = false;
    bool              m_canSend       = true;
    bool              m_probed        = false;
    bool              m_hovered       = false;
    bool              m_focusWithin   = false;
    bool              m_keyboardFocus = false;
};

// Over the window while files are dragged onto it.
class ComposeDialog::DropHint : public QWidget
{
  public:
    explicit DropHint(QWidget* parent)
        : QWidget(parent)
    {
        setProperty("tsmFlat", true);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

    void setTheme(const Theme& theme)
    {
        m_theme = theme;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(composeui::mixColors(m_theme.body, m_theme.accent, 0.06), 0.96));
        p.drawRoundedRect(frame, 14, 14);
        QPen border(m_theme.accent, 2.0, Qt::DashLine);
        border.setDashPattern({4.0, 3.0});
        p.setPen(border);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 14, 14);
        const QPointF centre = frame.center();
        const QRectF  disc(centre.x() - 32, centre.y() - 52, 64, 64);
        p.setPen(Qt::NoPen);
        p.setBrush(composeui::mixColors(m_theme.body, m_theme.accent, 0.20));
        p.drawEllipse(disc);
        composeui::drawGlyph(p, Glyph::Upload, disc.adjusted(18, 18, -18, -18), m_theme.accent);
        QFont font = scaledFont(demiBold(this->font()), 1.25);
        p.setFont(font);
        p.setPen(m_theme.text);
        p.drawText(QRectF(frame.left(), disc.bottom() + 12, frame.width(), QFontMetricsF(font).height()), Qt::AlignHCenter | Qt::AlignTop, i18n::t("Drop to add them"));
    }

  private:
    Theme m_theme;
};

// 2.3 compose-v2: nothing added yet: a drop zone that opens the file picker when clicked.
class ComposeDialog::EmptyZone : public QAbstractButton
{
  public:
    explicit EmptyZone(QWidget* parent)
        : QAbstractButton(parent)
    {
        setObjectName(QString::fromLatin1("tsmComposeEmpty"));
        setProperty("tsmFlat", true);
        setFixedHeight(184);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setText(i18n::t("Drop files here"));
        setAccessibleName(i18n::t("Add files"));
        setAccessibleDescription(subtitle());
        m_hover = new composeui::Fader(this);
    }

    void setTheme(const Theme& theme)
    {
        m_theme = theme;
        update();
    }

  protected:
    static QString subtitle() { return i18n::t("or paste them with Ctrl+V, or click to choose files"); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal  hover = m_hover->value();
        const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
        p.setPen(Qt::NoPen);
        p.setBrush(composeui::mixColors(m_theme.surface, composeui::mixColors(m_theme.surface, m_theme.accent, 0.08), hover));
        p.drawRoundedRect(frame, 14, 14);
        QPen border(composeui::mixColors(m_theme.dark ? composeui::mixColors(m_theme.body, m_theme.text, 0.28) : composeui::mixColors(m_theme.body, m_theme.text, 0.30),
                                         m_theme.accent, hover),
                    1.5, Qt::DashLine);
        border.setDashPattern({5.0, 4.0});
        p.setPen(border);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 14, 14);
        const QFont         title = scaledFont(demiBold(font()), 1.15);
        const QFontMetricsF tfm(title);
        const QFontMetricsF sfm(font());
        const qreal         block = 56 + 14 + tfm.height() + 4 + sfm.height();
        const qreal         top   = frame.center().y() - block / 2;
        const QRectF        disc(frame.center().x() - 28, top, 56, 56);
        p.setPen(Qt::NoPen);
        p.setBrush(composeui::mixColors(m_theme.body, m_theme.accent, 0.16 + 0.06 * hover));
        p.drawEllipse(disc);
        composeui::drawGlyph(p, Glyph::Upload, disc.adjusted(16, 16, -16, -16), m_theme.accent);
        p.setFont(title);
        p.setPen(m_theme.text);
        p.drawText(QRectF(frame.left(), disc.bottom() + 14, frame.width(), tfm.height()), Qt::AlignCenter, text());
        p.setFont(font());
        p.setPen(m_theme.muted);
        p.drawText(QRectF(frame.left(), disc.bottom() + 14 + tfm.height() + 4, frame.width(), sfm.height()), Qt::AlignCenter, subtitle());
        if (hasFocus() && m_focus.keyboard())
            composeui::drawFocusRing(p, frame, 14, m_theme.accent);
    }

    void enterEvent(QEvent* event) override
    {
        QAbstractButton::enterEvent(event);
        m_hover->setTarget(1.0);
    }

    void leaveEvent(QEvent* event) override
    {
        QAbstractButton::leaveEvent(event);
        m_hover->setTarget(0.0);
    }

    void focusInEvent(QFocusEvent* event) override
    {
        m_focus.onFocusIn(event, this);
        QAbstractButton::focusInEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            click();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }

  private:
    Theme                   m_theme;
    composeui::Fader*       m_hover;
    composeui::FocusTracker m_focus;
};

// 2.3 compose-v2: a tinted box with the alert mark: why nothing can be sent, what is skipped.
class ComposeDialog::Callout : public QWidget
{
  public:
    explicit Callout(QWidget* parent)
        : QWidget(parent)
    {
        setProperty("tsmFlat", true);
        m_icon = new QLabel(this);
        m_icon->setFixedSize(16, 16);
        m_text = new QLabel(this);
        m_text->setTextFormat(Qt::PlainText);
        m_text->setWordWrap(true);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(12, 10, 12, 10);
        row->setSpacing(10);
        row->addWidget(m_icon, 0, Qt::AlignTop);
        row->addWidget(m_text, 1);
        hide();
    }

    QLabel* textLabel() const { return m_text; }

    void setTheme(const Theme& theme)
    {
        m_theme = theme;
        m_icon->setPixmap(alertPixmap(16, devicePixelRatioF(), theme.error, theme.dark ? QColor(0x20, 0x22, 0x25) : QColor(Qt::white)));
        m_icon->setContentsMargins(0, qMax(0, (m_text->fontMetrics().height() - 16) / 2), 0, 0);
        m_icon->setFixedHeight(16 + qMax(0, (m_text->fontMetrics().height() - 16) / 2));
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(composeui::mixColors(m_theme.errorTint, m_theme.error, 0.30), 1.0));
        p.setBrush(m_theme.errorTint);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    }

  private:
    Theme   m_theme;
    QLabel* m_icon;
    QLabel* m_text;
};

// 2.3 compose-v2: the destination's mark in the header: # for a channel, a person for a private chat,
// a globe for the whole server.
class ComposeDialog::Badge : public QWidget
{
  public:
    Badge(Glyph glyph, QWidget* parent)
        : QWidget(parent)
        , m_glyph(glyph)
    {
        setProperty("tsmFlat", true);
        setFixedSize(40, 40);
    }

    void setTheme(const Theme& theme)
    {
        m_theme = theme;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(composeui::mixColors(m_theme.body, m_theme.accent, m_theme.dark ? 0.20 : 0.14));
        p.drawRoundedRect(QRectF(rect()), 12, 12);
        composeui::drawGlyph(p, m_glyph, QRectF(10, 10, 20, 20), m_theme.accent);
    }

  private:
    Glyph m_glyph;
    Theme m_theme;
};

// ============================================================================================
// The window
// ============================================================================================

ComposeDialog::ComposeDialog(ComposeHost host, const ChatTarget& target, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
    , m_host(std::move(host))
    , m_target(target)
    , m_closing(std::make_shared<std::atomic_bool>(false))
{
    // fromLatin1: plugin shutdown finds it by this name; it lives among TeamSpeak's windows.
    setObjectName(QString::fromLatin1("tsmediaCompose"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setLayoutDirection(Qt::LeftToRight);
    setWindowTitle(i18n::t("Send to chat"));
    setAcceptDrops(true);
    m_pool.setMaxThreadCount(2);
    m_limitMB = m_host.uploadLimitMB ? m_host.uploadLimitMB(m_target) : 100;
    if (m_host.linkContext)
        m_linkContext = m_host.linkContext(m_target);

    const QScreen* screen = parent ? parent->screen() : this->screen();
    const QRect    avail  = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    setFixedWidth(avail.width() < 760 ? kNarrowWidth : kWidth);
    if (avail.height() < 800)
        m_previewMaxHeight = 220; // the whole window stays within 80% of a small screen

    // ---- header: where it goes --------------------------------------------------------------------
    const Glyph destination = m_target.mode == TextMessageTarget_SERVER ? Glyph::Server : m_target.mode == TextMessageTarget_CLIENT ? Glyph::Person : Glyph::Channel;
    m_badge  = new Badge(destination, this);
    m_header = new QLabel(this);
    m_header->setTextFormat(Qt::PlainText);
    m_header->setWordWrap(true);
    m_header->setFont(scaledFont(demiBold(font()), 1.2));
    m_header->setText(i18n::t("Send to %1").arg(m_host.describeTarget ? m_host.describeTarget(m_target) : QString()));
    m_summary = roleLabel(QString(), "hint", this);
    m_summary->setWordWrap(false);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addStretch(1);
    titles->addWidget(m_header);
    titles->addWidget(m_summary);
    if (m_target.mode == TextMessageTarget_SERVER) {
        m_serverNote = roleLabel(i18n::t("Everyone on this server gets this message."), "hint", this);
        titles->addWidget(m_serverNote);
    }
    titles->addStretch(1);
    auto* header = new QHBoxLayout;
    header->setSpacing(12);
    header->addWidget(m_badge, 0, Qt::AlignTop);
    header->addLayout(titles, 1);

    // 2.2 presence: "3 of 5 people here will see it in the chat" (the plugin-protocol feature's widget),
    // as a quiet status line. In a box of ours, so hiding it for the banner never overrides what the
    // widget does by itself.
    if (QWidget* presence = compose::createPresenceLine(this, m_target)) {
        m_presence = new QWidget(this);
        m_presence->setProperty("tsmFlat", true);
        auto* presenceBox = new QVBoxLayout(m_presence);
        presenceBox->setContentsMargins(0, 0, 0, 0);
        presence->setParent(m_presence);
        presenceBox->addWidget(presence);
        for (QLabel* label : presence->findChildren<QLabel*>())
            label->setProperty("role", QString::fromLatin1("hint"));
        m_presence->installEventFilter(this); // 2.3: its height changes ("Who?" opens a list): the window follows
    }

    // Why nothing can be sent right now (replaces the presence line; Send is off).
    m_banner = new Callout(this);

    // ---- the items (rebuilt as they change) ------------------------------------------------------
    m_itemsHost = new QWidget(this);
    m_itemsHost->setProperty("tsmFlat", true);
    m_itemsLayout = new QVBoxLayout(m_itemsHost);
    m_itemsLayout->setContentsMargins(0, 0, 0, 0);
    m_itemsLayout->setSpacing(0);

    m_album = new composeui::SwitchRow(Glyph::Album, i18n::t("Send as an a&lbum"), this);
    m_album->setObjectName(QString::fromLatin1("tsmComposeAlbum"));
    m_album->setToolTip(i18n::t("Shows them together in a grid. Up to %1 per album.").arg(MediaLink::kMaxAlbumItems));
    m_album->setChecked(m_host.albumDefault);
    m_album->hide();

    // ---- caption ----------------------------------------------------------------------------------
    m_caption = new QLineEdit(this);
    m_caption->setObjectName(QString::fromLatin1("tsmComposeCaption"));
    m_caption->setMaxLength(kCaptionMaxChars);
    m_caption->setPlaceholderText(i18n::t("Add a caption…"));
    m_caption->setAccessibleName(i18n::t("Caption"));
    m_caption->setClearButtonEnabled(false);
    m_caption->setAcceptDrops(false); // files dropped on it are added, not typed in
    m_caption->installEventFilter(this);
    EmojiInput::addPickerButton(m_caption); // 2.2 emoji: the emoji picker for the caption
    auto* captionKey = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_C), this); // was the label's &Caption
    connect(captionKey, &QShortcut::activated, this, [this] { m_caption->setFocus(Qt::ShortcutFocusReason); });
    m_captionHelp = roleLabel(QString(), "hint", this);
    m_counter     = roleLabel(QString(), "hint", this);
    m_counter->setWordWrap(false);
    m_counter->setAlignment(Qt::AlignRight | Qt::AlignTop);
    m_splitHint = roleLabel(i18n::t("This caption is too long to share a message with the file, so it's sent as its own message right above it."), "hint", this);
    m_splitHint->hide();
    m_captionNotes = new QWidget(this);
    m_captionNotes->setProperty("tsmFlat", true);
    auto* captionNotes = new QHBoxLayout(m_captionNotes);
    captionNotes->setContentsMargins(4, 0, 4, 0);
    captionNotes->setSpacing(kSection);
    m_captionHelp->setParent(m_captionNotes);
    m_counter->setParent(m_captionNotes);
    captionNotes->addWidget(m_captionHelp, 1);
    captionNotes->addWidget(m_counter, 0, Qt::AlignTop);
    m_splitHint->setContentsMargins(4, 0, 4, 0);
    auto* caption = new QVBoxLayout;
    caption->setSpacing(6);
    caption->addWidget(m_caption);
    caption->addWidget(m_captionNotes);
    caption->addWidget(m_splitHint);

    // ---- what can't be sent ----------------------------------------------------------------------
    m_warning = new Callout(this);

    // ---- the footer band: Add files at the left, then Send and Cancel (Windows order) -------------
    m_footer = new QWidget(this);
    m_footer->setProperty("tsmFlat", true);
    m_addFiles = new composeui::Button(composeui::Button::Kind::Ghost, i18n::t("&Add files…"), m_footer);
    m_addFiles->setObjectName(QString::fromLatin1("tsmComposeAddFiles"));
    m_addFiles->setGlyph(Glyph::Plus);
    m_addFiles->setToolTip(i18n::t("Add files (Ctrl+O). You can also drop files here or paste them."));
    // 2.3 compose-v2: the button was never connected (2.2.0 to 2.2.1): a click did nothing, only Ctrl+O
    // opened the picker.
    connect(m_addFiles, &QAbstractButton::clicked, this, [this] { addFromPicker(); });
    auto* buttons = new QDialogButtonBox(m_footer);
    buttons->setProperty("tsmFlat", true);
    m_send   = new composeui::Button(composeui::Button::Kind::Primary, i18n::t("Send"), buttons);
    m_cancel = new composeui::Button(composeui::Button::Kind::Secondary, i18n::t("Cancel"), buttons);
    m_send->setObjectName(QString::fromLatin1("tsmComposeSend"));
    m_cancel->setObjectName(QString::fromLatin1("tsmComposeCancel"));
    buttons->addButton(m_send, QDialogButtonBox::AcceptRole);
    buttons->addButton(m_cancel, QDialogButtonBox::RejectRole);
    m_send->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { send(); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });
    auto* footer = new QHBoxLayout(m_footer);
    footer->setContentsMargins(kMargin - 8, 12, kMargin, 12);
    footer->setSpacing(8);
    footer->addWidget(m_addFiles);
    footer->addStretch(1);
    footer->addWidget(buttons);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
    body->setSpacing(kSection);
    body->addLayout(header);
    // 2.2 reply: the reply these files go out as; dropping it (its x) lays the window out again.
    if (QWidget* reply = m_host.replyLine ? m_host.replyLine(this, m_target) : nullptr) {
        body->addWidget(reply);
        connect(reply, &QObject::destroyed, this, [this] { QMetaObject::invokeMethod(this, [this] { fitHeight(); }, Qt::QueuedConnection); });
    }
    if (m_presence)
        body->addWidget(m_presence);
    body->addWidget(m_banner);
    body->addWidget(m_itemsHost);
    body->addWidget(m_album);
    body->addLayout(caption);
    body->addWidget(m_warning);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(body);
    layout->addWidget(m_footer);

    m_dropHint = new DropHint(this);

    connect(m_caption, &QLineEdit::textChanged, this, [this] { updateCaption(); });
    connect(m_album, &QAbstractButton::toggled, this, [this] {
        if (isVisible())
            m_changed = true;
        updateState();
    });

    // Connected? Password? Partner still there? Checked while the window is open.
    m_targetTimer = new QTimer(this);
    m_targetTimer->setInterval(kTargetCheckMs);
    connect(m_targetTimer, &QTimer::timeout, this, [this] { refreshTarget(); });
    m_targetTimer->start();

    applyTheme();
    refreshTarget();
    rebuildItems();
}

ComposeDialog::~ComposeDialog()
{
    // Workers run code of this DLL and post back to this window: done before either goes away.
    m_closing->store(true);
    m_pool.clear();
    m_pool.waitForDone();
    // 2.2 editor: the edited copies are ours (what was sent went to Core as separate links or copies).
    for (const EditRecord& edit : qAsConst(m_edits)) {
        if (!edit.folder.isEmpty())
            QDir(edit.folder).removeRecursively();
    }
}

// ---- items --------------------------------------------------------------------------------------

void ComposeDialog::addFiles(const QStringList& paths)
{
    QSet<QString> known;
    for (const Item& item : qAsConst(m_items)) {
        if (!item.isPasted())
            known.insert(samePathKey(item.path));
    }
    for (const EditRecord& edit : qAsConst(m_edits)) { // 2.2 editor: an edited file is still that file
        if (!edit.original.isPasted())
            known.insert(samePathKey(edit.original.path));
    }
    const qint64 limit = static_cast<qint64>(qMax(0, m_limitMB)) * 1024 * 1024;
    bool         added = false;
    for (const QString& raw : paths) {
        if (raw.isEmpty())
            continue;
        const QFileInfo fi(raw);
        const QString   key = samePathKey(raw);
        if (known.contains(key))
            continue;
        if (m_items.size() >= compose::kMaxItems) {
            m_overflow = true;
            break;
        }
        known.insert(key);
        Item item;
        item.id       = ++m_nextId;
        item.path     = fi.absoluteFilePath();
        item.fileName = fi.fileName();
        item.kind     = kindForFileName(item.fileName);
        item.size     = fi.size();
        item.problem  = compose::checkFile(fi.exists(), fi.isFile(), item.size, limit);
        // 2.4 compress: a video over the limit may still go, made smaller; the probe tells.
        item.compressible = m_host.compressOptions && videocompress::isCompressibleVideoName(item.fileName) && m_host.compressOptions().mediaFoundation;
        if (item.compressible && item.problem == Problem::TooLarge)
            item.problem = Problem::None;
        m_items.append(item);
        if (item.problem != Problem::Missing && item.problem != Problem::Empty)
            startProbe(item);
        else
            m_items.last().probed = true;
        added = true;
    }
    if (!added && !m_overflow)
        return;
    if (isVisible())
        m_changed = true;
    rebuildItems();
}

void ComposeDialog::addImage(const QImage& image)
{
    if (image.isNull())
        return;
    if (m_items.size() >= compose::kMaxItems) {
        m_overflow = true;
        updateState();
        return;
    }
    Item item;
    item.id       = ++m_nextId;
    item.image    = image;
    item.fileName = QStringLiteral("Pasted image.png");
    item.kind     = MediaKind::Image;
    item.pixels   = image.size();
    m_items.append(item);
    startProbe(item);
    if (isVisible())
        m_changed = true;
    rebuildItems();
}

void ComposeDialog::setPrefilledCaption(const QString& caption)
{
    m_prefill = caption;
    m_caption->setText(caption);
    m_caption->setCursorPosition(caption.size());
}

void ComposeDialog::startProbe(const Item& item)
{
    ++m_pending;
    const qreal  dpr     = devicePixelRatioF();
    const QSize  maxBox  = QSize(kPreviewWidth, kPreviewHeight) * dpr;
    const int    minSide = qRound(kTileMax * dpr);
    const int    id      = item.id;
    const QString path   = item.path;
    const QImage  pasted = item.image;
    const MediaKind kind = item.kind;
    const bool    facts  = item.compressible; // 2.4 compress
    const auto    closing = m_closing;
    ComposeDialog* self   = this; // waits for this pool in its destructor
    m_pool.start([self, id, path, pasted, kind, maxBox, minSide, closing, facts] {
        ProbeData data;
        if (!pasted.isNull()) {
            data.pixels       = pasted.size();
            const QSize target = decodeSize(pasted.size(), maxBox, minSide);
            data.thumb         = target == pasted.size() ? pasted : pasted.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        } else {
            data = probeFile(path, kind, maxBox, minSide, *closing, facts);
        }
        if (closing->load())
            return;
        QMetaObject::invokeMethod(self, [self, id, data] {
            if (data.hasFacts)
                self->setVideoFacts(id, data.facts); // 2.4 compress: before onProbed, which shows them
            self->onProbed(id, data.readable, data.pixels, data.durationMs, data.thumb);
        }, Qt::QueuedConnection);
    });
}

void ComposeDialog::onProbed(int id, bool readable, const QSize& pixels, qint64 durationMs, const QImage& thumb)
{
    m_pending = qMax(0, m_pending - 1);
    const int index = indexOf(id);
    if (index < 0)
        return; // removed meanwhile
    Item& item = m_items[index];
    item.probed = true;
    if (!readable && item.problem == Problem::None)
        item.problem = Problem::Unreadable;
    if (pixels.isValid() && !pixels.isEmpty())
        item.pixels = pixels;
    if (durationMs > 0)
        item.durationMs = durationMs;
    if (!thumb.isNull())
        m_thumbs.insert(id, thumb);

    if (Tile* tile = m_tiles.value(id)) {
        tile->setItem(item, m_limitMB, composeui::themeFor(palette()), editBlocker(id));
        refreshThumb(id);
    } else if (id == m_singleId) {
        // The large preview gets its final size, the details their numbers; the focus stays put
        // (rebuildItems gives it back to the same control).
        rebuildItems();
        return;
    }
    updateState();
}

// ---- 2.4 compress: the Quality chip ----------------------------------------------------------------

void ComposeDialog::setVideoFacts(int id, const videocompress::VideoFacts& facts)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    Item& item      = m_items[index];
    item.facts      = facts;
    item.factsKnown = true;
    planQuality(item);
}

// The planner's entries for the item at the window's limit, and what that means for sending it: over the
// limit it goes only when an entry that fits is in effect.
void ComposeDialog::planQuality(Item& item)
{
    if (!item.compressible || !item.factsKnown || !m_host.compressOptions)
        return;
    videocompress::Options options = m_host.compressOptions();
    options.limitBytes             = static_cast<quint64>(qMax(0, m_limitMB)) * 1024 * 1024;
    item.facts.bytes               = static_cast<quint64>(qMax<qint64>(0, item.size));
    item.choices                   = videocompress::choices(item.facts, options);
    if (item.quality >= item.choices.size())
        item.quality = -1;
    item.compressProblem.clear();
    const bool over = item.facts.bytes > options.limitBytes;
    if (item.problem != Problem::None && item.problem != Problem::TooLarge)
        return; // missing, empty, unreadable: that says it
    if (!over) {
        if (item.problem == Problem::TooLarge)
            item.problem = Problem::None;
        return;
    }
    if (effectiveQuality(item) >= 0) {
        item.problem = Problem::None;
        return;
    }
    // Over the limit and nothing fits: the planner says why.
    options.request                = videocompress::Request::Auto;
    const videocompress::Plan plan = videocompress::planCompression(item.facts, options);
    item.problem                   = Problem::TooLarge;
    if (plan.decision == videocompress::Decision::Fail)
        item.compressProblem = videocompress::failureText(plan, m_limitMB);
    else
        item.compressProblem = videocompress::failureText(videocompress::Plan{videocompress::Decision::Fail, videocompress::Reason::CannotFit}, m_limitMB);
}

void ComposeDialog::setQuality(int id, int quality)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    Item& item = m_items[index];
    if (quality < 0 || quality >= item.choices.size() || !item.choices.at(quality).enabled || quality == effectiveQuality(item))
        return;
    item.quality = quality;
    if (isVisible())
        m_changed = true;
    if (Tile* tile = m_tiles.value(id))
        tile->setItem(item, m_limitMB, composeui::themeFor(palette()), editBlocker(id));
    updateState();
}

// 2.3 compose-v2: a choice from the chip's menu or its arrow keys. The single view shows what the
// choice means: rebuilt, with the focus kept on the chip.
void ComposeDialog::chooseQuality(int id, int index)
{
    const int before = indexOf(id) >= 0 ? effectiveQuality(m_items.at(indexOf(id))) : -1;
    setQuality(id, index);
    const int at = indexOf(id);
    if (at < 0 || effectiveQuality(m_items.at(at)) == before)
        return;
    if (id == m_singleId)
        rebuildItems(); // the focus stays on the chip (rebuildItems gives it back)
}

void ComposeDialog::stepQuality(int id, int delta)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    const Item& item = m_items.at(index);
    int         next = effectiveQuality(item);
    if (next < 0)
        return;
    for (int i = next + delta; i >= 0 && i < item.choices.size(); i += delta) {
        if (item.choices.at(i).enabled) {
            chooseQuality(id, i);
            return;
        }
    }
}

void ComposeDialog::openQualityMenu(int id, QWidget* anchor)
{
    const int index = indexOf(id);
    if (index < 0 || !anchor)
        return;
    const Item& item = m_items.at(index);
    if (!item.factsKnown || item.choices.isEmpty())
        return;
    // Not exec(): no nested event loop (plugin shutdown may delete this window at any time).
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName(QString::fromLatin1("tsmComposeQualityMenu"));
    menu->setToolTipsVisible(true);
    auto*     group   = new QActionGroup(menu);
    const int current = effectiveQuality(item);
    for (int i = 0; i < item.choices.size(); ++i) {
        const videocompress::Choice& choice = item.choices.at(i);
        QAction*                     action = menu->addAction(choice.longLabel);
        action->setCheckable(true);
        action->setChecked(i == current);
        action->setEnabled(choice.enabled); // Original over the limit: listed, but it can't be picked
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id, i] { chooseQuality(id, i); });
    }
    menu->popup(anchor->mapToGlobal(QPoint(0, anchor->height() + 4)));
}

bool ComposeDialog::fitsCompressed(const Item& item) const
{
    const int quality = effectiveQuality(item);
    return item.compressible && item.factsKnown && quality >= 0
           && (item.choices.at(quality).request != videocompress::Request::Original || item.size <= static_cast<qint64>(qMax(0, m_limitMB)) * 1024 * 1024);
}

void ComposeDialog::setQualityIndex(int index, int quality)
{
    if (index >= 0 && index < m_items.size())
        chooseQuality(m_items.at(index).id, quality);
}

QVector<videocompress::Choice> ComposeDialog::qualityChoices(int index) const
{
    return index >= 0 && index < m_items.size() ? m_items.at(index).choices : QVector<videocompress::Choice>();
}

int ComposeDialog::indexOf(int id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).id == id)
            return i;
    }
    return -1;
}

bool ComposeDialog::isBusy() const
{
    return m_pending > 0 || m_editJobs > 0; // 2.2 editor: opening or saving a picture
}

void ComposeDialog::removeItem(int id)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    m_items.remove(index);
    m_thumbs.remove(id);
    // 2.2 editor: its edited copy goes too.
    const auto edit = m_edits.find(id);
    if (edit != m_edits.end()) {
        if (!edit->folder.isEmpty())
            QDir(edit->folder).removeRecursively();
        m_edits.erase(edit);
    }
    m_changed  = true;
    m_overflow = false;
    m_sendError.clear();
    // 2.3 compose-v2 review: the focus goes on to the next tile only when it was on the removed one (Delete,
    // or Enter / Space on its x). A click on an x takes no focus, and the focus stays where it was: moved to
    // the next tile's x, the caption being typed would go to it (Space removing that tile, S marking it).
    const int focusIndex = focusSpot().id == id ? index : -1;
    // Later: this may run inside the tile's own key or click handler.
    QMetaObject::invokeMethod(this, [this, focusIndex] { rebuildItems(focusIndex); }, Qt::QueuedConnection);
}

void ComposeDialog::toggleSpoiler(int id)
{
    const int index = indexOf(id);
    if (index >= 0)
        setSpoiler(index, !m_items.at(index).spoiler);
}

void ComposeDialog::setSpoiler(int index, bool spoiler)
{
    if (index < 0 || index >= m_items.size() || !compose::spoilerAllowed(m_items.at(index).kind) || m_items.at(index).spoiler == spoiler)
        return;
    Item& item  = m_items[index];
    item.spoiler = spoiler;
    if (isVisible())
        m_changed = true;
    if (Tile* tile = m_tiles.value(item.id))
        tile->setItem(item, m_limitMB, composeui::themeFor(palette()), editBlocker(item.id));
    if (m_singleTile && item.id == m_singleId)
        m_singleTile->setItem(item, m_limitMB, composeui::themeFor(palette()), editBlocker(item.id));
    updateState();
}

void ComposeDialog::setAlbum(bool album)
{
    m_album->setChecked(album);
}

// ---- views --------------------------------------------------------------------------------------

int ComposeDialog::contentWidth() const
{
    return width() - 2 * kMargin;
}

int ComposeDialog::gridColumns() const
{
    return contentWidth() >= 480 ? 3 : 2;
}

// The side of a grid tile; scrolls: the grid's scroll bar takes room at the right.
int ComposeDialog::tileSide(bool scrolls) const
{
    int room = contentWidth();
    if (scrolls && m_scroll) {
        m_scroll->verticalScrollBar()->ensurePolished();
        room -= m_scroll->verticalScrollBar()->sizeHint().width() + 4;
    } else if (scrolls)
        room -= style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 4;
    const int cols = gridColumns();
    return qMin(kTileMax, (room - (cols - 1) * kGridGap) / cols);
}

int ComposeDialog::gridHeight(int rows) const
{
    int height = 0;
    for (int row = 0; row < qMin(rows, m_rowHeights.size()); ++row)
        height += (row > 0 ? kGridGap : 0) + m_rowHeights.at(row);
    return height;
}

void ComposeDialog::rebuildItems(int focusIndex)
{
    // 2.3 compose-v2 review: a control of the old view with the focus gets it back in the new one (its
    // details arrived, its file changed, the quality was picked, another tile went). Meanwhile the focus
    // waits on the items' box: hiding the old view would pass it on to the caption with Tab's reason
    // (selecting its text).
    const FocusSpot spot = focusSpot();
    if (spot.inView)
        m_itemsHost->setFocus(Qt::OtherFocusReason);
    if (m_itemsView) {
        m_itemsLayout->removeWidget(m_itemsView);
        m_itemsView->hide();
        m_itemsView->deleteLater();
        m_itemsView = nullptr;
    }
    m_tiles.clear();
    m_scroll        = nullptr;
    m_singleTile    = nullptr;
    m_singleSpoiler = nullptr;
    m_singleQuality = nullptr; // 2.4 compress
    m_singleId      = -1;
    m_singleEdit    = nullptr; // 2.2 editor
    m_singleRevert  = nullptr;
    m_gridRows      = 0;

    QWidget*   view  = nullptr;
    EmptyZone* empty = nullptr;
    if (m_items.isEmpty()) {
        empty = new EmptyZone(m_itemsHost);
        connect(empty, &QAbstractButton::clicked, this, [this] { addFromPicker(); });
        view = empty;
    } else if (m_items.size() == 1) {
        view = buildSingle(m_items.first());
    } else {
        view = buildGrid();
    }
    m_itemsView = view;
    m_itemsLayout->addWidget(view);
    view->show();

    // Tab order follows the window: the items, Send as an album, the caption, Add files…, the buttons.
    QWidget* previous = nullptr;
    const auto chain  = [&previous](QWidget* next) {
        if (!next)
            return;
        if (previous)
            QWidget::setTabOrder(previous, next);
        previous = next;
    };
    chain(empty);
    for (const Item& item : qAsConst(m_items)) {
        if (Tile* tile = m_tiles.value(item.id)) {
            chain(tile->quality); // 2.4 compress
            chain(tile->revert);  // 2.2 editor
            chain(tile->edit);
            chain(tile->spoiler);
            chain(tile->remove);
        }
    }
    if (m_singleTile) {
        chain(m_singleTile->spoiler);
        chain(m_singleTile->remove);
    }
    chain(m_singleEdit); // 2.2 editor
    chain(m_singleRevert);
    chain(m_singleQuality); // 2.4 compress
    chain(m_album);
    chain(m_caption);
    chain(m_addFiles);
    chain(m_send);
    chain(m_cancel);

    applyTheme();
    updateState();
    fitHeight();

    if (focusIndex >= 0) {
        // After Remove: the next tile's Remove (or the previous one's), or Add files… when none is left.
        if (!m_items.isEmpty() && !m_tiles.isEmpty()) {
            const Item& next = m_items.at(qMin(focusIndex, m_items.size() - 1));
            if (Tile* tile = m_tiles.value(next.id)) {
                tile->remove->setFocus(Qt::OtherFocusReason);
                if (m_scroll)
                    m_scroll->ensureWidgetVisible(tile);
            }
        } else if (m_items.isEmpty()) {
            m_addFiles->setFocus(Qt::OtherFocusReason);
        } else {
            m_caption->setFocus(Qt::OtherFocusReason);
        }
    } else if (spot.inView && !restoreFocus(spot) && m_itemsHost->hasFocus()) {
        m_caption->setFocus(Qt::OtherFocusReason); // that control is gone (its item, or the Quality chip)
    }
}

ComposeDialog::FocusSpot ComposeDialog::focusSpot() const
{
    FocusSpot      spot;
    QWidget* const focus = QApplication::focusWidget();
    if (!focus || !m_itemsView || !m_itemsView->isAncestorOf(focus))
        return spot; // elsewhere (also: TeamSpeak's own windows)
    spot.inView = true;
    spot.name   = focus->objectName();
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
        if (it.value()->isAncestorOf(focus))
            spot.id = it.key();
    }
    if (spot.id < 0 && m_singleId >= 0)
        spot.id = m_singleId; // the single view: its preview and its chips
    return spot;
}

bool ComposeDialog::restoreFocus(const FocusSpot& spot)
{
    if (!spot.inView || spot.name.isEmpty() || indexOf(spot.id) < 0)
        return false;
    QWidget* scope = m_tiles.value(spot.id);
    if (!scope && spot.id == m_singleId)
        scope = m_itemsView;
    if (!scope)
        return false;
    for (QWidget* widget : scope->findChildren<QWidget*>(spot.name)) {
        if (widget->isVisibleTo(this) && widget->isEnabled() && (widget->focusPolicy() & Qt::TabFocus)) {
            widget->setFocus(Qt::OtherFocusReason);
            if (m_scroll && scope != m_itemsView)
                m_scroll->ensureWidgetVisible(scope);
            return true;
        }
    }
    return false;
}

QWidget* ComposeDialog::buildSingle(const Item& itemIn)
{
    Item& item = m_items.first();
    Q_UNUSED(itemIn);
    auto* view = new QWidget(m_itemsHost);
    view->setProperty("tsmFlat", true);
    auto* layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_singleId = item.id;

    const Theme theme = composeui::themeFor(palette());
    // The large preview while a picture is coming or there is one; a file card when the worker found none
    // (a video it can't decode, a picture too large for a thumbnail).
    const bool media = isMedia(item) && (item.isPasted() || !item.probed || m_thumbs.contains(item.id));
    const int  room  = contentWidth();

    if (media) {
        // The stage is known before the picture: nothing moves when it arrives. Pictures keep their shape
        // (read from the header now if the worker hasn't yet); videos get 16:9 with the poster inside.
        if (!item.pixels.isValid() && !item.isPasted() && isPreviewableImage(item.kind)) {
            QImageReader reader(item.path);
            reader.setAutoTransform(true);
            const QSize raw = reader.size();
            if (raw.isValid())
                item.pixels = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90) ? raw.transposed() : raw;
        }
        const QSize maxBox(room, m_previewMaxHeight);
        int         stageH = qMin(maxBox.height(), room * 9 / 16);
        if (item.kind != MediaKind::Video && item.pixels.isValid() && !item.pixels.isEmpty()) {
            const QSize fitted = item.pixels.width() > maxBox.width() || item.pixels.height() > maxBox.height() ? item.pixels.scaled(maxBox, Qt::KeepAspectRatio) : item.pixels;
            stageH             = qBound(qMin(kMinStage, maxBox.height()), fitted.height(), maxBox.height());
        }
        m_singleTile = new Tile(this, item, Tile::Mode::Stage, QSize(room, stageH), false, view);
        layout->addWidget(m_singleTile);

        auto* name = new ElidedLabel(Qt::ElideMiddle, view);
        name->setFont(demiBold(font()));
        const QString shownName = compose::displayName(item);
        name->setFullText(shownName);
        name->setToolTip(QString::fromLatin1("<p style='white-space:pre'>%1</p>").arg(shownName.toHtmlEscaped()));
        auto* meta = new ElidedLabel(Qt::ElideRight, view);
        meta->setProperty("role", QString::fromLatin1("hint"));
        meta->setFullText(compose::metaText(item));
        auto* texts = new QVBoxLayout;
        texts->setContentsMargins(2, 0, 2, 0);
        texts->setSpacing(2);
        texts->addWidget(name);
        texts->addWidget(meta);
        layout->addSpacing(12);
        layout->addLayout(texts);
    } else {
        // A file without a picture: a card with its type art, name and details.
        m_singleTile = new Tile(this, item, Tile::Mode::FileCard, QSize(room, kFileCardH), false, view);
        layout->addWidget(m_singleTile);
    }
    m_singleTile->setItem(item, m_limitMB, theme, editBlocker(item.id));
    m_singleSpoiler = m_singleTile->spoiler;
    refreshThumb(item.id);

    // ---- chips: Edit / Revert (pictures), Quality (videos) ------------------------------------------
    auto* chips = new QHBoxLayout;
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(8);
    QString editNote;
    if (editingOffered() && isPicture(item)) { // 2.2 editor
        const QString blocker = editBlocker(item.id);
        m_singleEdit = new composeui::Chip(Glyph::Edit, i18n::t("&Edit"), view);
        m_singleEdit->setObjectName(QString::fromLatin1("tsmComposeEdit"));
        m_singleEdit->setEnabled(blocker.isEmpty());
        m_singleEdit->setToolTip(blocker.isEmpty() ? i18n::t("Crop, draw or hide details (E)") : blocker);
        m_singleEdit->setAccessibleName(i18n::t("Edit"));
        m_singleEdit->setAccessibleDescription(blocker);
        connect(m_singleEdit, &QAbstractButton::clicked, this, [this, id = item.id] { openEditor(id); });
        chips->addWidget(m_singleEdit);
        if (item.edited) {
            m_singleRevert = new composeui::Chip(Glyph::Revert, i18n::t("Re&vert to original"), view);
            m_singleRevert->setObjectName(QString::fromLatin1("tsmComposeRevert"));
            m_singleRevert->setToolTip(i18n::t("Revert to the original picture"));
            connect(m_singleRevert, &QAbstractButton::clicked, this, [this, id = item.id] { askRevert(id); });
            chips->addWidget(m_singleRevert);
        }
        editNote = blocker;
    }
    // 2.4 compress: the video's Quality, from the planner, with what it means.
    QString qualityNote;
    if (item.compressible && item.canSend() && (!item.factsKnown || !item.choices.isEmpty())) {
        m_singleQuality = new composeui::Chip(Glyph::Quality, qualityText(item, true), view);
        m_singleQuality->setObjectName(QString::fromLatin1("tsmComposeQuality"));
        m_singleQuality->setMenuIndicator(true);
        m_singleQuality->setMaximumTextWidth(room - 80);
        m_singleQuality->setEnabled(item.factsKnown && effectiveQuality(item) >= 0);
        m_singleQuality->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Q)); // was the label's &Quality
        m_singleQuality->setAccessibleName(i18n::t("Quality"));
        m_singleQuality->setToolTip(i18n::t("Compressed on this computer to an MP4 that plays everywhere. Smaller downloads faster."));
        m_singleQuality->setStepHandler([this, id = item.id](int delta) { stepQuality(id, delta); });
        connect(m_singleQuality, &QAbstractButton::clicked, this, [this, id = item.id] { openQualityMenu(id, m_singleQuality); });
        chips->addWidget(m_singleQuality);
        const int  current = effectiveQuality(item);
        const bool over    = item.size > static_cast<qint64>(qMax(0, m_limitMB)) * 1024 * 1024;
        if (!item.factsKnown)
            qualityNote = QString();
        else if (over && current >= 0)
            qualityNote = i18n::t("It's over your %1 MB upload limit as it is, so a smaller quality that fits was picked.").arg(m_limitMB);
        else if (current >= 0 && item.choices.at(current).request != videocompress::Request::Original)
            qualityNote = item.choices.at(current).plan.reason == videocompress::Reason::Convert
                              ? i18n::t("Converted on this computer to an MP4 that plays for everyone.")
                              : i18n::t("Made smaller on this computer before it's sent: an MP4 that plays everywhere.");
        m_singleQuality->setAccessibleDescription(qualityNote);
    }
    if (m_singleEdit || m_singleQuality) {
        if (!editNote.isEmpty()) {
            auto* hint = roleLabel(editNote, "hint", view);
            hint->setWordWrap(false);
            chips->addWidget(hint, 1);
        } else {
            chips->addStretch(1);
        }
        layout->addSpacing(12);
        layout->addLayout(chips);
    }
    if (!qualityNote.isEmpty()) {
        auto* hint = roleLabel(qualityNote, "hint", view);
        hint->setContentsMargins(2, 0, 2, 0);
        layout->addSpacing(8);
        layout->addWidget(hint);
    }

    if (!item.canSend()) {
        auto* problemRow = new QHBoxLayout;
        problemRow->setContentsMargins(2, 0, 2, 0);
        problemRow->setSpacing(8);
        auto* icon = new QLabel(view);
        icon->setFixedSize(16, 16);
        icon->setPixmap(alertPixmap(16, devicePixelRatioF(), theme.error, theme.dark ? QColor(0x20, 0x22, 0x25) : QColor(Qt::white)));
        const QString problem = problemFor(item, m_limitMB, true);
        auto*         text    = roleLabel(problem, "error", view);
        problemRow->addWidget(icon, 0, Qt::AlignTop);
        problemRow->addWidget(text, 1);
        layout->addSpacing(10);
        layout->addLayout(problemRow);
        view->setAccessibleDescription(problem);
    }
    return view;
}

QWidget* ComposeDialog::buildGrid()
{
    auto* view = new QWidget(m_itemsHost);
    view->setProperty("tsmFlat", true);
    auto* layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_scroll = new QScrollArea(view);
    m_scroll->setProperty("tsmFlat", true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_scroll->viewport()->setProperty("tsmFlat", true);
    auto* grid = new QWidget(m_scroll);
    grid->setProperty("tsmFlat", true);
    auto* gridLayout = new QGridLayout(grid);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setHorizontalSpacing(kGridGap);
    gridLayout->setVerticalSpacing(kGridGap);

    const int  cols      = gridColumns();
    const int  rowsTotal = (m_items.size() + cols - 1) / cols;
    m_gridScrolls        = rowsTotal > m_visibleRows || m_gridSqueezed;
    const int  side      = tileSide(m_gridScrolls);
    // A row with a video that has a Quality chip is taller; the others keep to the thumbnail and text.
    QVector<bool> chips(rowsTotal, false);
    for (int i = 0; i < m_items.size(); ++i)
        chips[i / cols] = chips.at(i / cols) || m_items.at(i).compressible;
    m_rowHeights.clear();
    for (int row = 0; row < rowsTotal; ++row)
        m_rowHeights << Tile::gridHeight(side, chips.at(row), font());
    const Theme theme = composeui::themeFor(palette());
    for (int i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items.at(i);
        auto*       tile = new Tile(this, item, Tile::Mode::Grid, QSize(side, m_rowHeights.at(i / cols)), chips.at(i / cols), grid);
        tile->setItem(item, m_limitMB, theme, editBlocker(item.id));
        gridLayout->addWidget(tile, i / cols, i % cols, Qt::AlignLeft | Qt::AlignTop);
        m_tiles.insert(item.id, tile);
    }
    gridLayout->setColumnStretch(cols, 1);
    gridLayout->setRowStretch(rowsTotal, 1);
    m_scroll->setWidget(grid);
    grid->setAutoFillBackground(false);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_gridRows = qMin(rowsTotal, m_visibleRows);
    m_scroll->setFixedHeight(gridHeight(m_gridRows)); // fitHeight() shows less on a small screen
    layout->addWidget(m_scroll);
    for (const Item& item : qAsConst(m_items))
        refreshThumb(item.id);
    return view;
}

QImage ComposeDialog::thumbFor(int id, const QSize& logical, bool cover) const
{
    const QImage source = m_thumbs.value(id);
    if (source.isNull())
        return {};
    const qreal dpr    = devicePixelRatioF();
    const QSize device = logical * dpr;
    QImage      out;
    if (cover) {
        out = compose::coverThumbnail(source, device);
    } else {
        const QSize fit = source.width() > device.width() || source.height() > device.height() ? source.size().scaled(device, Qt::KeepAspectRatio) : source.size();
        out             = fit == source.size() ? source : source.scaled(fit, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    out.setDevicePixelRatio(dpr);
    return out;
}

void ComposeDialog::refreshThumb(int id)
{
    if (indexOf(id) < 0)
        return;
    Tile* tile = m_tiles.value(id);
    if (!tile && id == m_singleId)
        tile = m_singleTile;
    if (!tile || tile->mode() == Tile::Mode::FileCard)
        return;
    tile->setImage(thumbFor(id, tile->pictureSize(), tile->mode() == Tile::Mode::Grid)); // none yet: the type art
}

// ---- state ----------------------------------------------------------------------------------------

void ComposeDialog::updateState()
{
    const int sendable = compose::sendableCount(m_items);
    m_send->setText(compose::sendButtonText(m_items));
    m_send->setEnabled(sendable > 0 && m_blocker.isEmpty() && !m_sending);
    m_summary->setText(summaryText(m_items)); // 2.3 compose-v2

    const bool albumOffered = compose::albumsEnabled() && compose::albumCandidates(m_items) >= 2;
    if (m_album->isHidden() == albumOffered) // isHidden: also right before the window is shown
        m_album->setVisible(albumOffered);
    // 2.3 compose-v2: the split ("12 images will be sent as 2 albums") under the switch, else what it does.
    const QString albumHint = albumOffered && m_album->isChecked() ? compose::albumHint(m_items) : QString();
    m_album->setSubtitle(albumHint.isEmpty() ? i18n::t("Shows them together in a grid. Up to %1 per album.").arg(MediaLink::kMaxAlbumItems) : albumHint);

    QString warning = m_sendError.isEmpty() ? compose::skippedText(m_items) : m_sendError;
    if (m_overflow)
        warning += (warning.isEmpty() ? QString() : QStringLiteral(" ")) + i18n::t("Only the first %1 files can be sent at once.").arg(compose::kMaxItems);
    QLabel* warningText = m_warning->textLabel();
    if (warning != warningText->text()) {
        warningText->setText(warning);
        m_warning->setVisible(!warning.isEmpty());
        if (!warning.isEmpty())
            announce(warningText);
    }
    updateCaption();
    fitHeight();
}

void ComposeDialog::updateCaption()
{
    const QString text = m_caption->text();
    const QString count = compose::captionCounterText(text.size());
    m_counter->setText(count);
    m_counter->setVisible(!count.isEmpty());

    const bool         album = !m_album->isHidden() && m_album->isChecked();
    const QVector<int> order = compose::postOrder(m_items, album);
    QString            help;
    if (order.size() >= 2) {
        const bool firstInAlbum = album && compose::albumCandidates(m_items) >= 2 && compose::isAlbumKind(m_items.at(order.first()));
        help                    = firstInAlbum ? i18n::t("Shown above the album.") : i18n::t("Shown above the first file.");
    }
    m_captionHelp->setText(help);
    m_captionHelp->setVisible(!help.isEmpty());
    const bool notes = !help.isEmpty() || !count.isEmpty();
    if (m_captionNotes->isHidden() == notes) {
        m_captionNotes->setVisible(notes);
        fitHeight();
    }

    const bool alone = compose::captionGoesAlone(text, m_items, album, m_linkContext);
    if (alone == m_splitHint->isHidden()) {
        m_splitHint->setVisible(alone);
        fitHeight();
    }
    QStringList description;
    if (!help.isEmpty())
        description << help;
    if (alone)
        description << m_splitHint->text();
    m_caption->setAccessibleDescription(description.join(QLatin1Char(' ')));
    // A Persian or Arabic caption starts at the right by itself: the field's alignment is logical (leading).
}

void ComposeDialog::refreshTarget()
{
    const QString blocker = m_host.blocker ? m_host.blocker(m_target) : QString();
    if (blocker.isEmpty() && m_host.describeTarget) {
        // A private chat partner may have been renamed.
        const QString header = i18n::t("Send to %1").arg(m_host.describeTarget(m_target));
        if (header != m_header->text())
            m_header->setText(header);
    }
    if (blocker == m_blocker && m_targetChecked)
        return;
    m_targetChecked = true;
    m_blocker       = blocker;
    m_banner->textLabel()->setText(blocker);
    m_banner->setVisible(!blocker.isEmpty());
    if (m_presence)
        m_presence->setVisible(blocker.isEmpty());
    if (!blocker.isEmpty())
        announce(m_banner->textLabel());
    updateState();
}

bool ComposeDialog::restat()
{
    const qint64 limit   = static_cast<qint64>(qMax(0, m_limitMB)) * 1024 * 1024;
    bool         changed = false;
    for (Item& item : m_items) {
        if (item.isPasted())
            continue;
        const QFileInfo fi(item.path);
        Problem         problem = compose::checkFile(fi.exists(), fi.isFile(), fi.size(), limit);
        // 2.4 compress: over the limit is fine while the quality in effect fits (re-planned if it grew).
        if (problem == Problem::TooLarge && item.compressible) {
            Item resized = item;
            resized.size = fi.size();
            planQuality(resized);
            problem = !resized.factsKnown || fitsCompressed(resized) ? Problem::None : Problem::TooLarge;
        }
        if (problem == Problem::None) {
            QFile file(item.path);
            if (!file.open(QIODevice::ReadOnly))
                problem = Problem::Unreadable;
        }
        if (problem != item.problem || fi.size() != item.size) {
            item.problem = problem;
            item.size    = fi.size();
            planQuality(item); // 2.4 compress: estimates for the new size
            changed      = true;
        }
    }
    if (changed)
        rebuildItems();
    return changed;
}

void ComposeDialog::fitHeight()
{
    QLayout* l = layout();
    if (!l)
        return;
    const auto measure = [this, l] {
        l->activate();
        return l->hasHeightForWidth() ? l->totalHeightForWidth(width()) : l->totalSizeHint().height();
    };
    if (m_scroll)
        m_scroll->setFixedHeight(gridHeight(m_gridRows)); // whole rows first
    const bool stage       = m_singleTile && m_singleTile->mode() == Tile::Mode::Stage;
    const int  stageBefore = stage ? m_singleTile->height() : 0;
    if (stage && m_singleTile->height() != m_singleTile->naturalHeight)
        m_singleTile->setFixedHeight(m_singleTile->naturalHeight); // the large preview at its own size first
    const QScreen* screen = parentWidget() ? parentWidget()->screen() : this->screen();
    const int      cap    = screen ? screen->availableGeometry().height() * 8 / 10 : 800;
    int            height = measure();
    // A long grid gets fewer visible rows rather than a window taller than the screen allows.
    while (height > cap && m_scroll && m_visibleRows > 1 && m_gridRows > 1) {
        --m_visibleRows;
        m_gridRows = qMin(m_gridRows, m_visibleRows);
        m_scroll->setFixedHeight(gridHeight(m_gridRows));
        height = measure();
    }
    // 2.3 compose-v2: on a small screen even one row may not fit: less of it shows, and it scrolls.
    if (height > cap && m_scroll && m_gridRows == 1) {
        const int visible = qMax(kMinGridView, gridHeight(1) - (height - cap));
        if (visible < gridHeight(1)) {
            m_gridSqueezed = true;
            m_scroll->setFixedHeight(visible);
            height = measure();
        }
    }
    // ... and the large preview gets lower.
    if (height > cap && stage) {
        const int lower = qMax(kMinGridView, m_singleTile->naturalHeight - (height - cap));
        if (lower < m_singleTile->height()) {
            m_singleTile->setFixedHeight(lower);
            height = measure();
        }
    }
    // 2.3 compose-v2 review: its picture is scaled again once, and only if its height changed (not twice
    // on every call on a small screen). (Only grids are built again below.)
    if (stage && m_singleTile->height() != stageBefore)
        refreshThumb(m_singleId);
    // The grid scrolls now: its tiles make room for the scroll bar (built again once).
    if (m_scroll && !m_refitting && !m_gridScrolls && !m_items.isEmpty()) {
        const int cols = gridColumns();
        if ((m_items.size() + cols - 1) / cols > m_visibleRows || m_gridSqueezed) {
            m_refitting = true;
            rebuildItems();
            m_refitting = false;
            return;
        }
    }
    setFixedHeight(qMin(height, qMax(cap, 200)));
}

bool ComposeDialog::isDirty() const
{
    return m_changed || m_caption->text().trimmed() != m_prefill.trimmed();
}

// ---- sending --------------------------------------------------------------------------------------

void ComposeDialog::send()
{
    if (m_sending)
        return;
    refreshTarget();
    // A file changed since it was added (gone, emptied, grown past the limit, locked): the window shows
    // it first, and the next Send goes with what is left.
    if (restat() || !m_blocker.isEmpty() || compose::sendableCount(m_items) == 0 || !m_host.send)
        return;

    SendRequest request;
    request.target  = m_target;
    request.caption = m_caption->text();
    request.album   = compose::albumsEnabled() && !m_album->isHidden() && m_album->isChecked();
    QStringList written; // pasted pictures written for this send
    QStringList editFolders; // 2.2 editor: folders of the edited copies handed to Core
    for (const Item& item : qAsConst(m_items)) {
        if (!item.canSend())
            continue;
        SendItem sendItem;
        if (item.isPasted()) {
            const QString path = m_host.savePastedImage ? m_host.savePastedImage(item.image) : QString();
            if (path.isEmpty()) {
                for (const QString& file : qAsConst(written))
                    QFile::remove(file);
                m_sendError = i18n::t("Couldn't prepare the pasted image. Check free disk space and try again.");
                updateState();
                return;
            }
            written << path;
            sendItem.path    = path;
            sendItem.pasted  = true;
            sendItem.ownTemp = true; // Core deletes it with the job
        } else if (item.edited) {
            // 2.2 editor: Core gets a link (or copy) of its own; the window's copy stays for another try.
            const QString path = handOver(item, &editFolders);
            if (path.isEmpty()) {
                for (const QString& file : qAsConst(written))
                    QFile::remove(file);
                for (const QString& folder : qAsConst(editFolders))
                    QDir(folder).removeRecursively();
                m_sendError = i18n::t("Couldn't prepare the edited picture. Check free disk space and try again.");
                updateState();
                return;
            }
            written << path;
            sendItem.path        = path;
            sendItem.ownTemp     = true;
            sendItem.displayName = item.fileName;
        } else {
            sendItem.path = item.path;
        }
        sendItem.spoiler = item.spoiler && compose::spoilerAllowed(item.kind);
        // 2.4 compress: the Quality picked (Auto when it is what the settings would do anyway).
        if (item.compressible && item.factsKnown && !item.choices.isEmpty())
            sendItem.quality = sendQualityFor(videocompress::requestFor(item.choices, effectiveQuality(item)));
        request.items.append(sendItem);
    }

    m_sending = true;
    const int batch = m_host.send(request);
    m_sending = false;
    if (batch == 0) {
        // Core said why in the chat (not connected, files gone) and removed the pasted files.
        refreshTarget();
        restat();
        updateState();
        return;
    }
    if (!m_album->isHidden() && m_host.rememberAlbum)
        m_host.rememberAlbum(m_album->isChecked());
    const QString caption = sanitizeCaption(m_caption->text()).isEmpty() ? QString() : m_caption->text();
    emit sent(caption, batch);
    accept(); // closes and deletes the window (WA_DeleteOnClose)
}

void ComposeDialog::reject()
{
    if (m_discard) {
        m_discard->raise();
        m_discard->activateWindow();
        return;
    }
    if (!isDirty()) {
        QDialog::reject();
        return;
    }
    // Not exec(): no nested event loop (plugin shutdown may delete this window at any time).
    auto* box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setLayoutDirection(Qt::LeftToRight);
    box->setWindowTitle(i18n::t("Send to chat"));
    box->setTextFormat(Qt::PlainText);
    box->setStyleSheet(QString::fromLatin1("QLabel{min-width:260px;}")); // one line for the explanation
    box->setText(i18n::t("Discard this message?"));
    box->setInformativeText(i18n::t("Your caption and other changes will be lost."));
    QPushButton* discard = box->addButton(i18n::t("Discard"), QMessageBox::DestructiveRole);
    QPushButton* keep    = box->addButton(i18n::t("Keep editing"), QMessageBox::RejectRole);
    box->setDefaultButton(keep);
    box->setEscapeButton(keep);
    connect(box, &QMessageBox::finished, this, [this, box, discard] {
        if (box->clickedButton() == discard)
            QDialog::reject();
    });
    m_discard = box;
    box->open();
}

// ---- input --------------------------------------------------------------------------------------

void ComposeDialog::addFromPicker()
{
    if (m_picking)
        return; // 2.3 compose-v2: one picker at a time
    QString dir;
    for (const Item& item : qAsConst(m_items)) {
        if (!item.isPasted()) {
            dir = QFileInfo(item.path).absolutePath();
            break;
        }
    }
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString title  = i18n::t("Add files to send");
    const QString filter = i18n::t("All files (*.*);;Images (*.png *.jpg *.jpeg *.jfif *.gif *.webp *.bmp);;Videos (*.mp4 *.webm *.mkv *.mov *.avi *.wmv *.m4v *.3gp *.mts *.m2ts *.mpg)");
    const QPointer<ComposeDialog> guard(this);
    m_picking = true;
    // 2.3 compose-v2: a host may answer it (tests, render tools); otherwise Windows' file dialog.
    const QStringList files = m_host.pickFiles ? m_host.pickFiles(this, title, dir, filter) : QFileDialog::getOpenFileNames(this, title, dir, filter);
    if (!guard)
        return;
    m_picking = false;
    if (files.isEmpty())
        return;
    m_changed = true;
    addFiles(files);
}

bool ComposeDialog::acceptsMime(const QMimeData* mime) const
{
    if (!mime || filedrag::isOwn(mime)) // dragged out of a chat: never sent again by accident
        return false;
    for (const QString& format : mime->formats()) {
        if (format.contains(QLatin1String("ts3"), Qt::CaseInsensitive))
            return false; // TeamSpeak's own file browser
    }
    if (mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        if (urls.isEmpty())
            return false;
        return std::all_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile(); });
    }
    return mime->hasImage();
}

bool ComposeDialog::addMime(const QMimeData* mime)
{
    if (!acceptsMime(mime))
        return false;
    if (mime->hasUrls()) {
        QStringList paths;
        for (const QUrl& url : mime->urls())
            paths << url.toLocalFile();
        addFiles(paths);
    } else {
        addImage(qvariant_cast<QImage>(mime->imageData()));
    }
    m_changed = true;
    return true;
}

// Files, or a picture without text (a picture that comes with text is pasted as text, as in the chat).
bool ComposeDialog::clipboardHasMedia() const
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !acceptsMime(mime))
        return false;
    return mime->hasUrls() || !mime->hasText() || mime->text().trimmed().isEmpty();
}

void ComposeDialog::pasteFromClipboard()
{
    addMime(QGuiApplication::clipboard()->mimeData());
}

void ComposeDialog::keyPressEvent(QKeyEvent* event)
{
    const bool    ctrl = (event->modifiers() & Qt::ControlModifier) && !(event->modifiers() & (Qt::AltModifier | Qt::ShiftModifier));
    const quint32 vk   = event->nativeVirtualKey();
    if (ctrl && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        if (m_send->isEnabled())
            send();
        return;
    }
    if (ctrl && (event->key() == Qt::Key_O || vk == kVkO)) {
        addFromPicker();
        return;
    }
    if ((event->matches(QKeySequence::Paste) || (ctrl && vk == kVkV)) && clipboardHasMedia()) {
        pasteFromClipboard();
        return;
    }
    QDialog::keyPressEvent(event); // Esc: reject(); Enter: the default button
}

bool ComposeDialog::eventFilter(QObject* watched, QEvent* event)
{
    // 2.3: the presence line grew or shrank ("Who?" opened its list, people came or went). fitHeight() fixes
    // the window's height, so without this its rows would overlap: the window follows, once per turn.
    if (watched == m_presence && event->type() == QEvent::LayoutRequest && m_shown && !m_fitQueued) {
        m_fitQueued = true;
        QMetaObject::invokeMethod(this, [this] {
            m_fitQueued = false;
            fitHeight();
        }, Qt::QueuedConnection);
    }
    // Ctrl+V in the caption adds files or a picture; text is pasted as text.
    if (watched == m_caption && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto*      ke    = static_cast<QKeyEvent*>(event);
        const bool ctrl  = (ke->modifiers() & Qt::ControlModifier) && !(ke->modifiers() & (Qt::AltModifier | Qt::ShiftModifier));
        const bool paste = ke->matches(QKeySequence::Paste) || (ctrl && ke->nativeVirtualKey() == kVkV);
        if (paste && clipboardHasMedia()) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept(); // a key press follows
                return true;
            }
            pasteFromClipboard();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ComposeDialog::showDropTarget(bool shown)
{
    if (shown) {
        m_dropHint->setGeometry(rect().adjusted(10, 10, -10, -10));
        m_dropHint->raise();
        m_dropHint->show();
    } else {
        m_dropHint->hide();
    }
}

void ComposeDialog::dragEnterEvent(QDragEnterEvent* event)
{
    if (!acceptsMime(event->mimeData()) || m_items.size() >= compose::kMaxItems) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    showDropTarget(true);
}

void ComposeDialog::dragMoveEvent(QDragMoveEvent* event)
{
    if (!acceptsMime(event->mimeData())) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void ComposeDialog::dragLeaveEvent(QDragLeaveEvent* event)
{
    showDropTarget(false);
    QDialog::dragLeaveEvent(event);
}

void ComposeDialog::dropEvent(QDropEvent* event)
{
    showDropTarget(false);
    if (!addMime(event->mimeData())) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    activateWindow();
}

// ---- look -----------------------------------------------------------------------------------------

void ComposeDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!m_shown) {
        m_shown = true;
        composeui::applyWindowCaption(this, composeui::themeFor(palette())); // 2.3 compose-v2
        fitHeight();
        // 2.3: once more when the window is up (its parts' final sizes, the presence line's two lines, come
        // with the first layout): a small screen's preview isn't left lower than it needs to be.
        if (!m_fitQueued) {
            m_fitQueued = true;
            QMetaObject::invokeMethod(this, [this] {
                m_fitQueued = false;
                fitHeight();
            }, Qt::QueuedConnection);
        }
        (m_items.isEmpty() ? static_cast<QWidget*>(m_addFiles) : static_cast<QWidget*>(m_caption))->setFocus(Qt::OtherFocusReason);
        // QDialog gives the first focus as if by Tab, which selects a prefilled caption: typing would
        // replace it. The cursor goes after it instead (once the window is up).
        QMetaObject::invokeMethod(this, [this] {
            if (m_caption->hasSelectedText())
                m_caption->deselect();
            m_caption->end(false);
        }, Qt::QueuedConnection);
    }
}

void ComposeDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (m_dropHint && m_dropHint->isVisible())
        m_dropHint->setGeometry(rect().adjusted(10, 10, -10, -10));
}

void ComposeDialog::paintEvent(QPaintEvent*)
{
    // 2.3 compose-v2: the window's colour, and the footer band under a hairline.
    QPainter    p(this);
    const Theme theme = composeui::themeFor(palette());
    p.fillRect(rect(), theme.body);
    if (m_footer) {
        const int top = m_footer->geometry().top();
        p.fillRect(QRect(0, top, width(), height() - top), theme.footer);
        p.fillRect(QRect(0, top, width(), 1), theme.border);
    }
}

void ComposeDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        applyTheme();
}

void ComposeDialog::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    ensurePolished();
    const Theme theme = composeui::themeFor(palette());
    m_dark            = theme.dark;

    for (QLabel* label : findChildren<QLabel*>()) {
        const QString role = label->property("role").toString();
        if (!role.isEmpty())
            setRole(label, role == QLatin1String("error") ? "error" : "hint", theme);
    }

    // 2.3 compose-v2: our containers and labels show the window's colour through (TeamSpeak's skins
    // paint every widget), the role colours beat the skin's label colour, and the caption field gets its
    // rounded look and focus ring. fromLatin1, never QStringLiteral: TeamSpeak's style keeps the parsed
    // text after the plugin is unloaded.
    const QColor selection = composeui::mixColors(theme.field, theme.accent, 0.45);
    QString       sheet    = QString::fromLatin1("#tsmediaCompose *[tsmFlat=\"true\"]{background:transparent;border:none;}"
                                                 "#tsmediaCompose QLabel{background:transparent;}"
                                                 "#tsmediaCompose QLabel[role=\"hint\"]{color:%1;}"
                                                 "#tsmediaCompose QLabel[role=\"error\"]{color:%2;}"
                                                 "#tsmediaCompose QLineEdit#tsmComposeCaption{background:%3;color:%4;border:1px solid %5;border-radius:10px;"
                                                 "padding:8px 8px 8px 12px;selection-background-color:%6;selection-color:%4;}"
                                                 "#tsmediaCompose QLineEdit#tsmComposeCaption:hover{border-color:%7;}"
                                                 "#tsmediaCompose QLineEdit#tsmComposeCaption:focus{border:2px solid %8;padding:7px 7px 7px 11px;}")
                              .arg(theme.muted.name(), theme.error.name(), theme.field.name(), theme.text.name(), theme.fieldBorder.name(), selection.name(),
                                   composeui::mixColors(theme.fieldBorder, theme.text, 0.25).name(), theme.accent.name());
    // 2.3 compose-v2 review: "Discard this message?" and "Revert to the original picture?" keep 2.2's
    // focus ring in dark skins, which paint both buttons alike and draw none (Tab, then Enter, would
    // otherwise discard unseen). The same size: the skin's 8 / 14 px padding less the border.
    if (theme.dark && !qApp->styleSheet().isEmpty())
        sheet += QString::fromLatin1("#tsmediaCompose QMessageBox QPushButton:focus{border:2px solid #ffffff;padding:6px 12px;}");
    if (sheet != styleSheet())
        setStyleSheet(sheet);
    // 2.3: the presence line's "Who?" is one of the window's chips, never a link in the skin's blue. Its own
    // style sheet (a widget's own sheet beats the window's); a click doesn't take the focus, so its ring means
    // the keyboard. fromLatin1 for the same reason as above.
    for (QToolButton* who : findChildren<QToolButton*>()) {
        const QWidget* line = who->parentWidget();
        if (!line || line->objectName() != QLatin1String("tsmediaPresenceLine"))
            continue;
        who->setFocusPolicy(Qt::TabFocus);
        who->setAutoRaise(false);
        const QString whoSheet = QString::fromLatin1("QToolButton{background:%1;color:%2;border:none;border-radius:10px;padding:2px 10px;font-weight:600;"
                                                     "text-decoration:none;}"
                                                     "QToolButton:hover{background:%3;}"
                                                     "QToolButton:pressed{background:%4;}"
                                                     "QToolButton:checked{background:%5;}"
                                                     "QToolButton:focus{border:2px solid %6;padding:0px 8px;}")
                                     .arg(theme.chip.name(), theme.text.name(), theme.chipHover.name(), theme.chipPressed.name(),
                                          composeui::mixColors(theme.body, theme.accent, 0.22).name(), theme.accent.name());
        if (who->styleSheet() != whoSheet)
            who->setStyleSheet(whoSheet);
    }
    QPalette captionPalette = m_caption->palette();
    captionPalette.setColor(QPalette::PlaceholderText, theme.muted);
    m_caption->setPalette(captionPalette);

    m_badge->setTheme(theme);
    m_banner->setTheme(theme);
    m_warning->setTheme(theme);
    m_dropHint->setTheme(theme);
    for (QWidget* widget : findChildren<QWidget*>()) {
        if (auto* button = dynamic_cast<composeui::Button*>(widget))
            button->setTheme(theme);
        else if (auto* chip = dynamic_cast<composeui::Chip*>(widget))
            chip->setTheme(theme);
        else if (auto* row = dynamic_cast<composeui::SwitchRow*>(widget))
            row->setTheme(theme);
        else if (auto* zone = dynamic_cast<EmptyZone*>(widget))
            zone->setTheme(theme);
    }
    for (Tile* tile : qAsConst(m_tiles)) {
        const int index = indexOf(tile->id());
        if (index >= 0)
            tile->setItem(m_items.at(index), m_limitMB, theme, editBlocker(tile->id()));
    }
    if (m_singleTile && indexOf(m_singleId) >= 0)
        m_singleTile->setItem(m_items.at(indexOf(m_singleId)), m_limitMB, theme, editBlocker(m_singleId));
    if (m_shown)
        composeui::applyWindowCaption(this, theme);
    update();
    m_applyingTheme = false;
}

void ComposeDialog::announce(QWidget* widget)
{
    if (!widget || !QAccessible::isActive())
        return;
    QAccessibleEvent event(widget, QAccessible::NameChanged); // polite: the focus stays where it is
    QAccessible::updateAccessibility(&event);
}

// ============================================================================================
// 2.2 editor: crop & annotate a picture before it is sent
// ============================================================================================

namespace {

QString randomFolderName()
{
    return QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
}

QString saveFailedText()
{
    return i18n::t("Couldn't save the edited image. Check free disk space and try again.");
}

} // namespace

ImageEditor* ComposeDialog::editor() const
{
    return m_editor.data();
}

void ComposeDialog::editItem(int index)
{
    if (index >= 0 && index < m_items.size())
        openEditor(m_items.at(index).id);
}

bool ComposeDialog::isEdited(int index) const
{
    return index >= 0 && index < m_items.size() && m_items.at(index).edited;
}

QString ComposeDialog::editBlocker(int id) const
{
    const int index = indexOf(id);
    if (index < 0)
        return imageedit::openProblemText(imageedit::OpenProblem::Unreadable);
    const Item& item   = m_items.at(index);
    const auto  record = m_edits.constFind(id);
    const Item& source = record != m_edits.constEnd() ? record->original : item; // what the editor opens
    if (!item.canSend())
        return compose::problemText(item.problem, m_limitMB);
    if (!source.isPasted() && source.kind == MediaKind::AnimatedImage)
        return imageedit::openProblemText(imageedit::OpenProblem::Animated);
    if (source.pixels.isValid() && static_cast<qint64>(source.pixels.width()) * source.pixels.height() > imageedit::kMaxPixels)
        return imageedit::openProblemText(imageedit::OpenProblem::TooLarge);
    return {};
}

void ComposeDialog::openEditor(int id)
{
    if (m_editor) {
        m_editor->raise();
        m_editor->activateWindow();
        return;
    }
    const int index = indexOf(id);
    if (index < 0 || !editingOffered() || !editBlocker(id).isEmpty())
        return;
    auto record = m_edits.find(id);
    if (record == m_edits.end()) {
        EditRecord fresh;
        fresh.original      = m_items.at(index);
        fresh.originalThumb = m_thumbs.value(id);
        record              = m_edits.insert(id, fresh);
    }
    const Item original = record.value().original;

    auto* editor = new ImageEditor(compose::displayName(original), m_host.editorPrefs, this);
    m_editor     = editor;
    m_editingId  = id;
    connect(editor, &ImageEditor::doneRequested, this, [this] { saveEdit(); });
    connect(editor, &QDialog::finished, this, [this, editor, id] {
        // The colour, size and hiding mode chosen last are where the next edit starts.
        m_host.editorPrefs = editor->prefs();
        if (m_host.rememberEditorPrefs)
            m_host.rememberEditorPrefs(m_host.editorPrefs);
        if (m_editor == editor)
            m_editingId = -1;
        // A first edit that was cancelled leaves nothing behind.
        const auto record = m_edits.constFind(id);
        const int  index  = indexOf(id);
        if (record != m_edits.constEnd() && (index < 0 || !m_items.at(index).edited) && record->folder.isEmpty())
            m_edits.remove(id);
        QMetaObject::invokeMethod(this, [this, id] { focusEdit(id); }, Qt::QueuedConnection);
    });
    editor->show();

    // The picture is read on the window's workers (a large photo takes a moment); the editor says
    // "Opening the picture…" meanwhile.
    const int      generation = ++m_editGeneration;
    const QString  path       = original.path;
    const QImage   pasted     = original.image;
    const auto     closing    = m_closing;
    ComposeDialog* self       = this; // waits for this pool in its destructor
    ++m_editJobs;
    m_pool.start([self, id, generation, path, pasted, closing] {
        imageedit::OpenProblem problem = imageedit::OpenProblem::None;
        QImage                 picture;
        if (!pasted.isNull()) {
            if (static_cast<qint64>(pasted.width()) * pasted.height() > imageedit::kMaxPixels)
                problem = imageedit::OpenProblem::TooLarge;
            else
                picture = pasted;
        } else {
            picture = imageedit::loadForEditing(path, &problem);
        }
        if (closing->load())
            return;
        const int why = static_cast<int>(problem);
        QMetaObject::invokeMethod(self, [self, id, generation, picture, why] { self->onEditOpened(id, generation, picture, why); }, Qt::QueuedConnection);
    });
}

void ComposeDialog::onEditOpened(int id, int generation, const QImage& picture, int problem)
{
    m_editJobs = qMax(0, m_editJobs - 1);
    if (!m_editor || id != m_editingId || generation != m_editGeneration || indexOf(id) < 0)
        return;
    const auto why = static_cast<imageedit::OpenProblem>(problem);
    if (why != imageedit::OpenProblem::None || picture.isNull()) {
        m_editor->setOpenError(imageedit::openProblemText(why == imageedit::OpenProblem::None ? imageedit::OpenProblem::Unreadable : why));
        return;
    }
    imageedit::ImageEditModel model = m_edits.value(id).model; // the earlier edits, if any (with their undo steps)
    model.setBase(picture);
    m_editor->setModel(model);
}

void ComposeDialog::saveEdit()
{
    if (!m_editor || m_editingId < 0)
        return;
    const int id     = m_editingId;
    const int index  = indexOf(id);
    auto      record = m_edits.find(id);
    if (index < 0 || record == m_edits.end()) {
        m_editor->accept();
        return;
    }
    const imageedit::ImageEditModel model = m_editor->model();
    if (!model.isModified()) {
        // Back to the picture as it was: nothing to write.
        if (m_items.at(index).edited)
            revertEdit(id);
        else
            m_edits.erase(record);
        m_editor->accept();
        return;
    }
    if (m_items.at(index).edited && model.doc() == record.value().model.doc()) {
        m_editor->accept(); // unchanged since the last Done
        return;
    }

    m_editor->setSaving(true);
    const int      generation = ++m_editGeneration;
    const bool     pasted     = record.value().original.isPasted();
    const QString  sourceName = record.value().original.fileName;
    const QString  folder     = m_host.editDirectory + QLatin1Char('/') + randomFolderName();
    const bool     convert    = m_host.convertLargePngToJpeg;
    const qreal    dpr        = devicePixelRatioF();
    const QSize    maxBox     = QSize(kPreviewWidth, kPreviewHeight) * dpr;
    const int      minSide    = qRound(kTileMax * dpr);
    const auto     closing    = m_closing;
    ComposeDialog* self       = this; // waits for this pool in its destructor
    ++m_editJobs;
    m_pool.start([self, id, generation, model, pasted, sourceName, folder, convert, maxBox, minSide, closing] {
        const QImage out = model.exportImage(); // full resolution, fresh pixels: no EXIF, no location
        QString      path;
        QString      error;
        QImage       thumb;
        if (out.isNull()) {
            error = saveFailedText();
        } else {
            const QSize size = decodeSize(out.size(), maxBox, minSide);
            thumb            = size == out.size() ? out : out.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (!pasted) {
                const imageedit::Encoded encoded = imageedit::encode(out, sourceName, false, convert);
                path = encoded.data.isEmpty() ? QString() : imageedit::writeEdited(encoded, folder, sourceName);
                if (path.isEmpty()) {
                    QDir(folder).removeRecursively();
                    error = saveFailedText();
                }
            }
        }
        if (closing->load()) {
            QDir(folder).removeRecursively();
            return;
        }
        // A pasted picture stays in memory; it is written (PNG, or JPEG when large) when it is sent.
        const QImage picture = pasted && error.isEmpty() ? out : QImage();
        QMetaObject::invokeMethod(self, [self, id, generation, model, folder, path, picture, thumb, error] {
            self->onEditSaved(id, generation, model, folder, path, picture, thumb, error);
        }, Qt::QueuedConnection);
    });
}

void ComposeDialog::onEditSaved(int id, int generation, const imageedit::ImageEditModel& model, const QString& folder, const QString& path,
                                const QImage& picture, const QImage& thumb, const QString& error)
{
    m_editJobs       = qMax(0, m_editJobs - 1);
    const int index  = indexOf(id);
    auto      record = m_edits.find(id);
    if (index < 0 || record == m_edits.end() || generation != m_editGeneration) {
        if (!path.isEmpty())
            QDir(folder).removeRecursively();
        return;
    }
    if (!error.isEmpty()) {
        if (m_editor)
            m_editor->setSaveError(error);
        return;
    }
    EditRecord& edit = record.value();
    if (!edit.folder.isEmpty())
        QDir(edit.folder).removeRecursively(); // the earlier edit's copy
    edit.model = model;
    edit.model.dropBase(); // the pixels are read again for the next edit
    Item& item  = m_items[index];
    item.edited = true;
    item.pixels = model.outputSize();
    if (item.isPasted()) {
        edit.folder.clear();
        item.image = picture;
    } else {
        edit.folder = folder;
        const QFileInfo fi(path);
        item.path     = fi.absoluteFilePath();
        item.fileName = fi.fileName();
        item.kind     = kindForFileName(item.fileName);
        item.size     = fi.size();
        item.problem  = compose::checkFile(fi.exists(), fi.isFile(), item.size, static_cast<qint64>(qMax(0, m_limitMB)) * 1024 * 1024);
    }
    item.probed = true;
    if (!thumb.isNull())
        m_thumbs.insert(id, thumb);
    m_changed = true;
    m_sendError.clear();
    if (m_editor) {
        m_editor->setSaving(false);
        m_editor->accept();
    }
    rebuildItems();
}

void ComposeDialog::askRevert(int id)
{
    if (m_revertBox) {
        m_revertBox->raise();
        m_revertBox->activateWindow();
        return;
    }
    // Not exec(): no nested event loop (plugin shutdown may delete this window at any time).
    auto* box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setLayoutDirection(Qt::LeftToRight);
    box->setWindowTitle(i18n::t("Send to chat"));
    box->setTextFormat(Qt::PlainText);
    box->setStyleSheet(QString::fromLatin1("QLabel{min-width:260px;}"));
    box->setText(i18n::t("Revert to the original picture?"));
    box->setInformativeText(i18n::t("Your edits to this picture will be lost."));
    QPushButton* revert = box->addButton(i18n::t("Revert"), QMessageBox::DestructiveRole);
    QPushButton* keep   = box->addButton(i18n::t("Keep edits"), QMessageBox::RejectRole);
    box->setDefaultButton(keep);
    box->setEscapeButton(keep);
    connect(box, &QMessageBox::finished, this, [this, box, revert, id] {
        if (box->clickedButton() == revert)
            revertEdit(id);
        else
            focusEdit(id);
    });
    m_revertBox = box;
    box->open();
}

void ComposeDialog::revertEdit(int id)
{
    const auto record = m_edits.find(id);
    const int  index  = indexOf(id);
    if (record == m_edits.end() || index < 0)
        return;
    Item&      item    = m_items[index];
    const bool spoiler = item.spoiler;
    item               = record.value().original;
    item.spoiler       = spoiler;
    item.edited        = false;
    if (!record.value().originalThumb.isNull())
        m_thumbs.insert(id, record.value().originalThumb);
    else
        m_thumbs.remove(id);
    if (!record.value().folder.isEmpty())
        QDir(record.value().folder).removeRecursively();
    m_edits.erase(record);
    m_changed = true;
    m_sendError.clear();
    if (!item.isPasted()) {
        // The original may have changed meanwhile.
        const QFileInfo fi(item.path);
        item.size    = fi.size();
        item.problem = compose::checkFile(fi.exists(), fi.isFile(), item.size, static_cast<qint64>(qMax(0, m_limitMB)) * 1024 * 1024);
    }
    if (!m_thumbs.contains(id) && item.problem == Problem::None)
        startProbe(item);
    rebuildItems();
    focusEdit(id);
}

void ComposeDialog::focusEdit(int id)
{
    if (Tile* tile = m_tiles.value(id)) {
        if (tile->edit && tile->edit->isEnabled()) {
            tile->edit->setFocus(Qt::OtherFocusReason);
            if (m_scroll)
                m_scroll->ensureWidgetVisible(tile);
        }
    } else if (id == m_singleId && m_singleEdit && m_singleEdit->isEnabled()) {
        m_singleEdit->setFocus(Qt::OtherFocusReason);
    }
}

QString ComposeDialog::handOver(const Item& item, QStringList* folders) const
{
    const QString folder = m_host.editDirectory + QLatin1Char('/') + randomFolderName();
    if (!QDir().mkpath(folder))
        return {};
    const QString target = folder + QLatin1Char('/') + QFileInfo(item.path).fileName();
    // A hard link costs nothing (same volume); a copy is the fallback (FAT drives).
    const bool linked = CreateHardLinkW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
                                        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(item.path).utf16()), nullptr)
                        != 0;
    if (!linked && !QFile::copy(item.path, target)) {
        QDir(folder).removeRecursively();
        return {};
    }
    folders->append(folder);
    return target;
}
