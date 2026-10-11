#include "polldialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QVBoxLayout>

#include "i18n.h"
#include "polls.h"
#include "uiutil.h"

namespace {

constexpr int kWidth = 440;

struct Tokens {
    bool   dark = false;
    QColor bg, surface, border, text, muted, accent, accentHover, accentText, danger, hover, disabled;
};

QColor alpha(QColor c, qreal a)
{
    c.setAlphaF(a);
    return c;
}

Tokens tokensFor(const QPalette& pal)
{
    Tokens t;
    t.bg   = pal.color(QPalette::Active, QPalette::Window);
    t.dark = t.bg.lightness() < 128;
    t.text = pal.color(QPalette::Active, QPalette::WindowText);
    if (ui::contrastRatio(t.text, t.bg) < 7.0)
        t.text = t.dark ? QColor(0xe3, 0xe5, 0xe8) : QColor(0x1e, 0x1f, 0x22);
    if (t.dark) {
        t.surface     = ui::flatten(QColor(0, 0, 0, 56), t.bg);
        t.border      = ui::flatten(QColor(255, 255, 255, 30), t.bg);
        t.accent      = QColor(0x58, 0x65, 0xf2);
        t.accentHover = QColor(0x47, 0x52, 0xc4);
        t.accentText  = QColor(0xa5, 0xab, 0xf8);
        t.danger      = QColor(0xfa, 0x77, 0x7c);
        t.hover       = ui::flatten(QColor(255, 255, 255, 18), t.bg);
    } else {
        t.surface     = ui::flatten(QColor(0, 0, 0, 9), t.bg);
        t.border      = ui::flatten(QColor(0, 0, 0, 46), t.bg);
        t.accent      = QColor(0x58, 0x65, 0xf2);
        t.accentHover = QColor(0x47, 0x52, 0xc4);
        t.accentText  = QColor(0x47, 0x52, 0xc4);
        t.danger      = QColor(0xc4, 0x28, 0x2d);
        t.hover       = ui::flatten(QColor(0, 0, 0, 14), t.bg);
    }
    t.muted = ui::flatten(alpha(t.text, 0.66), t.bg);
    if (ui::contrastRatio(t.muted, t.bg) < 4.5)
        t.muted = ui::flatten(alpha(t.text, 0.8), t.bg);
    t.disabled = ui::flatten(alpha(t.text, 0.18), t.bg);
    return t;
}

Tokens tokensOf(const QWidget* w)
{
    const QWidget* top = w ? w->window() : nullptr;
    return tokensFor(top ? top->palette() : QPalette());
}

QFont scaledFont(const QFont& base, qreal factor, bool bold)
{
    QFont f(base);
    if (base.pixelSize() > 0)
        f.setPixelSize(qMax(1, qRound(base.pixelSize() * factor)));
    else
        f.setPointSizeF(qMax(1.0, (base.pointSizeF() > 0 ? base.pointSizeF() : 9.0) * factor));
    f.setBold(bold);
    return f;
}

QLabel* label(const QString& text, const char* role, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setTextFormat(Qt::PlainText);
    l->setProperty("role", QString::fromLatin1(role));
    return l;
}

QString counterText(int used, int max)
{
    return QString::fromLatin1("%1/%2").arg(used).arg(max);
}

} // namespace

// ---- the pieces ---------------------------------------------------------------------------------------------

// The grip left of an option: drag it to move the option (Alt+Up / Alt+Down do the same).
class PollGrip : public QWidget
{
  public:
    PollGrip(PollOptionRow* row, QWidget* parent)
        : QWidget(parent)
        , m_row(row)
    {
        setFixedSize(18, 34);
        setCursor(Qt::OpenHandCursor);
        setAttribute(Qt::WA_Hover);
        setToolTip(i18n::t("Drag to move this option (or Alt+Up / Alt+Down)"));
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Tokens t = tokensOf(this);
        p.setPen(Qt::NoPen);
        p.setBrush(underMouse() || m_dragging ? t.text : t.muted);
        const qreal cx = width() / 2.0, cy = height() / 2.0;
        for (int r = -1; r <= 1; ++r) {
            for (int c = 0; c < 2; ++c)
                p.drawEllipse(QPointF(cx - 2.5 + c * 5.0, cy + r * 5.0), 1.45, 1.45);
        }
    }
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        update();
        QWidget::mouseReleaseEvent(event);
    }

  private:
    PollOptionRow* m_row;
    bool           m_dragging = false;
};

