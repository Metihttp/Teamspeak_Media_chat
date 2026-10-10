#include "composedialog.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QImageIOHandler>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "composehooks.h"
#include "filedrag.h" // 2.2 drag-out: the own-drag marker
#include "i18n.h"
#include "spoiler.h" // 2.2 spoiler: the receivers' cover on spoiler thumbnails
#include "uiutil.h"
#include "video/mfvideo.h"

using compose::Item;
using compose::Problem;

namespace {

constexpr int kMargin        = 16;
constexpr int kSpacing       = 12;
constexpr int kWidth         = 520; // logical px; kNarrowWidth on screens under 700 px
constexpr int kNarrowWidth   = 440;
constexpr int kRowHeight     = 76;
constexpr int kRowGap        = 4;
constexpr int kVisibleRows   = 5; // the list scrolls beyond
constexpr int kThumbSide     = 64;
constexpr int kButtonSide    = 28;
constexpr int kPreviewWidth  = 472; // the large preview of a single picture or video
constexpr int kPreviewHeight = 280;
constexpr int kMinPreview    = 64;
constexpr int kTargetCheckMs = 1000; // connected? partner still there?

// Virtual keys for shortcuts on non-Latin keyboard layouts (as in the viewer).
constexpr quint32 kVkO = 0x4F;
constexpr quint32 kVkS = 0x53;
constexpr quint32 kVkV = 0x56;

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

// The colours of the window, from TeamSpeak's palette (light or dark skin) and the 2.1 tokens.
struct Colors {
    bool   dark = false;
    QColor window;
    QColor text;
    QColor muted;  // secondary text, at least 4.5:1
    QColor accent; // checked spoiler, focus rings, the drop frame (3:1 or more)
    QColor error;  // problems, at least 4.5:1
    QColor frame;  // thumbnail frames
    QColor placeholder;
};

Colors colorsFor(const QPalette& pal)
{
    Colors c;
    c.window = pal.color(QPalette::Active, QPalette::Window);
    c.text   = pal.color(QPalette::Active, QPalette::WindowText);
    c.dark   = c.window.lightness() < 128;
    c.muted  = ui::flatten(withAlpha(c.text, 175.0 / 255.0), c.window);
    if (ui::contrastRatio(c.muted, c.window) < 4.5)
        c.muted = c.text;
    c.accent = c.dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4);
    if (ui::contrastRatio(c.accent, c.window) < 3.0)
        c.accent = c.text;
    c.error = c.dark ? QColor(0xfa, 0x77, 0x7c) : QColor(0xc4, 0x28, 0x2d);
    if (ui::contrastRatio(c.error, c.window) < 4.5)
        c.error = c.text; // the alert glyph still marks it
    c.frame       = ui::flatten(withAlpha(c.text, 0.22), c.window);
    c.placeholder = ui::flatten(withAlpha(c.text, 0.08), c.window);
    return c;
}

QFont scaledFont(const QFont& base, qreal factor)
{
    QFont font(base);
    if (base.pixelSize() > 0)
        font.setPixelSize(qMax(1, qRound(base.pixelSize() * factor)));
    else
        font.setPointSizeF(qMax(1.0, (base.pointSizeF() > 0 ? base.pointSizeF() : 9.0) * factor));
    return font;
}

// ---- glyphs: vector paths in the stroke style of the other plugin glyphs -------------------------

