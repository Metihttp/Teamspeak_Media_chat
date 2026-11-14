#include "proxywindow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>

#include "findrun.h"
#include "i18n.h"
#include "linkparse.h"
#include "settingssection.h"
#include "settingstheme.h"
#include "uiutil.h"
#include "windowsproxy.h"

namespace proxy::ui {

namespace {

QPointer<ProxyWindow> g_window; // the one window

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

QLabel* hintLabel(const QString& text, QWidget* parent)
{
    return SettingsSection::hint(text, parent); // the Settings dialog's hint: plain text, wraps, role "hint"
}

bool sameEndpoint(const Config& a, const Config& b)
{
    return a.type == b.type && a.host == b.host && a.port == b.port && a.user == b.user;
}

} // namespace

QWidget* showProxyWindow(QWidget* parent, Request request, Control* control)
{
    if (!control)
        control = Control::current();
    if (!control)
        return nullptr;
    if (g_window) {
        if (request == Request::UseIt)
            g_window->prepare(request);
        g_window->raise();
        g_window->activateWindow();
        return g_window;
    }
    auto* window = new ProxyWindow(control, parent);
    window->setAttribute(Qt::WA_DeleteOnClose);
    // From the Settings dialog: window-modal to it (its own windows stay usable elsewhere).
    if (parent && parent->objectName() == QLatin1String("tsmediaSettingsDialog"))
        window->setWindowModality(Qt::WindowModal);
    window->prepare(request);
    g_window = window;
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

ProxyWindow::ProxyWindow(Control* control, QWidget* parent, const Jobs& jobs)
    : QDialog(parent)
    , m_control(control)
    , m_jobs(jobs)
{
    setObjectName(latin("tsmediaProxyDialog"));
    setWindowTitle(i18n::t("Chat proxy"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    if (!m_jobs.test) {
        // The real jobs: one at a time each; their results come through their own posting links.
        m_testRun = new TestRun(this);
        m_findRun = new FindRun(this);
        m_jobs.test = [this](const Config& form, protect::SecretBytes typed, const QByteArray& blob, std::function<void(const TestResult&)> done) {
            if (!m_testRun->start(form, std::move(typed), blob))
                return false;
            QObject::disconnect(m_testRun, nullptr, this, nullptr);
            connect(m_testRun, &TestRun::finished, this, [this, done] { done(m_testRun->result()); });
            return true;
        };
        m_jobs.find = [this](quint16 bridgePort, std::function<void(const QVector<quint16>&)> done) {
            delete m_findRun;
            FindRun::Options options;
            options.bridgePort = bridgePort;
            m_findRun          = new FindRun(options, this);
            connect(m_findRun, &FindRun::finished, this, [this, done] { done(m_findRun->found()); });
            return m_findRun->start();
        };
        m_jobs.cancel = [this] {
            if (m_testRun)
                m_testRun->cancel();
            if (m_findRun)
                m_findRun->cancel();
        };
    }

    auto* intro = hintLabel(i18n::t("For TS Media's own web traffic: link previews, the YouTube player and update checks. TeamSpeak itself (servers, "
                                    "voice, chat, file transfers) never uses it, so servers in your country stay reachable while previews go "
                                    "through a VPN app."),
                            this);
    m_use = new QCheckBox(i18n::t("&Use a chat proxy for link previews, the YouTube player and update checks"), this);
    m_use->setAccessibleDescription(intro->text());

    // The form, lined up with the checkbox's text.
    auto* form = new QWidget(this);
    m_type     = new QComboBox(form);
    m_type->addItem(i18n::t("SOCKS5"));
    m_type->addItem(i18n::t("HTTP"));
    m_type->setAccessibleName(i18n::t("Proxy type"));
    m_server = new QLineEdit(form);
    m_server->setPlaceholderText(latin("127.0.0.1"));
    m_server->setMaxLength(kMaxLinkChars);
    m_port = new QLineEdit(form);
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    m_port->setAccessibleName(i18n::t("Port"));
    m_port->setAccessibleDescription(i18n::t("Port"));
    m_port->setFixedWidth(m_port->fontMetrics().averageCharWidth() * 7 + 2 * style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, m_port) + 8);
    m_user = new QLineEdit(form);
    m_user->setPlaceholderText(i18n::t("optional"));
    m_password = new QLineEdit(form);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(i18n::t("optional"));

    auto* typeLabel     = new QLabel(i18n::t("T&ype:"), form);
    auto* serverLabel   = new QLabel(i18n::t("Ser&ver:"), form);
    auto* portLabel     = new QLabel(i18n::t("P&ort:"), form);
    auto* userLabel     = new QLabel(i18n::t("Us&er name:"), form);
    auto* passwordLabel = new QLabel(i18n::t("Pa&ssword:"), form);
    typeLabel->setBuddy(m_type);
    serverLabel->setBuddy(m_server);
    portLabel->setBuddy(m_port);
    userLabel->setBuddy(m_user);
    passwordLabel->setBuddy(m_password);

    const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    const auto errorRow = [this, iconSize](QWidget* field, QWidget* parent) {
        ErrorRow e;
        e.field = field;
        e.row   = new QWidget(parent);
        e.icon  = new QLabel(e.row);
        e.icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(iconSize, iconSize));
        e.icon->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        e.text = new QLabel(e.row);
        e.text->setTextFormat(Qt::PlainText);
        e.text->setWordWrap(true);
        e.text->setProperty("role", latin("error"));
        auto* layout = new QHBoxLayout(e.row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(e.icon, 0, Qt::AlignTop);
        layout->addWidget(e.text, 1);
        e.row->hide();
        m_errors.append(e);
        return e.row;
    };

    auto* grid = new QGridLayout(form);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    grid->addWidget(typeLabel, 0, 0);
    grid->addWidget(m_type, 0, 1, Qt::AlignLeft);
    grid->addWidget(serverLabel, 1, 0);
    grid->addWidget(m_server, 1, 1);
    grid->addWidget(portLabel, 1, 2);
    grid->addWidget(m_port, 1, 3, Qt::AlignLeft);
    grid->addWidget(errorRow(m_server, form), 2, 1);
    grid->addWidget(errorRow(m_port, form), 2, 3);
    grid->addWidget(userLabel, 3, 0);
    grid->addWidget(m_user, 3, 1);
    grid->addWidget(passwordLabel, 3, 2);
    grid->addWidget(m_password, 3, 3);
    grid->addWidget(errorRow(m_user, form), 4, 1);
    grid->addWidget(errorRow(m_password, form), 4, 3);

    m_portsHint = hintLabel(i18n::t("Your VPN app shows its local port: v2rayN 10808, Throne and Nekoray 2080, Clash Verge Rev 7897, Hiddify 12334 "
                                    "(SOCKS5 and HTTP both work there). Find my VPN app tries these on this PC. The proxy looks up website names "
                                    "itself, so filtered addresses on your network don't matter."),
                            form);
    m_lanHint     = hintLabel(i18n::t("The user name and password travel unencrypted to a proxy on another computer."), form);
    m_droppedHint = hintLabel(i18n::t("The saved password will be removed because the server changed. Type it again if this proxy needs it."), form);
    m_lanHint->hide();
    m_droppedHint->hide();
    grid->addWidget(m_droppedHint, 5, 0, 1, 4);
    grid->addWidget(m_portsHint, 6, 0, 1, 4);
    grid->addWidget(m_lanHint, 7, 0, 1, 4);
    // The fields' hints are their accessible descriptions (the PrivacySection pattern).
    for (ErrorRow& e : m_errors)
        e.hint = m_portsHint->text();
    m_errors[2].hint = m_lanHint->text(); // user name
    m_errors[3].hint = m_lanHint->text(); // password
    for (const ErrorRow& e : qAsConst(m_errors))
        e.field->setAccessibleDescription(e.hint);
    m_type->setAccessibleDescription(m_portsHint->text());

    m_find  = new QPushButton(i18n::t("&Find my VPN app"), form);
    m_paste = new QPushButton(i18n::t("&Paste from clipboard"), form);
    m_test  = new QPushButton(i18n::t("&Test"), form);
    m_find->setAccessibleDescription(i18n::t("Looks for a VPN app's proxy on this PC at its usual ports. Nothing leaves this PC."));
    m_test->setAccessibleDescription(i18n::t("Sends one request to www.youtube.com through this proxy."));
    m_paste->setAccessibleDescription(i18n::t("Fills in the fields from a proxy link you copied, such as socks5://127.0.0.1:10808."));
    for (QPushButton* b : {m_find, m_paste, m_test})
        b->setAutoDefault(false);
    auto* jobRow = new QHBoxLayout;
    jobRow->setContentsMargins(0, 0, 0, 0);
    jobRow->addWidget(m_find);
    jobRow->addWidget(m_paste);
    jobRow->addWidget(m_test);
    jobRow->addStretch(1);
    grid->addLayout(jobRow, 8, 0, 1, 4);

    m_resultRow  = new QWidget(form);
    m_resultIcon = new QLabel(m_resultRow);
    m_resultIcon->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_resultText = new QLabel(m_resultRow);
    m_resultText->setTextFormat(Qt::PlainText);
    m_resultText->setWordWrap(true);
    auto* resultLayout = new QHBoxLayout(m_resultRow);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    resultLayout->addWidget(m_resultIcon, 0, Qt::AlignTop);
    resultLayout->addWidget(m_resultText, 1);
    m_resultRow->hide();
    grid->addWidget(m_resultRow, 9, 0, 1, 4);

    // When the chat proxy is off.
    auto* whenOffLabel = new QLabel(i18n::t("When the chat proxy is off:"), this);
    m_system           = new QRadioButton(i18n::t("Use &Windows proxy settings"), this);
    auto* systemHint   = hintLabel(i18n::t("The proxy Windows is set to use, if any. A VPN app's \"system proxy\" option sets one."), this);
    m_direct           = new QRadioButton(i18n::t("No proxy (&direct)"), this);
    m_system->setAccessibleDescription(systemHint->text());
    m_direct->setAccessibleDescription(i18n::t("TS Media connects directly. Sites your network blocks won't load."));
    m_whenOff = new QButtonGroup(this);
    m_whenOff->setExclusive(true);
    m_whenOff->addButton(m_system);
    m_whenOff->addButton(m_direct);

    // The status row: what is in use now.
    auto* statusRow = new QWidget(this);
    m_statusIcon    = new QLabel(statusRow);
    m_statusIcon->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_statusText = new QLabel(statusRow);
    m_statusText->setTextFormat(Qt::PlainText);
    m_statusText->setWordWrap(true);
    m_useIt = new QPushButton(i18n::t("Use &it as the chat proxy"), statusRow);
    m_useIt->setFlat(true);
    m_useIt->setAutoDefault(false);
    m_useIt->setCursor(Qt::PointingHandCursor);
    m_useIt->hide();
    auto* statusLayout = new QHBoxLayout(statusRow);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->addWidget(m_statusIcon, 0, Qt::AlignTop);
    statusLayout->addWidget(m_statusText, 1);
    statusLayout->addWidget(m_useIt, 0, Qt::AlignVCenter);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_use);
    layout->addWidget(form);
    layout->addSpacing(4);
    layout->addWidget(whenOffLabel);
    layout->addWidget(m_system);
    layout->addWidget(systemHint);
    layout->addWidget(m_direct);
    layout->addSpacing(4);
    layout->addWidget(statusRow);
    layout->addWidget(m_buttons);
    // Lined up with the checkbox's / radio's text.
    const int indent = m_use->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, m_use) + m_use->style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, m_use);
    form->setContentsMargins(indent, 0, 0, 0);
    systemHint->setContentsMargins(indent, 0, 0, 0);