// The × that removes an option.
class PollRemove : public QAbstractButton
{
  public:
    explicit PollRemove(QWidget* parent)
        : QAbstractButton(parent)
    {
        setFixedSize(30, 30);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::TabFocus);
        setToolTip(i18n::t("Remove this option"));
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Tokens t = tokensOf(this);
        if (isEnabled() && (underMouse() || isDown() || hasFocus())) {
            p.setPen(hasFocus() ? QPen(t.accentText, 1.5) : Qt::NoPen);
            p.setBrush(isDown() ? t.border : t.hover);
            p.drawEllipse(QRectF(rect()).adjusted(3, 3, -3, -3));
        }
        QPen pen(isEnabled() ? (underMouse() ? t.danger : t.muted) : t.disabled, 1.7);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        const QPointF c = QRectF(rect()).center();
        p.drawLine(c + QPointF(-4.2, -4.2), c + QPointF(4.2, 4.2));
        p.drawLine(c + QPointF(4.2, -4.2), c + QPointF(-4.2, 4.2));
    }
};

class PollOptionRow : public QWidget
{
  public:
    PollOptionRow(PollDialog* dialog, QWidget* parent)
        : QWidget(parent)
        , m_dialog(dialog)
    {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        m_grip   = new PollGrip(this, this);
        m_field  = new QLineEdit(this);
        m_remove = new PollRemove(this);
        m_field->setMaxLength(polls::kMaxOptionChars);
        m_field->installEventFilter(dialog);
        row->addWidget(m_grip);
        row->addWidget(m_field, 1);
        row->addWidget(m_remove);
    }

    QLineEdit*  field() const { return m_field; }
    PollRemove* remove() const { return m_remove; }
    PollDialog* dialog() const { return m_dialog; }

  private:
    PollDialog* m_dialog;
    PollGrip*   m_grip;
    QLineEdit*  m_field;
    PollRemove* m_remove;
};

void PollGrip::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        m_row->field()->setFocus(Qt::MouseFocusReason);
        update();
    }
    QWidget::mousePressEvent(event);
}

void PollGrip::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton))
        m_row->dialog()->dragRow(m_row, event->globalPos().y());
    QWidget::mouseMoveEvent(event);
}

// "Allow multiple answers": a switch with its text (one button, for the keyboard and screen readers).
class PollSwitch : public QAbstractButton
{
  public:
    PollSwitch(const QString& text, QWidget* parent)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setText(text);
        setAccessibleName(text);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
    }

    QSize sizeHint() const override { return QSize(fontMetrics().horizontalAdvance(text()) + 34 + 12 + 6, qMax(28, fontMetrics().height() + 8)); }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Tokens t = tokensOf(this);
        const QRectF track(2, (height() - 20) / 2.0, 34, 20);
        if (hasFocus()) {
            p.setPen(QPen(t.accentText, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(track.adjusted(-2.5, -2.5, 2.5, 2.5), 12.5, 12.5);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? (underMouse() ? t.accentHover : t.accent) : (underMouse() ? t.muted : ui::flatten(alpha(t.text, 0.32), t.bg)));
        p.drawRoundedRect(track, 10, 10);
        const qreal knob = 14;
        const qreal x    = isChecked() ? track.right() - 3 - knob : track.left() + 3;
        p.setBrush(QColor(0xff, 0xff, 0xff));
        p.drawEllipse(QRectF(x, track.top() + 3, knob, knob));
        if (isChecked()) {
            QPen pen(t.accent, 1.6);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(pen);
            QPainterPath check;
            const QRectF k(x, track.top() + 3, knob, knob);
            check.moveTo(k.left() + 4.0, k.center().y() + 0.3);
            check.lineTo(k.left() + 6.2, k.center().y() + 2.4);
            check.lineTo(k.right() - 3.6, k.center().y() - 2.4);
            p.drawPath(check);
        }
        p.setPen(t.text);
        p.drawText(QRectF(track.right() + 12, 0, width() - track.right() - 12, height()), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text());
    }
};

