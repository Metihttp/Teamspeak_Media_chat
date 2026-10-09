#include "accessgroupbox.h"

#include <QAccessible>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

#include "accessgroup.h"
#include "i18n.h"

namespace {

// Secondary text, as the settings dialog's hint(): plain text, wrapped, coloured by the dialog's theme.
QLabel* roleLabel(const char* role, QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText); // server-supplied names are shown literally
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    if (role)
        label->setProperty("role", QString::fromLatin1(role));
    QSizePolicy policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    label->setSizePolicy(policy);
    return label;
}

// One line, elided in the middle (server names can be long), the full name in the tooltip.
class ElidedLabel : public QLabel
{
  public:
    explicit ElidedLabel(QWidget* parent)
        : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        // Expanding: a form layout grows only such fields; sizeHint() and minimumSizeHint() keep it elastic.
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    void setFullText(const QString& text)
    {
        if (text == m_full && !m_full.isNull())
            return;
        m_full = text;
        // Tooltips are rich text when they look like it: escape, and build the wrapper at run time.
        setToolTip(text.isEmpty() ? QString() : QString::fromLatin1("<p style='white-space:pre'>%1</p>").arg(text.toHtmlEscaped()));
        setAccessibleName(text);
        elide();
        updateGeometry();
    }

    QSize sizeHint() const override
    {
        QSize size = QLabel::sizeHint();
        size.setWidth(qMin(fontMetrics().horizontalAdvance(m_full) + 4, fontMetrics().averageCharWidth() * 40));
        return size;
    }

    QSize minimumSizeHint() const override
    {
        QSize size = QLabel::minimumSizeHint();
        size.setWidth(fontMetrics().averageCharWidth() * 8);
        return size;
    }

  protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        elide();
    }

  private:
    void elide()
    {
        const QString shown = fontMetrics().elidedText(m_full, Qt::ElideMiddle, qMax(0, contentsRect().width()));
        if (shown != text())
            setText(shown);
    }

    QString m_full;
};

QPixmap groupPixmap(qreal dpr)
{
    // The 32 px picture for HiDPI, scaled down smoothly for in-between factors (1.25, 1.5).
    const QImage source = QImage::fromData(dpr > 1.01 ? access::groupIcon32() : access::groupIcon16());
    const int    side   = qRound(16 * dpr);
    QPixmap      pixmap = QPixmap::fromImage(source.width() == side ? source : source.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

} // namespace

AccessGroupBox::AccessGroupBox(QWidget* parent)
    : QGroupBox(i18n::t("Server access"), parent)
    , m_access(AccessGroup::instance())
{
    m_intro = roleLabel("hint", this);
    m_intro->setText(access::introText());

    m_serverRow   = new QWidget(this);
    m_server      = new ElidedLabel(m_serverRow);
    auto* serverForm = new QFormLayout(m_serverRow);
    serverForm->setContentsMargins(0, 0, 0, 0);
    serverForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    serverForm->setLabelAlignment(Qt::AlignLeading | Qt::AlignVCenter);
    serverForm->addRow(i18n::t("Server"), m_server);

    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    m_statusIcon       = new QLabel(this);
    m_statusIcon->setFixedSize(iconSize, iconSize);
    m_statusIcon->setAlignment(Qt::AlignCenter);
    m_statusIcon->hide();
    m_status = roleLabel(nullptr, this);
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(6);
    statusRow->addWidget(m_statusIcon, 0, Qt::AlignTop);
    statusRow->addWidget(m_status, 1);

    m_detail      = roleLabel("hint", this);
    m_detailError = roleLabel("error", this);
    m_extra       = roleLabel("hint", this);
    m_detailError->hide();
    m_extra->hide();

    m_buttons = new QWidget(this);
    // The free access key is h ("c&hat"); every letter of "Details" is taken in the dialog.
    m_primary    = new QPushButton(m_buttons);
    m_details    = new QPushButton(i18n::t("Details…"), m_buttons);
    m_checkAgain = new QPushButton(i18n::t("Check again"), m_buttons);
    m_checkAgain->setFlat(true);
    m_checkAgain->setAccessibleName(i18n::t("Check the server again"));
    for (QPushButton* button : {m_primary, m_details, m_checkAgain})
        button->setAutoDefault(false); // Enter stays OK
    auto* buttonRow = new QHBoxLayout(m_buttons);
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addWidget(m_primary);
    buttonRow->addWidget(m_details);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_checkAgain);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_intro);
    layout->addWidget(m_serverRow);
    layout->addLayout(statusRow);
    layout->addWidget(m_detail);
    layout->addWidget(m_detailError);
    layout->addWidget(m_extra);
    layout->addWidget(m_buttons);

    connect(m_primary, &QPushButton::clicked, this, [this] { runPrimary(); });
    connect(m_details, &QPushButton::clicked, this, [this] { showDetails(); });
    connect(m_checkAgain, &QPushButton::clicked, this, [this] {
        if (m_access)
            m_access->refresh(m_sch, true);
    });
    if (m_access)
        connect(m_access.data(), &AccessGroup::changed, this, [this] { refreshView(); });
    refreshView();
}

AccessGroupBox::~AccessGroupBox()
{
    if (m_watching && m_access)
        m_access->setWatching(false);
}

void AccessGroupBox::showPreview(const access::ViewInput& view, const QString& server)
{
    m_preview = true;
    render(access::buildView(view), server, view.state != access::State::NotConnected);
}