// An eye with a slash: hidden until clicked.
void drawSpoilerGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal   s = box.width();
    const QPointF c = box.center();
    p.setPen(QPen(color, qMax(1.4, s * 0.09), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath eye;
    eye.moveTo(c.x() - s * 0.44, c.y());
    eye.quadTo(c.x(), c.y() - s * 0.46, c.x() + s * 0.44, c.y());
    eye.quadTo(c.x(), c.y() + s * 0.46, c.x() - s * 0.44, c.y());
    p.drawPath(eye);
    p.drawEllipse(c, s * 0.12, s * 0.12);
    p.drawLine(QPointF(box.left() + s * 0.12, box.top() + s * 0.12), QPointF(box.right() - s * 0.12, box.bottom() - s * 0.12));
}

void drawCrossGlyph(QPainter& p, const QRectF& box, const QColor& color)
{
    const qreal   h = box.width() * 0.3;
    const QPointF c = box.center();
    p.setPen(QPen(color, qMax(1.5, box.width() * 0.1), Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c + QPointF(-h, -h), c + QPointF(h, h));
    p.drawLine(c + QPointF(h, -h), c + QPointF(-h, h));
}

// A filled disc with "!" (never colour alone: the mark says it).
void drawAlertGlyph(QPainter& p, const QRectF& box, const QColor& fill, const QColor& mark)
{
    const qreal s  = box.width();
    const qreal cx = box.center().x();
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(box.adjusted(s * 0.04, s * 0.04, -s * 0.04, -s * 0.04));
    p.setPen(QPen(mark, s * 0.12, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx, box.top() + s * 0.26), QPointF(cx, box.top() + s * 0.56));
    p.setPen(Qt::NoPen);
    p.setBrush(mark);
    p.drawEllipse(QPointF(cx, box.top() + s * 0.74), s * 0.07, s * 0.07);
}

QPixmap alertPixmap(int size, qreal dpr, const QColor& fill, const QColor& mark)
{
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    drawAlertGlyph(p, QRectF(0, 0, size, size), fill, mark);
    return pixmap;
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
void setRole(QLabel* label, const char* role, const Colors& colors)
{
    const QString value = QString::fromLatin1(role);
    if (label->property("role").toString() != value) {
        label->setProperty("role", value);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    const QColor color = value == QLatin1String("error") ? colors.error : colors.muted;
    QPalette     pal   = label->palette();
    pal.setColor(QPalette::Active, QPalette::WindowText, color);
    pal.setColor(QPalette::Inactive, QPalette::WindowText, color);
    label->setPalette(pal);
}

// The size a worker decodes a picture at: fits the large preview, but covers a list thumbnail; never
// larger than the picture itself.
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
};

ProbeData probeFile(const QString& path, MediaKind kind, const QSize& maxBox, int minSide, const std::atomic_bool& closing)
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
            return data; // the type icon stays
        const QSize target = decodeSize(data.pixels, maxBox, minSide);
        reader.setScaledSize(rotated ? target.transposed() : target); // applied before the rotation
        data.thumb = reader.read();
    } else if (kind == MediaKind::Video || kind == MediaKind::Audio) {
        const mf::ProbeResult probe = mf::probe(path, qMax(maxBox.width(), maxBox.height()));
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

QString samePathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower(); // Windows paths ignore case
}

} // namespace

// ============================================================================================
// Parts of the window
// ============================================================================================

// A 28 x 28 px button with a vector glyph: Spoiler (checkable) or Remove. Tab focus only, so a click
// doesn't move the focus away from the caption; the focus ring shows after keyboard focus.
class ComposeDialog::GlyphButton : public QAbstractButton
{
  public:
    enum class Glyph { Spoiler, Remove };

    GlyphButton(Glyph glyph, QWidget* parent)
        : QAbstractButton(parent)
        , m_glyph(glyph)
    {
        setFixedSize(kButtonSide, kButtonSide);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setCheckable(glyph == Glyph::Spoiler);
    }

    void setColors(const Colors& colors)
    {
        m_colors = colors;
        update();
    }

    QSize sizeHint() const override { return QSize(kButtonSide, kButtonSide); }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        QColor       background(Qt::transparent);
        if (isChecked())
            background = withAlpha(m_colors.accent, isDown() ? 0.32 : 0.22);
        else if (isDown())
            background = withAlpha(m_colors.text, 0.18);
        else if (underMouse())
            background = withAlpha(m_colors.text, 0.10);
        if (background.alpha() > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(background);
            p.drawRoundedRect(r, 6, 6);
        }
        const QColor glyph = !isEnabled() ? withAlpha(m_colors.muted, 0.35)
                             : isChecked() ? m_colors.accent
                             : (underMouse() || hasFocus()) ? m_colors.text
                                                            : m_colors.muted;
        QRectF       box(0, 0, 16, 16);
        box.moveCenter(QRectF(rect()).center());
        if (m_glyph == Glyph::Spoiler)
            drawSpoilerGlyph(p, box, glyph);
        else
            drawCrossGlyph(p, box, glyph);
        if (hasFocus() && m_keyboardFocus) {
            p.setPen(QPen(m_colors.accent, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 6, 6);
        }
    }

    void focusInEvent(QFocusEvent* event) override
    {
        m_keyboardFocus = event->reason() != Qt::MouseFocusReason && event->reason() != Qt::PopupFocusReason;
        QAbstractButton::focusInEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers() == Qt::NoModifier) {
            click(); // like a focused push button: Enter acts on it, it doesn't send
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }

  private:
    Glyph  m_glyph;
    Colors m_colors;
    bool   m_keyboardFocus = false;
};

// A thumbnail: the picture (rounded, framed), or the file's type icon until there is one; for spoilers
// the receivers' cover (spoiler::drawCover: the same blur and SPOILER pill, or the eye-off mark where
// the pill doesn't fit); faded for items that can't be sent. Cover: the picture fills the box (list
// rows); otherwise it is fitted and centred (the large preview).
class ComposeDialog::Thumb : public QWidget
{
  public:
    Thumb(const QSize& logical, bool cover, QWidget* parent)
        : QWidget(parent)
        , m_cover(cover)
    {
        setFixedSize(logical);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setImage(const QImage& image)
    {
        m_image = image;
        update();
    }
    void setIcon(const QPixmap& icon)
    {
        m_icon = icon;
        update();
    }
    void setSpoiler(bool spoiler)
    {
        if (spoiler == m_spoiler)
            return;
        m_spoiler = spoiler;
        update();
    }
    void setDimmed(bool dimmed)
    {
        if (dimmed == m_dimmed)
            return;
        m_dimmed = dimmed;
        update();
    }
    void setColors(const Colors& colors)
    {
        m_colors = colors;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(box, 6, 6);
        p.setClipPath(clip);
        p.fillRect(rect(), m_colors.placeholder);

        if (m_spoiler) {
            // 2.2 spoiler: what receivers see (decisions: the same blur as the chat). Nothing sharp of
            // the picture is drawn; without a picture yet, the placeholder and the pill.
            bool               blurHash = false;
            const QImage       source   = spoiler::coverSource(QString(), QSizeF(size()), m_image, false, &blurHash);
            spoiler::CoverLook look;
            look.font = this->font();
            look.font.setPixelSize(m_cover ? 10 : 12);
            look.font.setBold(true);
            look.placeholder = m_colors.placeholder;
            spoiler::drawCover(p, QRectF(rect()), source, blurHash, look);
        } else if (!m_image.isNull()) {
            const QSizeF size = QSizeF(m_image.size()) / m_image.devicePixelRatio();
            QRectF       target(QPointF(0, 0), size);
            target.moveCenter(QRectF(rect()).center());
            p.drawImage(target, m_image);
        } else if (!m_icon.isNull()) {
            const QSizeF size = QSizeF(m_icon.size()) / m_icon.devicePixelRatio();
            QRectF       target(QPointF(0, 0), size);
            target.moveCenter(QRectF(rect()).center());
            p.drawPixmap(target, m_icon, QRectF(m_icon.rect()));
        }
        if (m_dimmed)
            p.fillRect(rect(), withAlpha(m_colors.window, 0.45)); // about 60% strength
        p.setClipping(false);
        p.setPen(QPen(m_colors.frame, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box, 6, 6);
    }

  private:
    bool    m_cover   = true;
    bool    m_spoiler = false;
    bool    m_dimmed  = false;
    QImage  m_image; // device pixels, sized for the box
    QPixmap m_icon;
    Colors  m_colors;
};

// One item in the list (2 or more items): thumbnail, name, details or the problem, Spoiler, Remove.
// Keys on a focused row: Delete removes it, S marks it as a spoiler.
class ComposeDialog::Row : public QFrame
{
  public:
    Row(ComposeDialog* dialog, const Item& item, QWidget* parent)
        : QFrame(parent)
        , m_dialog(dialog)
        , m_id(item.id)
    {
        setFixedHeight(kRowHeight);
        thumb  = new Thumb(QSize(kThumbSide, kThumbSide), true, this);
        m_name = new ElidedLabel(Qt::ElideMiddle, this);
        QFont bold = m_name->font();
        bold.setBold(true);
        m_name->setFont(bold);
        m_meta = new ElidedLabel(Qt::ElideRight, this);
        m_meta->setProperty("role", QString::fromLatin1("hint"));
        m_alert = new QLabel(this);
        m_alert->setFixedSize(14, 14);

        auto* metaRow = new QHBoxLayout;
        metaRow->setSpacing(6);
        metaRow->addWidget(m_alert, 0, Qt::AlignVCenter);
        metaRow->addWidget(m_meta, 1);
        auto* texts = new QVBoxLayout;
        texts->setSpacing(2);
        texts->addStretch(1);
        texts->addWidget(m_name);
        texts->addLayout(metaRow);
        texts->addStretch(1);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(kSpacing);
        layout->addWidget(thumb);
        layout->addLayout(texts, 1);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        if (compose::spoilerAllowed(item.kind)) {
            spoiler = new GlyphButton(GlyphButton::Glyph::Spoiler, this);
            buttons->addWidget(spoiler);
            QObject::connect(spoiler, &QAbstractButton::clicked, m_dialog, [d = m_dialog, id = m_id] { d->toggleSpoiler(id); });
        }
        remove = new GlyphButton(GlyphButton::Glyph::Remove, this);
        buttons->addWidget(remove);
        layout->addLayout(buttons);
        QObject::connect(remove, &QAbstractButton::clicked, m_dialog, [d = m_dialog, id = m_id] { d->removeItem(id); });
    }

    void setItem(const Item& item, int limitMB, const Colors& colors, qreal dpr)
    {
        const QString name = compose::displayName(item);
        m_name->setFullText(name);
        m_name->setToolTip(QString::fromLatin1("<p style='white-space:pre'>%1</p>").arg(name.toHtmlEscaped()));
        QString problem = compose::problemText(item.problem, limitMB);
        if (item.problem == Problem::TooLarge)
            problem += QStringLiteral(" · ") + formatSize(static_cast<quint64>(qMax<qint64>(0, item.size)));
        m_meta->setFullText(problem.isEmpty() ? compose::metaText(item) : problem);
        setRole(m_meta, problem.isEmpty() ? "hint" : "error", colors);
        m_alert->setVisible(!problem.isEmpty());
        if (!problem.isEmpty())
            m_alert->setPixmap(alertPixmap(14, dpr, colors.error, colors.window));
        thumb->setDimmed(!item.canSend());
        thumb->setSpoiler(item.spoiler);
        thumb->setColors(colors);
        if (spoiler) {
            spoiler->setEnabled(item.canSend()); // nothing to mark on what isn't sent
            spoiler->setChecked(item.spoiler);
            spoiler->setAccessibleName(i18n::t("Mark %1 as spoiler").arg(name));
            spoiler->setToolTip(item.spoiler ? i18n::t("Marked as spoiler: shown blurred until clicked (S)") : i18n::t("Mark as spoiler (S)"));
            spoiler->setColors(colors);
        }
        remove->setAccessibleName(i18n::t("Remove %1").arg(name));
        remove->setToolTip(i18n::t("Remove (Delete)"));
        remove->setColors(colors);
        setAccessibleName(compose::accessibleName(item));
        setAccessibleDescription(problem);
    }

    int id() const { return m_id; }

    Thumb*       thumb   = nullptr;
    GlyphButton* spoiler = nullptr;
    GlyphButton* remove  = nullptr;

  protected:
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
        QFrame::keyPressEvent(event);
    }

  private:
    ComposeDialog* m_dialog;
    int            m_id;
    ElidedLabel*   m_name  = nullptr;
    ElidedLabel*   m_meta  = nullptr;
    QLabel*        m_alert = nullptr;
};

// Over the window while files are dragged onto it.
class ComposeDialog::DropHint : public QWidget
{
  public:
    explicit DropHint(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

    void setColors(const Colors& colors)
    {
        m_colors = colors;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(m_colors.window, 0.92));
        p.drawRoundedRect(frame, 8, 8);
        QPen border(m_colors.accent, 2.0, Qt::DashLine);
        border.setDashPattern({4.0, 3.0});
        p.setPen(border);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 8, 8);
        QFont font = scaledFont(this->font(), 1.2);
        font.setBold(true);
        p.setFont(font);
        p.setPen(m_colors.text);
        p.drawText(frame, Qt::AlignCenter, i18n::t("Drop to add them"));
    }

  private:
    Colors m_colors;
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
    setFixedWidth(avail.width() < 700 ? kNarrowWidth : kWidth);
    if (avail.height() < 800)
        m_previewMaxHeight = 200; // the whole window stays within 80% of a small screen

    // ---- header: where it goes --------------------------------------------------------------------
    m_header = new QLabel(this);
    m_header->setTextFormat(Qt::PlainText);
    m_header->setWordWrap(true);
    QFont headerFont = scaledFont(font(), 1.1);
    headerFont.setBold(true);
    m_header->setFont(headerFont);
    m_header->setText(i18n::t("Send to %1").arg(m_host.describeTarget ? m_host.describeTarget(m_target) : QString()));
    auto* header = new QVBoxLayout;
    header->setSpacing(2);
    header->addWidget(m_header);
    if (m_target.mode == TextMessageTarget_SERVER) {
        m_serverNote = roleLabel(i18n::t("Everyone on this server gets this message."), "hint", this);
        header->addWidget(m_serverNote);
    }

    // 2.2 presence: "3 of 5 people here will see it in the chat" (the plugin-protocol feature's widget).
    // In a box of ours, so hiding it for the banner never overrides what the widget does by itself.
    if (QWidget* presence = compose::createPresenceLine(this, m_target)) {
        m_presence        = new QWidget(this);
        auto* presenceBox = new QVBoxLayout(m_presence);
        presenceBox->setContentsMargins(0, 0, 0, 0);
        presence->setParent(m_presence);
        presenceBox->addWidget(presence);
    }

    // Why nothing can be sent right now (replaces the presence line; Send is off).
    m_banner     = new QWidget(this);
    m_bannerIcon = new QLabel(m_banner);
    m_bannerIcon->setFixedSize(16, 16);
    m_bannerText = new QLabel(m_banner);
    m_bannerText->setTextFormat(Qt::PlainText);
    m_bannerText->setWordWrap(true);
    auto* bannerRow = new QHBoxLayout(m_banner);
    bannerRow->setContentsMargins(0, 0, 0, 0);
    bannerRow->setSpacing(8);
    bannerRow->addWidget(m_bannerIcon, 0, Qt::AlignTop);
    bannerRow->addWidget(m_bannerText, 1);
    m_banner->hide();

    // ---- the items (rebuilt as they change) ------------------------------------------------------
    m_itemsHost   = new QWidget(this);
    m_itemsLayout = new QVBoxLayout(m_itemsHost);
    m_itemsLayout->setContentsMargins(0, 0, 0, 0);
    m_itemsLayout->setSpacing(0);

    m_addFiles = new QPushButton(i18n::t("&Add files…"), this);
    m_addFiles->setToolTip(i18n::t("Add files (Ctrl+O). You can also drop files here or paste them."));
    m_album = new QCheckBox(i18n::t("Send as an a&lbum"), this);
    m_album->setToolTip(i18n::t("Shows them together in a grid. Up to %1 per album.").arg(MediaLink::kMaxAlbumItems));
    m_album->setChecked(m_host.albumDefault);
    m_album->hide();
    auto* footer = new QHBoxLayout;
    footer->addWidget(m_addFiles);
    footer->addStretch(1);
    footer->addWidget(m_album);
    m_albumHint = roleLabel(QString(), "hint", this);
    m_albumHint->hide();

    // ---- caption ----------------------------------------------------------------------------------
    auto* captionLabel = new QLabel(i18n::t("&Caption"), this);
    m_caption          = new QLineEdit(this);
    m_caption->setMaxLength(kCaptionMaxChars);
    m_caption->setPlaceholderText(i18n::t("Add a caption (optional)"));
    m_caption->setClearButtonEnabled(false);
    m_caption->setAcceptDrops(false); // files dropped on it are added, not typed in
    m_caption->installEventFilter(this);
    captionLabel->setBuddy(m_caption);
    m_captionHelp = roleLabel(QString(), "hint", this);
    m_counter     = roleLabel(QString(), "hint", this);
    m_counter->setWordWrap(false);
    m_counter->setAlignment(Qt::AlignRight | Qt::AlignTop);
    m_splitHint = roleLabel(i18n::t("This caption is too long to share a message with the file, so it's sent as its own message right above it."), "hint", this);
    m_splitHint->hide();
    auto* captionNotes = new QHBoxLayout;
    captionNotes->setSpacing(kSpacing);
    captionNotes->addWidget(m_captionHelp, 1);
    captionNotes->addWidget(m_counter, 0, Qt::AlignTop);
    auto* caption = new QVBoxLayout;
    caption->setSpacing(4);
    caption->addWidget(captionLabel);
    caption->addWidget(m_caption);
    caption->addLayout(captionNotes);
    caption->addWidget(m_splitHint);

    // ---- what can't be sent ----------------------------------------------------------------------
    m_warning     = new QWidget(this);
    m_warningIcon = new QLabel(m_warning);
    m_warningIcon->setFixedSize(16, 16);
    m_warningText = new QLabel(m_warning);
    m_warningText->setTextFormat(Qt::PlainText);
    m_warningText->setWordWrap(true);
    auto* warningRow = new QHBoxLayout(m_warning);
    warningRow->setContentsMargins(0, 0, 0, 0);
    warningRow->setSpacing(8);
    warningRow->addWidget(m_warningIcon, 0, Qt::AlignTop);
    warningRow->addWidget(m_warningText, 1);
    m_warning->hide();

    // ---- buttons (Windows order: Send, Cancel) ----------------------------------------------------
    auto* buttons = new QDialogButtonBox(this);
    m_send        = buttons->addButton(i18n::t("Send"), QDialogButtonBox::AcceptRole);
    m_cancel      = buttons->addButton(i18n::t("Cancel"), QDialogButtonBox::RejectRole);
    m_send->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { send(); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
    layout->setSpacing(kSpacing);
    layout->addLayout(header);
    // 2.2 reply: the reply these files go out as; dropping it (its x) lays the window out again.
    if (QWidget* reply = m_host.replyLine ? m_host.replyLine(this, m_target) : nullptr) {
        layout->addWidget(reply);
        connect(reply, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, [this] { fitHeight(); }); });
    }
    if (m_presence)
        layout->addWidget(m_presence);
    layout->addWidget(m_banner);
    layout->addWidget(m_itemsHost);
    layout->addLayout(footer);
    layout->addWidget(m_albumHint);
    layout->addLayout(caption);
    layout->addWidget(m_warning);
    layout->addWidget(buttons);

    m_dropHint = new DropHint(this);

    connect(m_caption, &QLineEdit::textChanged, this, [this] { updateCaption(); });
    connect(m_album, &QCheckBox::toggled, this, [this] {
        if (isVisible())
            m_changed = true;
        updateState();
    });

    // TeamSpeak's dark skins switch every focus indicator off: like the settings window, focus rings
    // show once the keyboard has been used to move around here.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (m_keyboardFocus || !now || now->window() != this || !testAttribute(Qt::WA_KeyboardFocusChange))
            return;
        m_keyboardFocus = true;
        applyTheme();
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
}