// ---- the window ---------------------------------------------------------------------------------------------

PollDialog::PollDialog(const QString& destination, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
{
    // fromLatin1: plugin shutdown finds it by this name; it lives among TeamSpeak's windows.
    setObjectName(QString::fromLatin1("tsmediaPollDialog"));
    // 2.3 ux review: a style sheet of its own from the start (applyTheme fills it in). A window's first own
    // sheet set after TeamSpeak's skin polished it leaves it unpolished (Qt 5.15): the dark skin's palette
    // came back light, and the window was themed light.
    setStyleSheet(QString::fromLatin1("#tsmediaPollDialog{}"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setLayoutDirection(Qt::LeftToRight);
    setWindowTitle(i18n::t("New poll"));
    setFixedWidth(kWidth);

    m_title = label(i18n::t("New poll"), "title", this);
    m_title->setFont(scaledFont(font(), 1.3, true));
    m_subtitle = label(destination.isEmpty() ? i18n::t("An anonymous poll") : i18n::t("An anonymous poll in %1").arg(destination), "muted", this);
    m_subtitle->setWordWrap(true);

    auto* questionHead = new QHBoxLayout;
    auto* questionLabel = label(i18n::t("Question"), "section", this);
    m_questionCount     = label(counterText(0, polls::kMaxQuestionChars), "muted", this);
    questionHead->addWidget(questionLabel);
    questionHead->addStretch(1);
    questionHead->addWidget(m_questionCount);
    m_question = new QLineEdit(this);
    m_question->setMaxLength(polls::kMaxQuestionChars);
    m_question->setPlaceholderText(i18n::t("Ask a question"));
    m_question->setAccessibleName(i18n::t("Question"));
    m_question->installEventFilter(this);
    questionLabel->setBuddy(m_question);
    connect(m_question, &QLineEdit::textChanged, this, [this] {
        updateCounters();
        clearErrors();
    });

    auto* optionsLabel = label(i18n::t("Options"), "section", this);
    m_optionsHost      = new QWidget(this);
    m_optionsLayout    = new QVBoxLayout(m_optionsHost);
    m_optionsLayout->setContentsMargins(0, 0, 0, 0);
    m_optionsLayout->setSpacing(8);

    m_add = new QPushButton(i18n::t("+  Add an option"), this);
    m_add->setProperty("role", QString::fromLatin1("link"));
    m_add->setCursor(Qt::PointingHandCursor);
    m_add->setAutoDefault(false);
    connect(m_add, &QPushButton::clicked, this, [this] { addOption(); });
    m_addHint = label(QString(), "muted", this);
    auto* addRow = new QHBoxLayout;
    addRow->setContentsMargins(18, 0, 0, 0);
    addRow->addWidget(m_add);
    addRow->addStretch(1);
    addRow->addWidget(m_addHint);

    m_multiple = new PollSwitch(i18n::t("Allow multiple answers"), this);
    m_multiple->setToolTip(i18n::t("People can choose more than one option."));

    m_lock = new QLabel(this);
    m_lock->setFixedSize(18, 18);
    m_anonymous = label(i18n::t("Votes are anonymous: nobody can see who voted for what, not even you."), "muted", this);
    m_anonymous->setWordWrap(true);
    auto* anonRow = new QHBoxLayout;
    anonRow->setSpacing(8);
    anonRow->addWidget(m_lock, 0, Qt::AlignTop);
    anonRow->addWidget(m_anonymous, 1);

    // Problems are shown next to what they are about: the question, the options, or (the message's size)
    // above the buttons.
    m_error = label(QString(), "error", this);
    m_error->setWordWrap(true);
    m_error->hide();
    m_questionError = label(QString(), "error", this);
    m_questionError->setWordWrap(true);
    m_questionError->hide();
    m_optionsError = label(QString(), "error", this);
    m_optionsError->setWordWrap(true);
    m_optionsError->hide();

    m_cancel = new QPushButton(i18n::t("Cancel"), this);
    m_cancel->setAutoDefault(false);
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_post = new QPushButton(i18n::t("Post poll"), this);
    m_post->setProperty("role", QString::fromLatin1("primary"));
    m_post->setDefault(true);
    m_post->setCursor(Qt::PointingHandCursor);
    m_post->setToolTip(i18n::t("Post the poll to the chat (Ctrl+Enter)"));
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_post, &QPushButton::clicked, this, [this] { tryPost(); });
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8); // its own: the window's layout has none (it would be inherited)
    buttons->addStretch(1);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_post);

    // 2.3 ux review: on the 4 / 8 px grid.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(0);
    root->addWidget(m_title);
    root->addSpacing(4);
    root->addWidget(m_subtitle);
    root->addSpacing(16);
    root->addLayout(questionHead);
    root->addSpacing(8);
    root->addWidget(m_question);
    root->addWidget(m_questionError);
    root->addSpacing(16);
    root->addWidget(optionsLabel);
    root->addSpacing(8);
    root->addWidget(m_optionsHost);
    root->addWidget(m_optionsError);
    root->addSpacing(4);
    root->addLayout(addRow);
    root->addSpacing(16);
    root->addWidget(m_multiple);
    root->addSpacing(12);
    root->addLayout(anonRow);
    root->addSpacing(8);
    root->addWidget(m_error);
    root->addSpacing(16);
    root->addLayout(buttons);

    addOption(QString(), false);
    addOption(QString(), false);
    applyTheme();
    updateCounters();
    fitHeight(); // 2.3 ux review: with the style sheet's sizes (the fields' padding), not the unpolished ones
    m_question->setFocus(Qt::OtherFocusReason);
}