void AccessGroupBox::showEvent(QShowEvent* event)
{
    QGroupBox::showEvent(event);
    if (!m_watching && m_access && !m_preview) {
        m_watching = true;
        m_access->setWatching(true); // checks the current server
    }
    refreshView();
}

void AccessGroupBox::hideEvent(QHideEvent* event)
{
    QGroupBox::hideEvent(event);
    if (m_watching && m_access && !event->spontaneous()) { // not when the dialog is merely minimized
        m_watching = false;
        m_access->setWatching(false);
    }
}

void AccessGroupBox::changeEvent(QEvent* event)
{
    QGroupBox::changeEvent(event);
    if (event->type() == QEvent::StyleChange)
        setStatusIcon(m_icon); // the warning icon comes from the style
}

void AccessGroupBox::refreshView()
{
    if (m_preview)
        return;
    if (!m_access) {
        render(access::buildView(access::ViewInput()), QString(), false);
        return;
    }
    m_sch                        = m_access->displayConnection();
    const access::ViewInput view = m_access->view(m_sch);
    render(access::buildView(view), m_access->serverName(m_sch), view.state != access::State::NotConnected);
}

void AccessGroupBox::render(const access::View& view, const QString& server, bool connected)
{
    const QSize before = sizeHint();

    m_intro->setVisible(view.showIntro);
    m_serverRow->setVisible(view.showServer);
    static_cast<ElidedLabel*>(m_server)->setFullText(connected && !server.isEmpty() ? server : access::notConnectedServerText());

    if (m_status->text() != view.status) {
        m_status->setText(view.status);
        // Screen readers hear the new state without the focus moving.
        QAccessibleEvent changed(m_status, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&changed);
    }
    setStatusIcon(view.icon);

    QLabel* shown  = view.detailIsError ? m_detailError : m_detail;
    QLabel* hidden = view.detailIsError ? m_detail : m_detailError;
    shown->setText(view.detail);
    shown->setVisible(!view.detail.isEmpty());
    hidden->clear();
    hidden->hide();
    m_extra->setText(view.extra);
    m_extra->setVisible(!view.extra.isEmpty());

    m_buttons->setVisible(view.showButtons);
    m_action = view.primary;
    m_primary->setVisible(!view.primaryText.isEmpty());
    m_primary->setText(view.primaryText);
    m_primary->setEnabled(view.primaryEnabled);
    m_details->setVisible(view.showDetails);
    m_checkAgain->setVisible(view.showCheckAgain);
    m_checkAgain->setEnabled(connected);
    setAccessibleDescription(view.status);

    if (sizeHint() != before)
        emit contentsChanged();
}

void AccessGroupBox::setStatusIcon(access::View::Icon icon)
{
    m_icon = icon;
    if (icon == access::View::Icon::None) {
        m_statusIcon->clear();
        m_statusIcon->hide();
        return;
    }
    const int    side = m_statusIcon->width();
    const qreal  dpr  = devicePixelRatioF();
    QPixmap      pixmap;
    if (icon == access::View::Icon::Group) {
        pixmap = groupPixmap(dpr);
        m_statusIcon->setAccessibleName(QString()); // decorative: the text says it
    } else {
        pixmap = style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(QSize(side, side));
        m_statusIcon->setAccessibleName(i18n::t("Warning"));
    }
    m_statusIcon->setPixmap(pixmap);
    m_statusIcon->show();
}

void AccessGroupBox::runPrimary()
{
    if (!m_access || m_action == access::Action::None)
        return;
    m_access->run(m_sch, m_action);
}

void AccessGroupBox::showDetails()
{
    if (!m_access)
        return;
    openDetails(m_access->details(m_sch));
}

// A window of its own (progressive disclosure keeps the dialog's height stable). Non-modal; its
// objectName starts with "tsmedia", so plugin shutdown closes it.
void AccessGroupBox::openDetails(const access::Details& details)
{
    if (m_detailsWindow)
        m_detailsWindow->close();
    auto* window = new QDialog(this->window());
    window->setObjectName(QString::fromLatin1("tsmediaAccessDetails"));
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowFlags(window->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    window->setLayoutDirection(Qt::LeftToRight);
    window->setWindowTitle(details.title);

    auto* text = roleLabel(nullptr, window);
    text->setText(details.text);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* informative = roleLabel(nullptr, window);
    informative->setText(details.informative);
    informative->setTextInteractionFlags(Qt::TextSelectableByMouse);
    informative->setVisible(!details.informative.isEmpty());

    auto* list = new QPlainTextEdit(window);
    list->setReadOnly(true);
    list->setPlainText(details.detailed);
    list->setLineWrapMode(QPlainTextEdit::NoWrap);
    list->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    list->setAccessibleName(i18n::t("Permissions"));
    list->setVisible(!details.detailed.isEmpty());
    list->setMinimumSize(list->fontMetrics().averageCharWidth() * 64, list->fontMetrics().lineSpacing() * 12);

    auto* buttons = new QDialogButtonBox(window);
    QPushButton* close = buttons->addButton(i18n::t("Close"), QDialogButtonBox::RejectRole);
    close->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, window, &QDialog::close);

    auto* layout = new QVBoxLayout(window);
    layout->addWidget(text);
    layout->addWidget(informative);
    layout->addWidget(list, 1);
    layout->addWidget(buttons);

    m_detailsWindow = window;
    window->show();
    window->raise();
    window->activateWindow();
}
