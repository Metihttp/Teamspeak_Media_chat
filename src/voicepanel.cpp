#include "voicepanel.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>
#include <functional>

#include "audio/waveform.h"
#include "i18n.h"
#include "medialink.h"
#include "previewpaint.h"
#include "uiutil.h"

namespace pp = previewpaint;

namespace {

constexpr int   kWidth      = 420;
constexpr int   kPadding    = 16;
constexpr int   kSpacing    = 8;
constexpr int   kWaveHeight = 48;
constexpr qreal kBar        = 3.0;
constexpr qreal kBarGap     = 2.0;
constexpr int   kSeekStepMs = 5000;

const QColor kAccent(0x58, 0x65, 0xf2);
const QColor kAccentHover(0x47, 0x52, 0xc4);
const QColor kAccentPressed(0x3c, 0x45, 0xa5);

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

// The window's colours, from TeamSpeak's palette (its light or dark skin) and the 2.1 tokens.
struct Colors {
    bool   dark = false;
    QColor window;
    QColor text;
    QColor muted;    // 4.5:1 or more
    QColor error;    // 4.5:1 or more
    QColor focus;    // focus rings, 3:1
    QColor record;   // the red dot
    QColor live;     // live bars, 3:1
    QColor played;   // review: played bars (3:1)
    QColor unplayed; // review: bars still to play (3:1)
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
    c.error = c.dark ? QColor(0xfa, 0x77, 0x7c) : QColor(0xc4, 0x28, 0x2d);
    if (ui::contrastRatio(c.error, c.window) < 4.5)
        c.error = c.text;
    c.focus = c.dark ? QColor(0x94, 0x9c, 0xf7) : QColor(0x47, 0x52, 0xc4);
    if (ui::contrastRatio(c.focus, c.window) < 3.0)
        c.focus = c.text;
    c.record   = c.dark ? QColor(0xf2, 0x3f, 0x43) : QColor(0xda, 0x37, 0x3c);
    c.live     = c.dark ? QColor(0x94, 0x9c, 0xf7) : kAccent;
    c.played   = c.dark ? QColor(0xc9, 0xcd, 0xfb) : QColor(0x47, 0x52, 0xc4);
    c.unplayed = QColor(0x80, 0x84, 0x8e);
    for (QColor* bars : {&c.live, &c.played, &c.unplayed}) {
        if (ui::contrastRatio(*bars, c.window) < 3.0)
            *bars = c.text;
    }
    return c;
}

QFont scaledFont(const QFont& base, qreal factor, bool bold)
{
    QFont font(base);
    if (base.pixelSize() > 0)
        font.setPixelSize(qMax(1, qRound(base.pixelSize() * factor)));
    else
        font.setPointSizeF(qMax(1.0, (base.pointSizeF() > 0 ? base.pointSizeF() : 9.0) * factor));
    if (bold)
        font.setWeight(QFont::DemiBold);
    return font;
}

void setRole(QLabel* label, const char* role)
{
    const QString value = QString::fromLatin1(role);
    if (label->property("role").toString() == value)
        return;
    label->setProperty("role", value);
    if (label->style()) { // the style sheet picks the new colour
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
}

QPixmap alertPixmap(int size, qreal dpr, const QColor& fill)
{
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    pp::drawAlertIcon(p, QRectF(0, 0, size, size), fill, Qt::white);
    return pixmap;
}

} // namespace

// ---- the round red "recording" dot -------------------------------------------------------------------

class VoicePanel::Dot : public QWidget
{
  public:
    explicit Dot(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(10, 10);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void set(const QColor& color, qreal opacity)
    {
        if (color == m_color && qFuzzyCompare(opacity, m_opacity))
            return;
        m_color   = color;
        m_opacity = opacity;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(m_opacity);
        p.setPen(Qt::NoPen);
        p.setBrush(m_color);
        p.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    }

  private:
    QColor m_color;
    qreal  m_opacity = 1.0;
};

// ---- the review's round play button --------------------------------------------------------------------

class VoicePanel::PlayButton : public QAbstractButton
{
  public:
    explicit PlayButton(QWidget* parent)
        : QAbstractButton(parent)
    {
        setFixedSize(40, 40); // the 36 px disc plus room for the focus ring
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
    }
    void setPlaying(bool playing)
    {
        if (playing == m_playing && !toolTip().isEmpty())
            return;
        m_playing = playing;
        setToolTip(playing ? i18n::t("Pause (Space)") : i18n::t("Play (Space)"));
        setAccessibleName(playing ? i18n::t("Pause") : i18n::t("Play"));
        update();
    }
    void setFocusColor(const QColor& color)
    {
        m_focus = color;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF disc = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
        if (hasFocus()) {
            p.setPen(QPen(m_focus, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(isDown() ? kAccentPressed : underMouse() ? kAccentHover : kAccent);
        p.drawEllipse(disc.adjusted(1.0, 1.0, -1.0, -1.0));
        const QRectF icon = disc.adjusted(11.0, 11.0, -11.0, -11.0);
        if (m_playing)
            pp::drawPauseIcon(p, icon, Qt::white);
        else
            pp::drawPlayIcon(p, icon.translated(1.0, 0.0), Qt::white);
    }
    void enterEvent(QEvent* event) override
    {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent* event) override
    {
        QAbstractButton::leaveEvent(event);
        update();
    }

  private:
    bool   m_playing = false;
    QColor m_focus   = kAccentHover;
};

// ---- the waveform: live while recording, seekable in the review ------------------------------------------

class VoicePanel::WaveStrip : public QWidget
{
  public:
    std::function<void(double)> onSeek;
    std::function<void(qint64)> onSeekBy;

    explicit WaveStrip(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumHeight(kWaveHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setAccessibleName(i18n::t("Voice message waveform"));
    }

    void setColors(const Colors& colors)
    {
        m_colors = colors;
        update();
    }
    void setLive(const QVector<float>& bins)
    {
        m_review = false;
        m_bins   = bins;
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
        update();
    }
    void setReview(const QByteArray& levels, double fraction, bool started)
    {
        m_review   = true;
        m_levels   = levels;
        m_fraction = qBound(0.0, fraction, 1.0);
        m_started  = started;
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF area   = QRectF(rect()).adjusted(2.0, 4.0, -2.0, -4.0);
        const int    count  = qMax(0, static_cast<int>((area.width() + kBarGap) / (kBar + kBarGap)));
        const qreal  center = area.center().y();
        const qreal  range  = area.height() - 3.0;
        p.setPen(Qt::NoPen);
        if (!m_review) {
            // Newest at the right; places not recorded yet are small dots.
            const int have = m_bins.size();
            for (int i = 0; i < count; ++i) {
                const int  bin = have - count + i;
                const bool on  = bin >= 0;
                const qreal h  = on ? 3.0 + range * waveform::liveHeight(m_bins.at(bin)) : 3.0;
                p.setBrush(on ? m_colors.live : withAlpha(m_colors.unplayed, 0.6));
                p.drawRoundedRect(QRectF(area.left() + i * (kBar + kBarGap), center - h / 2.0, kBar, h), kBar / 2.0, kBar / 2.0);
            }
        } else {
            const QVector<quint8> bars  = waveform::resample(m_levels, count);
            const qreal           split = area.left() + area.width() * m_fraction;
            auto paint = [&](const QColor& color) {
                p.setBrush(color);
                for (int i = 0; i < count; ++i) {
                    const qreal h = 3.0 + range * bars.at(i) / 15.0;
                    p.drawRoundedRect(QRectF(area.left() + i * (kBar + kBarGap), center - h / 2.0, kBar, h), kBar / 2.0, kBar / 2.0);
                }
            };
            paint(m_colors.unplayed);
            if (m_started && m_fraction > 0.0) {
                p.save();
                p.setClipRect(QRectF(area.left() - 1.0, 0.0, split - area.left() + 1.0, height()));
                paint(m_colors.played);
                p.restore();
            }
            if (m_started) {
                p.setBrush(m_colors.played);
                p.drawRoundedRect(QRectF(qBound(area.left(), split - 1.0, area.right() - 2.0), center - range / 2.0 - 2.0, 2.0, range + 4.0), 1.0, 1.0);
            }
            if (m_hoverX >= 0) { // where a click would jump to
                p.setBrush(m_colors.text);
                p.drawRect(QRectF(qBound(area.left(), m_hoverX, area.right()) - 0.5, area.top(), 1.0, area.height()));
            }
            if (hasFocus()) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(m_colors.focus, 2.0));
                p.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 4.0, 4.0);
            }
        }
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (m_review && event->button() == Qt::LeftButton) {
            m_dragging = true;
            seekTo(event->localPos().x());
        }
        QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_review)
            return;
        m_hoverX = event->localPos().x();
        update();
        if (m_dragging && (!m_seekClock.isValid() || m_seekClock.elapsed() >= 60))
            seekTo(event->localPos().x());
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_dragging && event->button() == Qt::LeftButton) {
            m_dragging = false;
            seekTo(event->localPos().x());
        }
    }
    void leaveEvent(QEvent*) override
    {
        m_hoverX = -1;
        update();
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (m_review) {
            switch (event->key()) {
            case Qt::Key_Left:
                if (onSeekBy)
                    onSeekBy(-kSeekStepMs);
                return;
            case Qt::Key_Right:
                if (onSeekBy)
                    onSeekBy(kSeekStepMs);
                return;
            case Qt::Key_Home:
                if (onSeek)
                    onSeek(0.0);
                return;
            case Qt::Key_End:
                if (onSeek)
                    onSeek(1.0);
                return;
            default:
                break;
            }
        }
        QWidget::keyPressEvent(event);
    }
    void focusInEvent(QFocusEvent* event) override
    {
        QWidget::focusInEvent(event);
        update();
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        QWidget::focusOutEvent(event);
        update();
    }