PollDialog::~PollDialog() = default;

QList<QLineEdit*> PollDialog::optionFields() const
{
    QList<QLineEdit*> out;
    for (PollOptionRow* row : m_rows)
        out << row->field();
    return out;
}

QAbstractButton* PollDialog::addButton() const
{
    return m_add;
}

QAbstractButton* PollDialog::multipleSwitch() const
{
    return m_multiple;
}

QString PollDialog::errorText() const
{
    for (const QLabel* label : {m_questionError, m_optionsError, m_error}) {
        if (!label->isHidden())
            return label->text();
    }
    return {};
}

int PollDialog::indexOf(const QObject* field) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i)->field() == field)
            return i;
    }
    return -1;
}

void PollDialog::addOption(const QString& text, bool focus)
{
    if (m_rows.size() >= polls::kMaxOptions)
        return;
    auto* row = new PollOptionRow(this, m_optionsHost);
    row->field()->setText(text);
    if (m_question) { // after the window was built: the placeholder in the muted colour too (applyTheme)
        QPalette pal = row->field()->palette();
        pal.setColor(QPalette::PlaceholderText, tokensFor(palette()).muted);
        row->field()->setPalette(pal);
    }
    connect(row->field(), &QLineEdit::textChanged, this, [this] { clearErrors(); });
    connect(row->remove(), &QAbstractButton::clicked, this, [this, row] { removeOption(m_rows.indexOf(row)); });
    m_rows.append(row);
    relayoutOptions();
    if (focus)
        row->field()->setFocus(Qt::OtherFocusReason);
}

void PollDialog::removeOption(int index)
{
    if (index < 0 || index >= m_rows.size() || m_rows.size() <= polls::kMinOptions)
        return;
    PollOptionRow* row = m_rows.takeAt(index);
    const bool     had = row->field()->hasFocus() || row->remove()->hasFocus();
    row->hide();
    row->deleteLater();
    relayoutOptions();
    if (had && !m_rows.isEmpty())
        m_rows.at(qMin(index, m_rows.size() - 1))->field()->setFocus(Qt::OtherFocusReason);
}