    // Tab order (1.3.2): checkbox, type, server, port, user name, password, Find, Paste, Test, the checked radio, the
    // status action, OK, Cancel.
    QWidget* const order[] = {m_use, m_type, m_server, m_port, m_user, m_password, m_find, m_paste, m_test, m_system, m_direct, m_useIt,
                              m_buttons->button(QDialogButtonBox::Ok), m_buttons->button(QDialogButtonBox::Cancel)};
    for (size_t i = 0; i + 1 < sizeof(order) / sizeof(order[0]); ++i)
        setTabOrder(order[i], order[i + 1]);

    // Signals.
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] { accept_(); });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &ProxyWindow::reject);
    connect(m_use, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading)
            return;
        m_checkTouched = true;
        if (on && m_server->text().trimmed().isEmpty()) {
            m_server->setText(latin("127.0.0.1")); // no ":" jump (F19): the port gets the focus
            m_port->setFocus();
        }
        if (!on)
            clearErrors();
        updateLanHint();
    });
    connect(m_whenOff, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked), this, [this] { m_radioTouched = true; });
    connect(m_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { touched(); });
    for (QLineEdit* field : {m_server, m_port, m_user, m_password})
        connect(field, &QLineEdit::textEdited, this, [this] { touched(); });
    // A field left: its own check (the server field reads a link or host:port first).
    connect(m_server, &QLineEdit::editingFinished, this, [this] {
        if (m_loading || !isVisible())
            return;
        if (!serverLink() && !m_server->text().trimmed().isEmpty())
            validateServer(true);
    });
    connect(m_port, &QLineEdit::editingFinished, this, [this] {
        if (!m_loading && isVisible() && !m_port->text().isEmpty())
            validatePort(true);
    });
    connect(m_user, &QLineEdit::editingFinished, this, [this] {
        if (!m_loading && isVisible())
            validateLogin();
    });
    connect(m_find, &QPushButton::clicked, this, [this] { startFind(); });
    connect(m_paste, &QPushButton::clicked, this, [this] { paste(); });
    connect(m_test, &QPushButton::clicked, this, [this] { startTest(); });
    connect(m_useIt, &QPushButton::clicked, this, [this] { useIt(); });
    if (m_control) {
        connect(m_control.data(), &Control::routeChanged, this, [this] { routeChanged(); });
        connect(m_control.data(), &Control::healthChanged, this, [this] { refreshStatus(); });
    }

    loadForm(m_control ? m_control->config() : Config());
    m_built = true; // from now on a skin's StyleChange / PaletteChange may repaint (live test fix: see changeEvent)
    refreshStatus();
    applyTheme();
}