  private:
    void seekTo(qreal x)
    {
        const QRectF area = QRectF(rect()).adjusted(2.0, 4.0, -2.0, -4.0);
        if (area.width() <= 0.0 || !onSeek)
            return;
        m_seekClock.restart();
        onSeek(qBound(0.0, (x - area.left()) / area.width(), 1.0));
    }

    Colors          m_colors;
    bool            m_review   = false;
    QVector<float>  m_bins;
    QByteArray      m_levels;
    double          m_fraction = 0.0;
    bool            m_started  = false;
    bool            m_dragging = false;
    qreal           m_hoverX   = -1;
    QElapsedTimer   m_seekClock;
};

// ---- the window --------------------------------------------------------------------------------------

VoicePanel::VoicePanel(QWidget* parent)
    : QDialog(parent, Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint)
{
    // fromLatin1: plugin shutdown finds it by this name; it lives among TeamSpeak's windows.
    setObjectName(QString::fromLatin1("tsmediaVoiceRecorder"));
    setWindowTitle(i18n::t("Voice message"));
    setLayoutDirection(Qt::LeftToRight);
    setFocusPolicy(Qt::StrongFocus); // keys go to the window unless a button was tabbed to

    m_dot    = new Dot(this);
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setFont(scaledFont(font(), 1.0, true));
    m_target = new QLabel(this);
    m_target->setTextFormat(Qt::PlainText);
    m_target->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_target->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setRole(m_target, "hint");
    auto* header = new QHBoxLayout;
    header->setSpacing(kSpacing);
    header->addWidget(m_dot, 0, Qt::AlignVCenter);
    header->addWidget(m_status);
    header->addWidget(m_target, 1);

    m_mainRow = new QWidget(this);
    m_play    = new PlayButton(m_mainRow);
    m_wave    = new WaveStrip(m_mainRow);
    m_time    = new QLabel(m_mainRow);
    m_time->setTextFormat(Qt::PlainText);
    m_time->setFont(scaledFont(font(), 1.25, true));
    m_time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_time->setMinimumWidth(QFontMetrics(m_time->font()).horizontalAdvance(QString::fromLatin1("00:00")) + 2);
    m_limit = new QLabel(m_mainRow);
    m_limit->setTextFormat(Qt::PlainText);
    setRole(m_limit, "hint");
    auto* main = new QHBoxLayout(m_mainRow);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(kSpacing);
    main->addWidget(m_play, 0, Qt::AlignVCenter);
    main->addWidget(m_wave, 1);
    main->addWidget(m_time, 0, Qt::AlignVCenter);
    main->addWidget(m_limit, 0, Qt::AlignVCenter);

    m_hint = new QLabel(this);
    m_hint->setTextFormat(Qt::PlainText);
    m_hint->setWordWrap(true);
    setRole(m_hint, "hint");
    m_hint2 = new QLabel(this);
    m_hint2->setTextFormat(Qt::PlainText);
    m_hint2->setWordWrap(true);
    setRole(m_hint2, "hint");

    m_errorBox   = new QWidget(this);
    m_errorIcon  = new QLabel(m_errorBox);
    m_errorTitle = new QLabel(m_errorBox);
    m_errorTitle->setTextFormat(Qt::PlainText);
    m_errorTitle->setFont(scaledFont(font(), 1.0, true));
    m_errorTitle->setWordWrap(true);
    m_errorBody = new QLabel(m_errorBox);
    m_errorBody->setTextFormat(Qt::PlainText);
    m_errorBody->setWordWrap(true);
    auto* errorText = new QVBoxLayout;
    errorText->setSpacing(4);
    errorText->addWidget(m_errorTitle);
    errorText->addWidget(m_errorBody);
    auto* error = new QHBoxLayout(m_errorBox);
    error->setContentsMargins(0, 0, 0, 0);
    error->setSpacing(12);
    error->addWidget(m_errorIcon, 0, Qt::AlignTop);
    error->addLayout(errorText, 1);

    m_question = new QLabel(i18n::t("Discard this voice message?"), this);
    m_question->setTextFormat(Qt::PlainText);
    m_question->setFont(scaledFont(font(), 1.0, true));

    m_left   = new QPushButton(this);
    m_middle = new QPushButton(this);
    m_right  = new QPushButton(this);
    for (QPushButton* b : {m_left, m_middle, m_right}) {
        b->setAutoDefault(false);
        b->setDefault(false);
        b->setFocusPolicy(Qt::TabFocus); // a click doesn't take the keys away from the window
        b->setMinimumSize(80, 32);
    }
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(kSpacing);
    buttons->addWidget(m_left);
    buttons->addStretch(1);
    buttons->addWidget(m_middle);
    buttons->addWidget(m_right);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kPadding, kPadding - 4, kPadding, kPadding);
    layout->setSpacing(kSpacing);
    layout->addLayout(header);
    layout->addWidget(m_mainRow);
    layout->addWidget(m_errorBox);
    layout->addWidget(m_hint);
    layout->addWidget(m_hint2);
    layout->addWidget(m_question);
    layout->addLayout(buttons);
    setTabOrder(m_play, m_wave);
    setTabOrder(m_wave, m_left);
    setTabOrder(m_left, m_middle);
    setTabOrder(m_middle, m_right);