void PollDialog::moveOption(int from, int to)
{
    if (from < 0 || from >= m_rows.size() || to < 0 || to >= m_rows.size() || from == to)
        return;
    m_rows.move(from, to);
    relayoutOptions();
}

void PollDialog::dragRow(PollOptionRow* row, int globalY)
{
    const int from = m_rows.indexOf(row);
    if (from < 0)
        return;
    // The row goes where the pointer is: after every other row whose middle is above the pointer.
    int to = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        const QWidget* other = m_rows.at(i);
        if (i != from && globalY > other->mapToGlobal(QPoint(0, other->height() / 2)).y())
            ++to;
    }
    if (to != from)
        moveOption(from, to);
}

void PollDialog::relayoutOptions()
{
    while (m_optionsLayout->count() > 0)
        m_optionsLayout->takeAt(0);
    for (int i = 0; i < m_rows.size(); ++i) {
        PollOptionRow* row = m_rows.at(i);
        m_optionsLayout->addWidget(row);
        row->field()->setPlaceholderText(i18n::t("Option %1").arg(i + 1));
        row->field()->setAccessibleName(i18n::t("Option %1").arg(i + 1));
        row->remove()->setAccessibleName(i18n::t("Remove option %1").arg(i + 1)); // which one, for screen readers
        row->remove()->setEnabled(m_rows.size() > polls::kMinOptions);
        row->show();
        if (m_question)
            row->ensurePolished(); // 2.3 ux review: its style sheet sizes before fitHeight() (also while hidden)
    }
    // Tab order: question, the options in order (each with its remove button), add, switch, buttons.
    QWidget* previous = m_question;
    for (PollOptionRow* row : qAsConst(m_rows)) {
        setTabOrder(previous, row->field());
        setTabOrder(row->field(), row->remove());
        previous = row->remove();
    }
    setTabOrder(previous, m_add);
    setTabOrder(m_add, m_multiple);
    setTabOrder(m_multiple, m_cancel);
    setTabOrder(m_cancel, m_post);
    updateCounters();
    fitHeight();
}

void PollDialog::updateCounters()
{
    const int used = m_question->text().size();
    m_questionCount->setText(counterText(used, polls::kMaxQuestionChars));
    const int left = polls::kMaxOptions - m_rows.size();
    m_add->setVisible(left > 0);
    m_addHint->setText(left > 0 ? (left == 1 ? i18n::t("You can add 1 more option.") : i18n::t("You can add %1 more options.").arg(left))
                                : i18n::t("That's the most options a poll can have."));
}

void PollDialog::showError(const QString& text, QWidget* field, QLabel* where)
{
    clearErrors();
    QLabel* target = where ? where : m_error;
    target->setText(text);
    target->setAccessibleDescription(text);
    target->show();
    if (field)
        field->setFocus(Qt::OtherFocusReason);
    fitHeight();
}

void PollDialog::fitHeight()
{
    adjustSize();
    // 2.3 ux review: adjustSize() keeps a window within 2/3 of the screen's height, and then at its minimum
    // height, which squeezes the fields (a 1080p screen at 150 % fits a poll of three options only squeezed):
    // as tall as its content at this width, as long as the screen has room.
    int want = !layout() ? height() : layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width()) : layout()->totalSizeHint().height();
    if (const QScreen* screen = this->screen())
        want = qMin(want, screen->availableGeometry().height() - 48); // (the title bar)
    if (height() < want)
        resize(width(), want);
}

void PollDialog::clearErrors()
{
    for (QLabel* label : {m_error, m_questionError, m_optionsError}) {
        if (label && !label->isHidden())
            label->hide();
    }
}