ProxyWindow::~ProxyWindow()
{
    cancelJobs(); // never joins: the workers end by themselves (proxy::shutdown() joins them, bounded)
    m_password->clear();
}

void ProxyWindow::prepare(Request request)
{
    if (request == Request::SetUp) {
        m_loading = true;
        m_use->setChecked(true);
        if (m_server->text().trimmed().isEmpty())
            m_server->setText(latin("127.0.0.1"));
        m_loading      = false;
        m_checkTouched = true;
        updatePasswordHints();
        m_find->setFocus();
    } else if (request == Request::UseIt) {
        useIt();
    } else {
        m_use->setFocus();
    }
}

void ProxyWindow::loadForm(const Config& saved)
{
    m_loading = true;
    m_saved   = saved;
    m_use->setChecked(saved.on);
    m_type->setCurrentIndex(saved.type == Type::Http ? 1 : 0);
    m_server->setText(saved.host);
    m_port->setText(saved.port ? QString::number(saved.port) : QString());
    m_user->setText(saved.user);
    m_password->clear();
    (saved.whenOff == WhenOff::Direct ? m_direct : m_system)->setChecked(true);
    m_loading = false;
    updatePasswordHints();
    updateLanHint();
}

Config ProxyWindow::formConfig() const
{
    Config c   = m_saved;
    c.on       = m_use->isChecked();
    c.whenOff  = m_direct->isChecked() ? WhenOff::Direct : WhenOff::System;
    c.type     = m_type->currentIndex() == 1 ? Type::Http : Type::Socks5;
    c.host     = normalizeHost(m_server->text());
    bool ok    = false;
    const uint port = m_port->text().trimmed().toUInt(&ok);
    c.port     = ok && port >= 1 && port <= 65535 ? static_cast<quint16>(port) : 0;
    c.user     = m_user->text();
    return c;
}