// ---- items --------------------------------------------------------------------------------------

void ComposeDialog::addFiles(const QStringList& paths)
{
    QSet<QString> known;
    for (const Item& item : qAsConst(m_items)) {
        if (!item.isPasted())
            known.insert(samePathKey(item.path));
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
    const int    minSide = qRound(kThumbSide * dpr);
    const int    id      = item.id;
    const QString path   = item.path;
    const QImage  pasted = item.image;
    const MediaKind kind = item.kind;
    const auto    closing = m_closing;
    ComposeDialog* self   = this; // waits for this pool in its destructor
    m_pool.start([self, id, path, pasted, kind, maxBox, minSide, closing] {
        ProbeData data;
        if (!pasted.isNull()) {
            data.pixels       = pasted.size();
            const QSize target = decodeSize(pasted.size(), maxBox, minSide);
            data.thumb         = target == pasted.size() ? pasted : pasted.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        } else {
            data = probeFile(path, kind, maxBox, minSide, *closing);
        }
        if (closing->load())
            return;
        QMetaObject::invokeMethod(self, [self, id, data] { self->onProbed(id, data.readable, data.pixels, data.durationMs, data.thumb); }, Qt::QueuedConnection);
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

    if (Row* row = m_rows.value(id)) {
        row->setItem(item, m_limitMB, colorsFor(palette()), devicePixelRatioF());
        refreshThumb(id);
    } else if (id == m_singleId) {
        // The large preview gets its final size, the details their numbers; the focus stays put.
        const bool spoilerFocused = m_singleSpoiler && m_singleSpoiler->hasFocus();
        rebuildItems();
        if (spoilerFocused && m_singleSpoiler)
            m_singleSpoiler->setFocus(Qt::OtherFocusReason);
        return;
    }
    updateState();
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
    return m_pending > 0;
}

void ComposeDialog::removeItem(int id)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    m_items.remove(index);
    m_thumbs.remove(id);
    m_changed  = true;
    m_overflow = false;
    m_sendError.clear();
    // Later: this may run inside the row's own key or click handler.
    QMetaObject::invokeMethod(this, [this, index] { rebuildItems(index); }, Qt::QueuedConnection);
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
    if (Row* row = m_rows.value(item.id))
        row->setItem(item, m_limitMB, colorsFor(palette()), devicePixelRatioF());
    if (m_singleThumb && item.id == m_singleId)
        m_singleThumb->setSpoiler(spoiler);
    if (m_singleSpoiler && item.id == m_singleId && m_singleSpoiler->isChecked() != spoiler)
        m_singleSpoiler->setChecked(spoiler);
    updateState();
}

void ComposeDialog::setAlbum(bool album)
{
    m_album->setChecked(album);
}

// ---- views --------------------------------------------------------------------------------------

void ComposeDialog::rebuildItems(int focusIndex)
{
    if (m_itemsView) {
        m_itemsLayout->removeWidget(m_itemsView);
        m_itemsView->hide();
        m_itemsView->deleteLater();
        m_itemsView = nullptr;
    }
    m_rows.clear();
    m_scroll        = nullptr;
    m_singleThumb   = nullptr;
    m_singleSpoiler = nullptr;
    m_singleId      = -1;
    m_spoilerHint   = nullptr;

    QWidget* view = nullptr;
    if (m_items.isEmpty()) {
        view          = new QWidget(m_itemsHost);
        auto* layout  = new QVBoxLayout(view);
        layout->setContentsMargins(0, 24, 0, 24);
        auto* empty = roleLabel(i18n::t("Nothing to send. Add files or drop them here."), "hint", view);
        empty->setAlignment(Qt::AlignCenter);
        layout->addWidget(empty);
    } else if (m_items.size() == 1) {
        view = buildSingle(m_items.first());
    } else {
        view = buildList();
    }
    m_itemsView = view;
    m_itemsLayout->addWidget(view);
    view->show();

    // Tab order follows the window: the items, Add files…, Send as an album, the caption, the buttons.
    QWidget* previous = nullptr;
    const auto chain  = [&previous](QWidget* next) {
        if (!next)
            return;
        if (previous)
            QWidget::setTabOrder(previous, next);
        previous = next;
    };
    for (const Item& item : qAsConst(m_items)) {
        if (Row* row = m_rows.value(item.id)) {
            chain(row->spoiler);
            chain(row->remove);
        }
    }
    chain(m_singleSpoiler);
    chain(m_addFiles);
    chain(m_album);
    chain(m_caption);
    chain(m_send);
    chain(m_cancel);

    applyTheme();
    updateState();
    fitHeight();

    if (focusIndex >= 0) {
        // After Remove: the next row's Remove (or the previous one's), or Add files… when none is left.
        if (!m_items.isEmpty() && !m_rows.isEmpty()) {
            const Item& next = m_items.at(qMin(focusIndex, m_items.size() - 1));
            if (Row* row = m_rows.value(next.id)) {
                row->remove->setFocus(Qt::OtherFocusReason);
                if (m_scroll)
                    m_scroll->ensureWidgetVisible(row);
            }
        } else if (m_items.isEmpty()) {
            m_addFiles->setFocus(Qt::OtherFocusReason);
        } else {
            m_caption->setFocus(Qt::OtherFocusReason);
        }
    }
}

QWidget* ComposeDialog::buildSingle(const Item& itemIn)
{
    Item& item   = m_items.first();
    Q_UNUSED(itemIn);
    auto* view   = new QWidget(m_itemsHost);
    auto* layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    m_singleId = item.id;

    const Colors colors = colorsFor(palette());
    const bool   media  = item.isPasted() || isPreviewableImage(item.kind) || item.kind == MediaKind::Video;
    const int    room   = width() - 2 * kMargin;

    auto* name = new ElidedLabel(Qt::ElideMiddle, view);
    QFont bold = name->font();
    bold.setBold(true);
    name->setFont(bold);
    const QString shownName = compose::displayName(item);
    name->setFullText(shownName);
    name->setToolTip(QString::fromLatin1("<p style='white-space:pre'>%1</p>").arg(shownName.toHtmlEscaped()));
    auto* meta = new ElidedLabel(Qt::ElideRight, view);
    meta->setProperty("role", QString::fromLatin1("hint"));
    meta->setFullText(compose::metaText(item));

    if (media) {
        // The box is known before the picture: nothing moves when it arrives. Pictures keep their shape
        // (read from the header now if the worker hasn't yet); videos get 16:9 with the poster inside.
        if (!item.pixels.isValid() && !item.isPasted() && isPreviewableImage(item.kind)) {
            QImageReader reader(item.path);
            reader.setAutoTransform(true);
            const QSize raw = reader.size();
            if (raw.isValid())
                item.pixels = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90) ? raw.transposed() : raw;
        }
        const QSize maxBox(qMin(kPreviewWidth, room), m_previewMaxHeight);
        QSize       box;
        if (item.kind == MediaKind::Video || !item.pixels.isValid() || item.pixels.isEmpty())
            box = QSize(maxBox.width(), qMin(maxBox.height(), maxBox.width() * 9 / 16));
        else
            box = (item.pixels.width() > maxBox.width() || item.pixels.height() > maxBox.height() ? item.pixels.scaled(maxBox, Qt::KeepAspectRatio) : item.pixels)
                      .expandedTo(QSize(kMinPreview, kMinPreview));
        m_singleThumb = new Thumb(box, false, view);
        m_singleThumb->setColors(colors);
        auto* center = new QHBoxLayout;
        center->addStretch(1);
        center->addWidget(m_singleThumb);
        center->addStretch(1);
        layout->addLayout(center);
        layout->addSpacing(4);
        layout->addWidget(name);
        layout->addWidget(meta);
    } else {
        // A file without a picture: its type icon next to the name.
        m_singleThumb = new Thumb(QSize(48, 48), true, view);
        m_singleThumb->setColors(colors);
        auto* texts = new QVBoxLayout;
        texts->setSpacing(2);
        texts->addStretch(1);
        texts->addWidget(name);
        texts->addWidget(meta);
        texts->addStretch(1);
        auto* row = new QHBoxLayout;
        row->setSpacing(kSpacing);
        row->addWidget(m_singleThumb);
        row->addLayout(texts, 1);
        layout->addLayout(row);
    }
    refreshThumb(item.id);

    if (!item.canSend()) {
        auto* problemRow = new QHBoxLayout;
        problemRow->setSpacing(8);
        auto* icon = new QLabel(view);
        icon->setFixedSize(16, 16);
        icon->setPixmap(alertPixmap(16, devicePixelRatioF(), colors.error, colors.window));
        QString problem = compose::problemText(item.problem, m_limitMB);
        if (item.problem == Problem::TooLarge)
            problem += QStringLiteral(". ") + i18n::t("You can raise the limit in Settings → Sending.");
        else if (item.problem == Problem::Empty)
            problem += QLatin1Char('.');
        auto* text = roleLabel(problem, "error", view);
        problemRow->addWidget(icon, 0, Qt::AlignTop);
        problemRow->addWidget(text, 1);
        layout->addSpacing(2);
        layout->addLayout(problemRow);
        m_singleThumb->setDimmed(true);
        view->setAccessibleDescription(problem);
    }

    if (compose::spoilerAllowed(item.kind) && item.canSend()) {
        m_singleSpoiler = new QCheckBox(i18n::t("Mark as &spoiler"), view);
        m_singleSpoiler->setChecked(item.spoiler);
        m_singleSpoiler->setToolTip(i18n::t("Shown blurred until someone clicks it (S)"));
        m_spoilerHint = roleLabel(i18n::t("People on TS Media 2.1 or older see it unblurred."), "hint", view);
        m_singleSpoiler->setAccessibleDescription(m_spoilerHint->text());
        connect(m_singleSpoiler, &QCheckBox::toggled, this, [this](bool on) { setSpoiler(0, on); });
        layout->addSpacing(4);
        layout->addWidget(m_singleSpoiler);
        layout->addWidget(m_spoilerHint);
    }
    return view;
}

QWidget* ComposeDialog::buildList()
{
    auto* view   = new QWidget(m_itemsHost);
    auto* layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_scroll = new QScrollArea(view);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    auto* list       = new QWidget(m_scroll);
    auto* listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(kRowGap);
    const Colors colors = colorsFor(palette());
    for (const Item& item : qAsConst(m_items)) {
        auto* row = new Row(this, item, list);
        row->setItem(item, m_limitMB, colors, devicePixelRatioF());
        listLayout->addWidget(row);
        m_rows.insert(item.id, row);
    }
    listLayout->addStretch(1);
    m_scroll->setWidget(list);
    list->setAutoFillBackground(false);
    m_scroll->viewport()->setAutoFillBackground(false);
    const int rows = qMin(m_items.size(), m_visibleRows);
    m_scroll->setFixedHeight(rows * kRowHeight + (rows - 1) * kRowGap);
    layout->addWidget(m_scroll);
    for (const Item& item : qAsConst(m_items))
        refreshThumb(item.id);

    m_spoilerHint = roleLabel(i18n::t("People on TS Media 2.1 or older see spoilers unblurred."), "hint", view);
    layout->addWidget(m_spoilerHint);
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

QPixmap ComposeDialog::iconFor(const Item& item, int size) const
{
    QFileIconProvider provider;
    const QIcon       icon = item.isPasted() ? provider.icon(QFileIconProvider::File) : provider.icon(QFileInfo(item.path));
    const qreal       dpr  = devicePixelRatioF();
    QPixmap           pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    icon.paint(&p, QRect(0, 0, size, size));
    return pixmap;
}

void ComposeDialog::refreshThumb(int id)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    const Item& item = m_items.at(index);
    Thumb*      thumb = nullptr;
    bool        cover = true;
    if (Row* row = m_rows.value(id)) {
        thumb = row->thumb;
    } else if (id == m_singleId && m_singleThumb) {
        thumb = m_singleThumb;
        cover = m_singleThumb->width() == 48; // the small type-icon box
    }
    if (!thumb)
        return;
    const QImage image = thumbFor(id, thumb->size(), cover);
    if (!image.isNull())
        thumb->setImage(image);
    else
        thumb->setIcon(iconFor(item, thumb->width() >= 64 && !cover ? 48 : 32));
    thumb->setSpoiler(item.spoiler);
}

// ---- state ----------------------------------------------------------------------------------------

void ComposeDialog::updateState()
{
    const int sendable = compose::sendableCount(m_items);
    m_send->setText(compose::sendButtonText(m_items));
    m_send->setEnabled(sendable > 0 && m_blocker.isEmpty() && !m_sending);

    const bool albumOffered = compose::albumsEnabled() && compose::albumCandidates(m_items) >= 2;
    if (m_album->isHidden() == albumOffered) // isHidden: also right before the window is shown
        m_album->setVisible(albumOffered);
    const QString albumHint = albumOffered && m_album->isChecked() ? compose::albumHint(m_items) : QString();
    m_albumHint->setText(albumHint);
    m_albumHint->setVisible(!albumHint.isEmpty());

    if (m_spoilerHint && m_rows.size() > 1) {
        const bool any = std::any_of(m_items.cbegin(), m_items.cend(), [](const Item& item) { return item.spoiler && item.canSend(); });
        m_spoilerHint->setVisible(any);
    }

    QString warning = m_sendError.isEmpty() ? compose::skippedText(m_items) : m_sendError;
    if (m_overflow)
        warning += (warning.isEmpty() ? QString() : QStringLiteral(" ")) + i18n::t("Only the first %1 files can be sent at once.").arg(compose::kMaxItems);
    if (warning != m_warningText->text()) {
        m_warningText->setText(warning);
        m_warning->setVisible(!warning.isEmpty());
        if (!warning.isEmpty())
            announce(m_warningText);
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
    m_captionHelp->setVisible(!help.isEmpty() || !count.isEmpty());

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
    m_bannerText->setText(blocker);
    m_banner->setVisible(!blocker.isEmpty());
    if (m_presence)
        m_presence->setVisible(blocker.isEmpty());
    if (!blocker.isEmpty())
        announce(m_bannerText);
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
        if (problem == Problem::None) {
            QFile file(item.path);
            if (!file.open(QIODevice::ReadOnly))
                problem = Problem::Unreadable;
        }
        if (problem != item.problem || fi.size() != item.size) {
            item.problem = problem;
            item.size    = fi.size();
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
    l->activate();
    const QScreen* screen = parentWidget() ? parentWidget()->screen() : this->screen();
    const int      cap    = screen ? screen->availableGeometry().height() * 8 / 10 : 800;
    int            height = l->hasHeightForWidth() ? l->totalHeightForWidth(width()) : l->totalSizeHint().height();
    // A long list gets fewer visible rows rather than a window taller than the screen allows.
    while (height > cap && m_scroll && m_visibleRows > 2) {
        const int rows = qMin(m_items.size(), --m_visibleRows);
        m_scroll->setFixedHeight(rows * kRowHeight + (rows - 1) * kRowGap);
        l->activate();
        height = l->hasHeightForWidth() ? l->totalHeightForWidth(width()) : l->totalSizeHint().height();
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
        } else {
            sendItem.path = item.path;
        }
        sendItem.spoiler = item.spoiler && compose::spoilerAllowed(item.kind);
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
    QString dir;
    for (const Item& item : qAsConst(m_items)) {
        if (!item.isPasted()) {
            dir = QFileInfo(item.path).absolutePath();
            break;
        }
    }
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QPointer<ComposeDialog> guard(this);
    const QStringList             files = QFileDialog::getOpenFileNames(
        this, i18n::t("Add files to send"), dir,
        i18n::t("All files (*.*);;Images (*.png *.jpg *.jpeg *.jfif *.gif *.webp *.bmp);;Videos (*.mp4 *.webm *.mkv *.mov *.avi *.wmv *.m4v)"));
    if (!guard || files.isEmpty())
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
        m_dropHint->setGeometry(rect().adjusted(8, 8, -8, -8));
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
        fitHeight();
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
        m_dropHint->setGeometry(rect().adjusted(8, 8, -8, -8));
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
    const Colors colors = colorsFor(palette());
    m_dark              = colors.dark;
    const qreal dpr     = devicePixelRatioF();

    for (QLabel* label : findChildren<QLabel*>()) {
        const QString role = label->property("role").toString();
        if (!role.isEmpty())
            setRole(label, role == QLatin1String("error") ? "error" : "hint", colors);
    }

    QString sheet;
    if (!qApp->styleSheet().isEmpty()) {
        // TeamSpeak's skins colour labels by style sheet, which beats a palette. fromLatin1, never
        // QStringLiteral: TeamSpeak's style keeps the parsed text after the plugin is unloaded.
        sheet = QString::fromLatin1("#tsmediaCompose QLabel[role=\"hint\"]{color:%1;}"
                                    "#tsmediaCompose QLabel[role=\"error\"]{color:%2;}")
                    .arg(colors.muted.name(), colors.error.name());
        if (colors.dark) {
            // Dark skins turn focus indicators off: the field being typed in gets an accent border, and
            // after keyboard use the buttons and the checkboxes get rings (as in the settings window).
            sheet += QString::fromLatin1("#tsmediaCompose QLineEdit:focus{border-color:%1;}").arg(colors.accent.name());
            if (m_keyboardFocus)
                sheet += QString::fromLatin1("#tsmediaCompose QPushButton:focus{border:2px solid #ffffff;padding:6px 12px;}"
                                             "#tsmediaCompose QCheckBox:focus{outline:1px solid %1;}")
                             .arg(colors.text.name());
        }
    }
    if (sheet != styleSheet())
        setStyleSheet(sheet);

    m_bannerIcon->setPixmap(alertPixmap(16, dpr, colors.error, colors.window));
    m_warningIcon->setPixmap(alertPixmap(16, dpr, colors.error, colors.window));
    m_dropHint->setColors(colors);
    for (Row* row : qAsConst(m_rows)) {
        const int index = indexOf(row->id());
        if (index >= 0)
            row->setItem(m_items.at(index), m_limitMB, colors, dpr);
    }
    if (m_singleThumb)
        m_singleThumb->setColors(colors);
    m_applyingTheme = false;
}

void ComposeDialog::announce(QWidget* widget)
{
    if (!widget || !QAccessible::isActive())
        return;
    QAccessibleEvent event(widget, QAccessible::NameChanged); // polite: the focus stays where it is
    QAccessible::updateAccessibility(&event);
}