PollDialog::Result PollDialog::result() const
{
    Result r;
    r.question = polls::cleanLine(m_question->text(), polls::kMaxQuestionChars);
    for (PollOptionRow* row : m_rows) {
        const QString option = polls::cleanLine(row->field()->text(), polls::kMaxOptionChars);
        if (!option.isEmpty())
            r.options << option;
    }
    r.multiple = m_multiple->isChecked();
    return r;
}

bool PollDialog::tryPost()
{
    const Result r = result();
    if (r.question.isEmpty()) {
        showError(i18n::t("Ask a question first."), m_question, m_questionError);
        return false;
    }
    if (r.options.size() < polls::kMinOptions) {
        QWidget* empty = nullptr;
        for (PollOptionRow* row : qAsConst(m_rows)) {
            if (!empty && polls::cleanLine(row->field()->text(), polls::kMaxOptionChars).isEmpty())
                empty = row->field();
        }
        showError(i18n::t("A poll needs at least two options."), empty, m_optionsError);
        return false;
    }
    QSet<QString> seen;
    for (int i = 0; i < r.options.size(); ++i) {
        const QString folded = r.options.at(i).toCaseFolded();
        if (seen.contains(folded)) {
            QWidget* field = nullptr;
            for (PollOptionRow* row : qAsConst(m_rows)) {
                if (polls::cleanLine(row->field()->text(), polls::kMaxOptionChars).toCaseFolded() == folded)
                    field = row->field();
            }
            showError(i18n::t("Each option must be different."), field, m_optionsError);
            return false;
        }
        seen.insert(folded);
    }
    if (m_validator) {
        const QString problem = m_validator(r);
        if (!problem.isEmpty()) {
            showError(problem, m_question, nullptr);
            return false;
        }
    }
    accept();
    return true;
}

bool PollDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress)
        return QDialog::eventFilter(watched, event);
    auto*     key   = static_cast<QKeyEvent*>(event);
    const int index = indexOf(watched);
    const bool ctrl = key->modifiers() & Qt::ControlModifier;
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && ctrl) {
        tryPost();
        return true;
    }
    if (watched == m_question && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
        if (!m_rows.isEmpty())
            m_rows.first()->field()->setFocus(Qt::TabFocusReason);
        return true;
    }
    if (index < 0)
        return QDialog::eventFilter(watched, event);
    if (key->modifiers() & Qt::AltModifier) {
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            const int to = index + (key->key() == Qt::Key_Up ? -1 : 1);
            moveOption(index, to);
            if (to >= 0 && to < m_rows.size())
                m_rows.at(to)->field()->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        if (index + 1 < m_rows.size())
            m_rows.at(index + 1)->field()->setFocus(Qt::TabFocusReason);
        else if (m_rows.size() < polls::kMaxOptions)
            addOption();
        else
            m_post->setFocus(Qt::TabFocusReason);
        return true;
    }
    if (key->key() == Qt::Key_Backspace && m_rows.at(index)->field()->text().isEmpty() && m_rows.size() > polls::kMinOptions) {
        removeOption(index);
        if (index > 0)
            m_rows.at(index - 1)->field()->setFocus(Qt::OtherFocusReason);
        return true;
    }
    if (key->key() == Qt::Key_Down && index + 1 < m_rows.size()) {
        m_rows.at(index + 1)->field()->setFocus(Qt::TabFocusReason);
        return true;
    }
    if (key->key() == Qt::Key_Up && index > 0) {
        m_rows.at(index - 1)->field()->setFocus(Qt::BacktabFocusReason);
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void PollDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (layout())
        layout()->activate(); // the sizes it is shown with (TeamSpeak's skin and ours applied)
    fitHeight();
}

void PollDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) && m_lock) // once the window is built
        applyTheme();
}