Endpoint ProxyWindow::formEndpoint() const
{
    return formConfig().endpoint();
}

bool ProxyWindow::endpointChanged() const
{
    return !sameEndpoint(formConfig(), m_saved);
}

void ProxyWindow::updatePasswordHints()
{
    // The saved password follows its endpoint (S7): another type, server, port or user name without a typed password
    // drops it on OK; changing them back restores both the placeholder and the hint.
    const bool saved   = !m_saved.passwordBlob.isEmpty();
    const bool dropped = saved && m_password->text().isEmpty() && endpointChanged();
    m_password->setPlaceholderText(saved && !dropped ? i18n::t("saved (type to replace)") : i18n::t("optional"));
    m_droppedHint->setVisible(dropped);
}

void ProxyWindow::updateLanHint()
{
    const Config c = formConfig();
    m_lanHint->setVisible(!c.host.isEmpty() && placeOf(c.host) != Place::ThisPc && !m_user->text().isEmpty());
}

void ProxyWindow::touched()
{
    if (m_loading)
        return;
    cancelJobs(); // an edit cancels a running Test or Find (its result is dropped)
    updatePasswordHints();
    updateLanHint();
}

// ---- validation (1.3.3) ---------------------------------------------------------------------------------------------

void ProxyWindow::showError(QWidget* field, const QString& text)
{
    for (ErrorRow& e : m_errors) {
        if (e.field != field)
            continue;
        e.text->setText(text);
        e.row->setVisible(!text.isEmpty());
        field->setAccessibleDescription(text.isEmpty() ? e.hint : text);
        // The focus goes to the first field with a problem once (validate): moving it here for every message would make
        // the field left check itself again with its own rule (editingFinished).
    }
}