    m_wave->onSeek   = [this](double fraction) { emit seekRequested(fraction); };
    m_wave->onSeekBy = [this](qint64 delta) { emit seekByRequested(delta); };
    connect(m_play, &QAbstractButton::clicked, this, &VoicePanel::playToggled);
    connect(m_left, &QPushButton::clicked, this, [this] {
        if (m_view.confirmDiscard)
            emit keepRequested();
        else if (m_view.mode == Mode::Error || m_view.mode == Mode::TooShort)
            emit closeRequested();
        else
            emit cancelRequested();
    });
    connect(m_middle, &QPushButton::clicked, this, [this] {
        if (m_view.mode == Mode::Review)
            emit rerecordRequested();
        else
            emit stopRequested();
    });
    connect(m_right, &QPushButton::clicked, this, [this] {
        if (m_view.confirmDiscard)
            emit discardConfirmed();
        else if (m_view.mode == Mode::Error)
            emit fixRequested();
        else if (m_view.mode == Mode::TooShort)
            emit closeRequested();
        else
            emit sendRequested();
    });

    applyTheme();
    setView(m_view);
}

bool VoicePanel::buttonHasFocus() const
{
    QWidget* focus = focusWidget();
    return focus && focus->hasFocus() && qobject_cast<QAbstractButton*>(focus) && focus->isVisible();
}