void PollDialog::applyTheme()
{
    if (m_applyingTheme)
        return;
    m_applyingTheme = true;
    // 2.3 ux review: TeamSpeak's skins colour windows by style sheet ("QWidget, QDialog { background: ... }"),
    // not by palette: the palette is the skin's only once polished. Read before (the application's light
    // palette), the dark skin's window was pinned light by the rule below, with the skin's dark behind the options.
    ensurePolished();
    const Tokens t = tokensFor(palette());
    m_dark         = t.dark;
    // fromLatin1, never QStringLiteral: TeamSpeak's style keeps parsed style sheet text after the plugin
    // is unloaded. Scoped to this window: it never reaches TeamSpeak's own widgets.
    const QString sheet =
        QString::fromLatin1(
            "#tsmediaPollDialog{background:%1;}"
            "#tsmediaPollDialog QLabel{color:%2;background:transparent;}"
            "#tsmediaPollDialog QLabel[role=\"muted\"]{color:%3;}"
            "#tsmediaPollDialog QLabel[role=\"section\"]{color:%3;font-weight:bold;}"
            "#tsmediaPollDialog QLabel[role=\"error\"]{color:%4;}"
            "#tsmediaPollDialog QLineEdit{background:%5;color:%2;border:1px solid %6;border-radius:8px;padding:7px 10px;"
            "selection-background-color:%7;selection-color:#ffffff;}"
            "#tsmediaPollDialog QLineEdit:hover{border:1px solid %3;}"
            "#tsmediaPollDialog QLineEdit:focus{border:1px solid %7;}"
            "#tsmediaPollDialog QPushButton{background:transparent;color:%2;border:1px solid %6;border-radius:8px;padding:7px 18px;min-height:18px;}"
            "#tsmediaPollDialog QPushButton:hover{background:%8;}"
            "#tsmediaPollDialog QPushButton:focus{border:2px solid %9;padding:6px 17px;}"
            "#tsmediaPollDialog QPushButton[role=\"primary\"]{background:%7;color:#ffffff;border:1px solid %7;padding:7px 20px;font-weight:bold;}"
            "#tsmediaPollDialog QPushButton[role=\"primary\"]:hover{background:%10;border-color:%10;}"
            "#tsmediaPollDialog QPushButton[role=\"primary\"]:focus{border:2px solid %9;padding:6px 19px;}"
            "#tsmediaPollDialog QLabel[role=\"error\"]{padding-top:4px;}"
            "#tsmediaPollDialog QPushButton[role=\"link\"]{background:transparent;color:%9;border:none;padding:6px 6px;font-weight:bold;text-align:left;}"
            "#tsmediaPollDialog QPushButton[role=\"link\"]:hover{background:%8;}"
            "#tsmediaPollDialog QPushButton[role=\"link\"]:focus{border:2px solid %9;padding:4px 4px;}")
            .arg(t.bg.name(), t.text.name(), t.muted.name(), t.danger.name(), t.surface.name(), t.border.name(), t.accent.name(), t.hover.name(),
                 t.accentText.name())
            .arg(t.accentHover.name());
    if (styleSheet() != sheet)
        setStyleSheet(sheet);
    // Placeholders in the muted colour (style sheets can't reach them in Qt 5).
    QList<QLineEdit*> fields = optionFields();
    fields << m_question;
    for (QLineEdit* field : qAsConst(fields)) {
        QPalette pal = field->palette();
        pal.setColor(QPalette::PlaceholderText, t.muted);
        field->setPalette(pal);
    }
    // The lock before the anonymity line.
    const qreal dpr = devicePixelRatioF();
    QPixmap     lock(QSize(18, 18) * dpr);
    lock.setDevicePixelRatio(dpr);
    lock.fill(Qt::transparent);
    {
        QPainter p(&lock);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(t.muted, 1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(5.5, 2.5, 7, 8), 0, 180 * 16);
        p.drawLine(QPointF(5.5, 6.5), QPointF(5.5, 8.5));
        p.drawLine(QPointF(12.5, 6.5), QPointF(12.5, 8.5));
        p.setPen(Qt::NoPen);
        p.setBrush(t.muted);
        p.drawRoundedRect(QRectF(3.5, 8, 11, 8), 2, 2);
    }
    m_lock->setPixmap(lock);
    m_applyingTheme = false;
}