void ProxyWindow::clearErrors()
{
    for (ErrorRow& e : m_errors) {
        e.text->clear();
        e.row->hide();
        e.field->setAccessibleDescription(e.hint);
    }
}

QString ProxyWindow::errorOf(QWidget* field) const
{
    for (const ErrorRow& e : m_errors) {
        if (e.field == field)
            return e.row->isVisibleTo(this) ? e.text->text() : QString();
    }
    return {};
}

bool ProxyWindow::errorIconShown(QWidget* field) const
{
    for (const ErrorRow& e : m_errors) {
        if (e.field == field)
            return e.row->isVisibleTo(this) && !e.icon->pixmap(Qt::ReturnByValue).isNull();
    }
    return false;
}

bool ProxyWindow::validateServer(bool required)
{
    const QString text = m_server->text().trimmed();
    if (text.isEmpty()) {
        showError(m_server, required ? i18n::t("Enter the proxy's address, for example 127.0.0.1.") : QString());
        return !required;
    }
    if (hostError(text) != HostError::None) {
        showError(m_server, i18n::t("Enter only the proxy's address, like 127.0.0.1 or proxy.example.net, without http:// or a port."));
        return false;
    }
    showError(m_server, QString());
    return true;
}

bool ProxyWindow::validatePort(bool required)
{
    const QString text = m_port->text().trimmed();
    bool          ok   = false;
    const uint    port = text.toUInt(&ok);
    if (text.isEmpty() && !required) {
        showError(m_port, QString());
        return true;
    }
    if (!ok || port < 1 || port > 65535) {
        showError(m_port, i18n::t("Enter the port, a number from 1 to 65535."));
        return false;
    }
    const quint16 bridgePort = m_control ? m_control->config().bridgePort : 0;
    const QString host       = normalizeHost(m_server->text());
    if (!host.isEmpty() && isBridgeSelfLoop(host, static_cast<quint16>(port), bridgePort)) {
        showError(m_port, i18n::t("This is TS Media's own local port. Enter your VPN app's port."));
        return false;
    }
    showError(m_port, QString());
    return true;
}

bool ProxyWindow::validateLogin()
{
    const Type    type = m_type->currentIndex() == 1 ? Type::Http : Type::Socks5;
    const QString user = m_user->text();
    if (user.toUtf8().size() > kMaxUserBytes) {
        showError(m_user, i18n::t("The user name is too long (at most 255 bytes)."));
        return false;
    }
    if (type == Type::Http && user.contains(QLatin1Char(':'))) {
        showError(m_user, i18n::t("An HTTP proxy's user name can't contain \":\"."));
        return false;
    }
    showError(m_user, QString());
    const QString password = m_password->text();
    if (password.toUtf8().size() > kMaxPasswordBytes) {
        showError(m_password, i18n::t("The password is too long (at most 255 bytes)."));
        return false;
    }
    const bool savedForThis = !m_saved.passwordBlob.isEmpty() && !endpointChanged();
    if (type == Type::Socks5 && !user.isEmpty() && password.isEmpty() && !savedForThis) {
        showError(m_password, i18n::t("SOCKS5 needs a password with the user name."));
        return false;
    }
    showError(m_password, QString());
    return true;
}

bool ProxyWindow::validate(bool forOk, QWidget** first)
{
    // Applies when the box is checked or the server or port field is non-empty (an empty endpoint with the box
    // unchecked is simply "nothing saved").
    const bool checked = m_use->isChecked();
    const bool any     = checked || !m_server->text().trimmed().isEmpty() || !m_port->text().trimmed().isEmpty();
    if (forOk && !any) {
        clearErrors();
        return true;
    }
    QWidget* bad = nullptr;
    if (!validateServer(true))
        bad = m_server;
    if (!validatePort(true) && !bad)
        bad = m_port;
    if (!validateLogin() && !bad)
        bad = !errorOf(m_user).isEmpty() ? m_user : m_password;
    if (bad)
        bad->setFocus();
    if (first)
        *first = bad;
    return !bad;
}