void VoicePanel::setView(const View& view)
{
    // The buttons change meaning with the state (Stop becomes Re-record, Keep becomes Discard): a button
    // that had the keyboard focus gives it back to the window, so Enter / Space / Esc keep their
    // documented meaning instead of pressing whatever now sits there.
    const bool newButtons = view.mode != m_view.mode || view.confirmDiscard != m_view.confirmDiscard;
    if (newButtons && buttonHasFocus() && !view.confirmDiscard)
        setFocus(Qt::OtherFocusReason);
    m_view            = view;
    const Mode   mode = view.mode;
    const Colors c    = colorsFor(palette());
    const bool   main = mode == Mode::Starting || mode == Mode::Recording || mode == Mode::Saving || mode == Mode::Review;

    // Header: the red dot and "Recording" (never colour alone), or what the window shows now.
    QString status;
    switch (mode) {
    case Mode::Starting:
        status = i18n::t("Starting microphone…");
        break;
    case Mode::Recording:
        status = i18n::t("Recording");
        break;
    case Mode::Saving:
        status = view.savingShown ? i18n::t("Saving…") : i18n::t("Recording");
        break;
    case Mode::Review:
        status = i18n::t("Review");
        break;
    case Mode::TooShort:
    case Mode::Error:
        status = i18n::t("Voice message");
        break;
    }
    m_dot->setVisible(mode == Mode::Recording);
    if (mode == Mode::Recording) {
        const qreal phase = (view.timeMs % 1000) / 1000.0;
        m_dot->set(c.record, view.animate ? 0.55 + 0.45 * (0.5 + 0.5 * std::cos(2.0 * M_PI * phase)) : 1.0);
    }
    if (m_status->text() != status) {
        m_status->setText(status);
        announce(m_status);
    }
    m_status->setToolTip(view.device.isEmpty() ? QString() : i18n::t("Microphone: %1").arg(view.device));
    const QString target = view.target.isEmpty() ? QString() : i18n::t("to %1").arg(view.target);
    m_target->setText(QFontMetrics(m_target->font()).elidedText(target, Qt::ElideMiddle, qMax(80, kWidth - 2 * kPadding - m_status->sizeHint().width() - 40)));
    m_target->setToolTip(target);

    // The waveform row.
    m_mainRow->setVisible(main);
    m_play->setVisible(mode == Mode::Review);
    m_play->setPlaying(view.playing);
    if (mode == Mode::Review) {
        const double fraction = view.lengthMs > 0 ? static_cast<double>(view.timeMs) / static_cast<double>(view.lengthMs) : 0.0;
        m_wave->setReview(view.levels, fraction, view.started);
        m_time->setText(formatDuration(view.started ? view.timeMs : view.lengthMs));
        m_limit->hide();
    } else {
        m_wave->setLive(view.liveBins);
        m_time->setText(formatDuration(view.timeMs));
        m_limit->setText(i18n::t(" / %1").arg(formatDuration(view.limitMs)));
        m_limit->show();
    }
    const QString timeSheet = view.timeWarning ? QString::fromLatin1("color:%1;").arg(c.error.name()) : QString();
    if (m_time->styleSheet() != timeSheet)
        m_time->setStyleSheet(timeSheet);

    // The error state.
    m_errorBox->setVisible(mode == Mode::Error);
    m_errorTitle->setText(view.errorTitle);
    m_errorBody->setText(view.errorBody);
    m_errorIcon->setPixmap(alertPixmap(26, devicePixelRatioF(), c.error));

    // Hints (or the question).
    m_question->setVisible(view.confirmDiscard);
    const QString hint  = view.confirmDiscard ? QString() : view.hints.value(0);
    const QString hint2 = view.confirmDiscard ? QString() : view.hints.mid(1).join(QLatin1Char('\n'));
    m_hint->setVisible(!hint.isEmpty());
    m_hint->setText(hint);
    setRole(m_hint, view.hintError ? "error" : "hint");
    m_hint2->setVisible(!hint2.isEmpty());
    m_hint2->setText(hint2);
    const QString both = hint + QLatin1Char('\n') + hint2;
    if (both != m_lastHint) {
        m_lastHint = both;
        if (!hint.isEmpty())
            announce(m_hint);
    }

    rebuildButtons();
    // Only when what decides the height changed (setView runs 30 times a second while recording).
    const QString layoutKey = QString::number(static_cast<int>(mode)) + QLatin1Char(view.confirmDiscard ? 'c' : '-') + both + view.errorTitle + view.errorBody + view.fixText;
    if (layoutKey != m_layoutKey) {
        m_layoutKey = layoutKey;
        fitHeight(true);
    }
}