bool ProxyWindow::serverLink()
{
    // A link or host:port typed into the server field (1.3.3): the fields from it, or its refusal under the field.
    const QString text = m_server->text().trimmed();
    if (text.isEmpty() || hostError(text) == HostError::None)
        return false;
    const bool linkish = text.contains(QLatin1String("://")) || text.startsWith(QLatin1String("t.me/"), Qt::CaseInsensitive)
                         || (text.count(QLatin1Char(':')) == 1 && !text.contains(QLatin1Char('['))) || text.startsWith(QLatin1Char('['));
    if (!linkish)
        return false;
    const ProxyLink link = parseProxyLink(text);
    if (link.result == ProxyLink::Result::Ok) {
        fillFrom(link);
        return true;
    }
    if (link.result == ProxyLink::Result::HostOnly)
        return false;
    showError(m_server, linkMessage(link.result));
    return true;
}

void ProxyWindow::fillFrom(const ProxyLink& link)
{
    m_loading = true;
    if (link.hasType)
        m_type->setCurrentIndex(link.type == Type::Http ? 1 : 0);
    m_server->setText(link.host);
    if (link.port)
        m_port->setText(QString::number(link.port));
    m_user->setText(link.user); // empty when the link has none (S7)
    if (link.hasPassword)
        m_password->setText(link.password); // as if typed
    m_loading = false;
    clearErrors();
    cancelJobs();
    updatePasswordHints();
    updateLanHint();
}

// ---- Paste, Find, Test --------------------------------------------------------------------------------------------

void ProxyWindow::paste()
{
    const QClipboard* clipboard = QGuiApplication::clipboard();
    const QString     text      = clipboard ? clipboard->text().left(kMaxLinkChars + 1).trimmed() : QString();
    const ProxyLink   link      = parseProxyLink(text);
    if (link.result == ProxyLink::Result::Ok) {
        fillFrom(link);
        showResult(i18n::t("Filled in from the clipboard. Press Test, then OK."), QString());
        m_test->setFocus();
        return;
    }
    // A bare host ("Proxy", "v2rayN", "::1") is no proxy link (design 1.3.6; review fix: it replaced the server and
    // cleared the user name). Only the server field keeps one.
    const ProxyLink::Result refused = link.result == ProxyLink::Result::HostOnly ? ProxyLink::Result::NotALink : link.result;
    showResult(linkMessage(refused), latin("warning"));
}

void ProxyWindow::startFind()
{
    if (m_testing || m_finding || !m_jobs.find)
        return;
    const quint64 seq = ++m_jobSeq;
    setBusy(false, true);
    showResult(QString(), QString());
    m_resultRow->hide();
    const quint16 bridgePort = m_control ? m_control->config().bridgePort : 0;
    if (!m_jobs.find(bridgePort, [this, seq](const QVector<quint16>& found) {
            if (seq == m_jobSeq)
                findDone(found);
        })) {
        setBusy(false, false);
        findDone({});
    }
}

void ProxyWindow::findDone(const QVector<quint16>& found)
{
    setBusy(false, false);
    if (found.isEmpty()) {
        showResult(findResultText(found), latin("warning"));
        return;
    }
    // SOCKS5, 127.0.0.1, the first port that answered, user name empty (the password follows its endpoint).
    ProxyLink link;
    link.result  = ProxyLink::Result::Ok;
    link.hasType = true;
    link.type    = Type::Socks5;
    link.host    = QString::fromLatin1("127.0.0.1");
    link.port    = found.first();
    fillFrom(link);
    showResult(findResultText(found), QString());
    m_test->setFocus();
}

void ProxyWindow::startTest()
{
    if (m_testing || m_finding || !m_jobs.test)
        return;
    QWidget* bad = nullptr;
    clearErrors();
    if (!validateServer(true) || !validatePort(true) || !validateLogin()) {
        validate(false, &bad);
        return;
    }
    const Config form = formConfig();
    // The typed password, else the saved one only for the very endpoint it was saved for (1.3.5).
    const QByteArray     typedUtf8 = m_password->text().toUtf8();
    protect::SecretBytes typed(typedUtf8.constData(), typedUtf8.size());
    const bool           savedValues = !endpointChanged();
    const QByteArray     blob        = m_password->text().isEmpty() && savedValues ? m_saved.passwordBlob : QByteArray();
    const bool           sameAsSaved = savedValues && m_password->text().isEmpty();
    const quint64        seq         = ++m_jobSeq;
    setBusy(true, false);
    showResult(i18n::t("Testing %1 through the proxy…").arg(latin("www.youtube.com")), QString());
    const bool started = m_jobs.test(form, std::move(typed), blob, [this, seq, form, sameAsSaved](const TestResult& r) {
        if (seq == m_jobSeq)
            testDone(r, form, sameAsSaved);
    });
    if (!started) {
        TestResult failed;
        failed.outcome = TestResult::Outcome::BridgeFailed;
        testDone(failed, form, false);
    }
}

void ProxyWindow::testDone(const TestResult& result, const Config& tested, bool savedValues)
{
    setBusy(false, false);
    const bool ok = testResultOk(result);
    showResult(testResultText(result, hostPort(tested.host, tested.port), tested.port), ok ? latin("check") : latin("warning"));
    if (m_control)
        m_control->noteTest(ok, result.failure, result.ms, savedValues && sameEndpoint(tested, m_control->config()));
}

void ProxyWindow::cancelJobs()
{
    if (!m_testing && !m_finding)
        return;
    ++m_jobSeq; // whatever comes now is dropped
    if (m_jobs.cancel)
        m_jobs.cancel();
    setBusy(false, false);
    m_resultRow->hide();
}

void ProxyWindow::setBusy(bool testing, bool finding)
{
    m_testing = testing;
    m_finding = finding;
    m_test->setText(testing ? i18n::t("Testing…") : i18n::t("&Test"));
    m_find->setText(finding ? i18n::t("Looking…") : i18n::t("&Find my VPN app"));
    m_test->setEnabled(!testing && !finding);
    m_find->setEnabled(!testing && !finding);
}

void ProxyWindow::setIcon(QLabel* label, const QString& kind)
{
    if (!label)
        return; // not built yet
    const int size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    if (kind == QLatin1String("warning"))
        label->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(size, size));
    else if (kind == QLatin1String("check"))
        label->setPixmap(settingstheme::checkMark(settingstheme::roleColors(palette()).ok, size, devicePixelRatioF()));
    else
        label->setPixmap(QPixmap());
    label->setVisible(!kind.isEmpty());
}

void ProxyWindow::showResult(const QString& text, const QString& icon)
{
    m_resultIconKind = icon;
    setIcon(m_resultIcon, icon);
    m_resultText->setText(text); // a plain QLabel: screen readers hear NameChanged
    m_resultText->setProperty("role", icon == QLatin1String("check") ? latin("ok") : QString());
    m_resultRow->setVisible(!text.isEmpty());
    applyTheme();
}

// ---- the status row, a route change elsewhere, Use it -----------------------------------------------------------------

void ProxyWindow::refreshStatus()
{
    if (!m_control)
        return;
    texts::StatusInput in;
    in.saved       = m_control->config();
    in.mode        = m_control->mode();
    in.health      = m_control->state();
    in.lastFailure = m_control->lastFailure();
    in.windows     = m_control->windowsStatus();
    in.nowMs       = m_control->clockMs();
    in.summary     = windowsproxy::windowsProxySummary();
    const Control::LastTest test = m_control->lastTest();
    if (test.ran && test.ok)
        in.lastTestWhen = ::ui::whenText(QDateTime::fromMSecsSinceEpoch(test.wallMs, Qt::UTC));
    const texts::StatusRow row = texts::statusRow(in);
    m_statusIconKind           = row.icon == texts::StatusRow::Icon::Check ? latin("check") : row.icon == texts::StatusRow::Icon::Warning ? latin("warning") : QString();
    setIcon(m_statusIcon, m_statusIconKind);
    m_statusText->setText(row.text);
    m_statusText->setProperty("role", row.icon == texts::StatusRow::Icon::Check ? latin("ok") : QString());
    m_useIt->setVisible(row.useIt);
    applyTheme();
}

void ProxyWindow::routeChanged()
{
    // The menu, the hotkey, the command or the Settings group changed it: the checkbox and the radios follow when the
    // user hasn't touched them here; the endpoint fields are never overwritten (1.3.9).
    if (m_control) {
        const Config saved = m_control->config();
        m_loading          = true;
        if (!m_checkTouched)
            m_use->setChecked(saved.on);
        if (!m_radioTouched)
            (saved.whenOff == WhenOff::Direct ? m_direct : m_system)->setChecked(true);
        m_loading = false;
    }
    refreshStatus();
}