// Fixed width; the height follows the content (wrapped hints, the error text). Once more when the
// layout catches up with widgets that were just shown (LayoutRequest).
void VoicePanel::fitHeight(bool invalidate)
{
    if (invalidate) // not from the LayoutRequest itself: invalidate() posts the next one
        layout()->invalidate();
    const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(kWidth) : layout()->totalSizeHint().height();
    if (size() != QSize(kWidth, height)) {
        setFixedSize(kWidth, height);
        keepPlaced();
    }
}

bool VoicePanel::event(QEvent* event)
{
    const bool result = QDialog::event(event);
    if (event->type() == QEvent::LayoutRequest)
        fitHeight(false);
    return result;
}

void VoicePanel::rebuildButtons()
{
    const Mode mode = m_view.mode;
    auto set = [](QPushButton* b, const QString& text, const QString& tip, bool visible, bool enabled = true, const char* name = "") {
        if (b->text() != text)
            b->setText(text);
        if (b->toolTip() != tip)
            b->setToolTip(tip);
        b->setVisible(visible);
        b->setEnabled(enabled);
        const QString objectName = QString::fromLatin1(name);
        if (b->objectName() != objectName) { // "voiceDanger": the error colour (style sheet)
            b->setObjectName(objectName);
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
    };
    if (m_view.confirmDiscard) {
        // Keep (focused) at the left, the destructive Discard apart at the right, in the error colour.
        set(m_left, i18n::t("&Keep"), i18n::t("Keep the recording (Esc)"), true);
        set(m_middle, QString(), QString(), false);
        set(m_right, i18n::t("&Discard"), i18n::t("Delete the recording"), true, true, "voiceDanger");
        if (!m_left->hasFocus())
            m_left->setFocus(Qt::OtherFocusReason);
        return;
    }
    switch (mode) {
    case Mode::Starting:
        set(m_left, i18n::t("&Cancel"), i18n::t("Discard the recording (Esc)"), true);
        set(m_middle, QString(), QString(), false);
        set(m_right, QString(), QString(), false);
        break;
    case Mode::Recording:
        set(m_left, i18n::t("&Cancel"), i18n::t("Discard the recording (Esc)"), true);
        set(m_middle, i18n::t("&Stop"), i18n::t("Stop and listen before sending (Space)"), true);
        set(m_right, i18n::t("&Send"), i18n::t("Send now (Enter)"), true, m_view.canSend);
        break;
    case Mode::Saving:
        set(m_left, i18n::t("&Cancel"), i18n::t("Discard the recording (Esc)"), true);
        set(m_middle, QString(), QString(), false);
        set(m_right, i18n::t("&Send"), i18n::t("Send now (Enter)"), true, false);
        break;
    case Mode::Review:
        set(m_left, i18n::t("&Discard"), i18n::t("Discard the recording (Esc)"), true);
        set(m_middle, i18n::t("&Re-record"), i18n::t("Discard and record again"), true);
        set(m_right, i18n::t("&Send"), i18n::t("Send (Enter)"), true, m_view.canSend);
        break;
    case Mode::TooShort:
        set(m_left, QString(), QString(), false);
        set(m_middle, QString(), QString(), false);
        set(m_right, i18n::t("&Close"), QString(), true);
        break;
    case Mode::Error:
        set(m_left, i18n::t("&Close"), QString(), true);
        set(m_middle, QString(), QString(), false);
        set(m_right, m_view.fixText, QString(), m_view.fix != Fix::None && !m_view.fixText.isEmpty());
        break;
    }
}

void VoicePanel::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    const Colors c = colorsFor(palette());
    m_dark         = c.dark;
    m_wave->setColors(c);
    m_play->setFocusColor(c.focus);

    // TeamSpeak's skins colour labels by style sheet, which beats a palette. fromLatin1, never
    // QStringLiteral: TeamSpeak's style keeps the parsed text after the plugin is unloaded.
    QString sheet = QString::fromLatin1("#tsmediaVoiceRecorder QLabel[role=\"hint\"]{color:%1;}"
                                        "#tsmediaVoiceRecorder QLabel[role=\"error\"]{color:%2;}"
                                        "#tsmediaVoiceRecorder QPushButton#voiceDanger{color:%2;}")
                        .arg(c.muted.name(), c.error.name());
    if (c.dark && !qApp->styleSheet().isEmpty()) // dark skins switch focus indicators off
        sheet += QString::fromLatin1("#tsmediaVoiceRecorder QPushButton:focus{border:2px solid %1;}").arg(c.focus.name());
    if (sheet != styleSheet())
        setStyleSheet(sheet);
    m_applyingTheme = false;
}

void VoicePanel::announce(QWidget* widget)
{
    if (!widget || !QAccessible::isActive())
        return;
    QAccessibleEvent event(widget, QAccessible::NameChanged); // polite: the focus stays where it is
    QAccessible::updateAccessibility(&event);
}

void VoicePanel::placeNear(QWidget* anchor)
{
    m_anchor       = anchor;
    m_placedBottom = -1;
    keepPlaced();
}

void VoicePanel::keepPlaced()
{
    const QSize size(kWidth, height());
    QScreen*    screen = nullptr;
    QPoint      topLeft;
    if (m_placedBottom < 0) {
        if (m_anchor && m_anchor->isVisible()) {
            const QPoint anchorTop = m_anchor->mapToGlobal(QPoint(m_anchor->width() / 2, 0));
            topLeft                = QPoint(anchorTop.x() - size.width() / 2, anchorTop.y() - 8 - size.height());
            screen                 = m_anchor->screen();
        } else {
            screen  = QGuiApplication::primaryScreen();
            topLeft = screen ? screen->availableGeometry().center() - QPoint(size.width() / 2, size.height() / 2) : QPoint(100, 100);
        }
    } else {
        topLeft = QPoint(geometry().left(), m_placedBottom - size.height());
        screen  = this->screen();
    }
    if (screen) {
        const QRect area = screen->availableGeometry();
        // The window frame (title bar) sits above the client area.
        const int titleBar = qMax(0, frameGeometry().height() - geometry().height());
        topLeft.setX(qBound(area.left(), topLeft.x(), qMax(area.left(), area.right() - size.width())));
        topLeft.setY(qBound(area.top() + titleBar, topLeft.y(), qMax(area.top() + titleBar, area.bottom() - size.height())));
    }
    move(topLeft);
    m_placedBottom = topLeft.y() + size.height();
}