void ProxyWindow::useIt()
{
    // Windows names only a SOCKS proxy: the form takes it (checked, SOCKS5, its host and port, no user name); nothing is
    // saved until OK.
    const windowsproxy::Summary s = windowsproxy::windowsProxySummary();
    if (s.kind != windowsproxy::Summary::Kind::SocksOnly || s.socksHost.isEmpty() || !s.socksPort)
        return;
    ProxyLink link;
    link.result  = ProxyLink::Result::Ok;
    link.hasType = true;
    link.type    = Type::Socks5;
    link.host    = normalizeHost(s.socksHost);
    link.port    = s.socksPort;
    fillFrom(link);
    m_loading = true;
    m_use->setChecked(true);
    m_loading      = false;
    m_checkTouched = true;
    m_test->setFocus();
}

// ---- OK, Cancel ------------------------------------------------------------------------------------------------------

bool ProxyWindow::accept_()
{
    if (!m_control)
        return false;
    QWidget* bad = nullptr;
    clearErrors();
    if (!validate(true, &bad))
        return false;
    Config next = formConfig();
    if (!next.saved())
        next.on = false; // nothing saved: off
    // The blob: typed -> sealed once with the endpoint's entropy; endpoint changed and nothing typed -> none; else kept.
    const QString typed = m_password->text();
    if (!typed.isEmpty()) {
        QByteArray blob;
        if (!protect::seal(typed, next.endpoint(), &blob)) {
            showError(m_password, i18n::t("Windows couldn't protect the password, so it wasn't saved. Try again."));
            return false;
        }
        next.passwordBlob = blob;
    } else if (!sameEndpoint(next, m_saved)) {
        next.passwordBlob.clear();
    }
    cancelJobs();
    m_password->clear();
    m_control->save(next);
    m_saved = m_control->config();
    QDialog::accept();
    return true;
}

void ProxyWindow::reject()
{
    cancelJobs();
    m_password->clear();
    QDialog::reject();
}

void ProxyWindow::closeEvent(QCloseEvent* event)
{
    cancelJobs();
    m_password->clear();
    QDialog::closeEvent(event);
}

// ---- theme ------------------------------------------------------------------------------------------------------------

void ProxyWindow::applyTheme()
{
    if (m_theming)
        return;
    m_theming = true;
    ensurePolished();
    const settingstheme::Colors colors = settingstheme::roleColors(palette());
    settingstheme::applyLabelRoles(this, colors);
    if (!qApp->styleSheet().isEmpty()) {
        const QString sheet = settingstheme::roleSheet(objectName(), colors);
        if (sheet != styleSheet())
            setStyleSheet(sheet);
    }
    // Review fix: the painted check mark takes the palette's ok colour when it is painted, which was before the skin
    // polished the window (light green on a dark skin): painted again with the colours of now.
    setIcon(m_statusIcon, m_statusIconKind);
    setIcon(m_resultIcon, m_resultIconKind);
    m_theming = false;
}

// Live test fix: a result, an error or a hint that appears makes the form taller, but a QDialog doesn't grow by itself and
// its minimum comes from the word-wrapped labels' one-line size hints, so the ports hint slid under the Find / Paste /
// Test buttons (unreadable) once the result row showed. On every layout request (the layout is active by then) the
// window keeps at least the height its layout needs at its width.
bool ProxyWindow::event(QEvent* event)
{
    const bool done = QDialog::event(event);
    if (event->type() == QEvent::LayoutRequest)
        fitHeight();
    return done;
}

void ProxyWindow::fitHeight()
{
    QLayout* l = layout();
    if (!m_built || !l)
        return;
    const int wanted = l->hasHeightForWidth() ? l->totalHeightForWidth(width()) : l->totalSizeHint().height();
    if (wanted <= 0)
        return;
    if (minimumHeight() < wanted)
        setMinimumHeight(wanted);
    if (height() < wanted)
        resize(width(), wanted);
}

void ProxyWindow::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    // Live test fix: under TeamSpeak's skin (an application style sheet) the constructor's setWindowFlags re-parents the
    // window, which sends a StyleChange before the labels exist; the constructor themes the window itself at its end.
    if (m_built && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange))
        applyTheme();
}

void ProxyWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (m_built)
        applyTheme();
}

} // namespace proxy::ui