void VoicePanel::keyPressEvent(QKeyEvent* event)
{
    const int  key    = event->key();
    const bool enter  = key == Qt::Key_Return || key == Qt::Key_Enter;
    const Mode mode   = m_view.mode;
    if (enter && buttonHasFocus()) {
        if (auto* button = qobject_cast<QAbstractButton*>(focusWidget()); button && button->isEnabled())
            button->click();
        return;
    }
    if (key == Qt::Key_Escape) {
        if (m_view.confirmDiscard)
            emit keepRequested();
        else if (mode == Mode::Error || mode == Mode::TooShort)
            emit closeRequested();
        else
            emit cancelRequested();
        return;
    }
    if (m_view.confirmDiscard) {
        QDialog::keyPressEvent(event);
        return;
    }
    if (enter) {
        if (mode == Mode::Error) {
            if (m_view.fix != Fix::None)
                emit fixRequested();
        } else if (mode == Mode::TooShort) {
            emit closeRequested();
        } else if ((mode == Mode::Recording || mode == Mode::Review) && m_view.canSend) {
            emit sendRequested();
        }
        return;
    }
    if (key == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        if (mode == Mode::Recording)
            emit stopRequested();
        else if (mode == Mode::Review)
            emit playToggled();
        return;
    }
    QDialog::keyPressEvent(event);
}

void VoicePanel::closeEvent(QCloseEvent* event)
{
    // The close button: the same as Cancel / Close; the controller decides (and may ask first).
    event->ignore();
    if (m_view.confirmDiscard)
        return;
    if (m_view.mode == Mode::Error || m_view.mode == Mode::TooShort)
        emit closeRequested();
    else
        emit cancelRequested();
}

void VoicePanel::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        applyTheme();
}

void VoicePanel::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!focusWidget() || !focusWidget()->isVisible())
        setFocus(Qt::OtherFocusReason);
}
