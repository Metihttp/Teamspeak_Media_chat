#include "updatesettingsgroup.h"

#include <QCheckBox>
#include <QDate>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

#include "i18n.h"
#include "updater.h"

namespace upd {

namespace {

QLabel* hintLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setProperty("role", QString::fromLatin1("hint")); // coloured by the settings dialog's theme code
    QSizePolicy policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    label->setSizePolicy(policy);
    return label;
}

// "today at 10:15", "yesterday at 10:15", "Nov 2 at 10:15" (local time, English like the rest).
QString whenText(const QDateTime& utc)
{
    const QDateTime local = utc.toLocalTime();
    const QLocale   english(QLocale::English);
    const QString   time  = english.toString(local.time(), QString::fromLatin1("HH:mm"));
    const QDate     today = QDate::currentDate();
    if (local.date() == today)
        return i18n::t("today at %1").arg(time);
    if (local.date() == today.addDays(-1))
        return i18n::t("yesterday at %1").arg(time);
    return i18n::t("%1 at %2").arg(english.toString(local.date(), QString::fromLatin1("MMM d")), time);
}

} // namespace

UpdateSettingsGroup::UpdateSettingsGroup(QWidget* parent)
    : QGroupBox(i18n::t("Updates"), parent)
{
    setObjectName(QString::fromLatin1("tsmediaUpdatesGroup"));
    auto* layout = new QVBoxLayout(this);

    Updater* updater = Updater::instance();
    if (!Updater::builtIn() || !updater) {
        layout->addWidget(hintLabel(i18n::t("Updates are turned off in versions you build yourself. Check "
                                            "github.com/Metihttp/Teamspeak_Media_chat/releases for new versions."),
                                    this));
        return;
    }

    m_automatic = new QCheckBox(i18n::t("Check for &updates automatically"), this);
    auto* hint  = hintLabel(i18n::t("About once a day, TS Media chat asks GitHub whether there is a new version. Nothing is installed until you "
                                    "click Update. GitHub sees your IP address, and Windows may check GitHub's certificate with its issuer; "
                                    "nothing else is sent."),
                           this);
    m_automatic->setAccessibleDescription(hint->text());
    m_initial = updater->status().automatic;
    m_automatic->setChecked(m_initial);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    QSizePolicy policy = m_status->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    m_status->setSizePolicy(policy);
    m_action = new QPushButton(this);
    m_action->setFlat(true);
    m_action->setAutoDefault(false);
    m_action->setCursor(Qt::PointingHandCursor);
    m_action->hide();
    m_checkNow = new QPushButton(i18n::t("Check &now"), this);
    m_checkNow->setAutoDefault(false);

    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_action, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_checkNow, 0, Qt::AlignVCenter);

    layout->addWidget(m_automatic);
    layout->addWidget(hint);
    layout->addLayout(statusRow);

    // Lined up with the checkbox's text, like the other dependent rows of the dialog.
    hint->setContentsMargins(m_automatic->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, m_automatic)
                                 + m_automatic->style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, m_automatic),
                             0, 0, 0);

    connect(m_checkNow, &QPushButton::clicked, this, [this] {
        if (Updater* u = Updater::instance()) {
            m_checking = true;
            u->checkNow(Updater::Origin::Settings);
        }
        refresh();
    });
    connect(m_action, &QPushButton::clicked, this, [this] {
        Updater* u = Updater::instance();
        if (!u)
            return;
        const Updater::Status s = u->status();
        if (s.phase == Updater::Phase::RestartPending)
            u->restartNow();
        else if (s.phase == Updater::Phase::Available || s.phase == Updater::Phase::Downloading)
            u->openUpdateDialog();
        else if (s.skipped.isValid())
            u->undoSkip();
    });
    connect(updater, &Updater::statusChanged, this, [this] { refresh(); });
    refresh();
}

void UpdateSettingsGroup::apply()
{
    Updater* updater = Updater::instance();
    if (!updater || !m_automatic || m_automatic->isChecked() == m_initial)
        return;
    m_initial = m_automatic->isChecked();
    updater->setAutomatic(m_initial);
}

void UpdateSettingsGroup::refresh()
{
    Updater* updater = Updater::instance();
    if (!updater || !m_status)
        return;
    const Updater::Status s       = updater->status();
    const QString         version = i18n::t("Version %1").arg(s.installed.toString());
    QString               text;
    QString               action;
    switch (s.phase) {
    case Updater::Phase::Checking:
        text = i18n::t("Checking for updates…");
        break;
    case Updater::Phase::Downloading:
    case Updater::Phase::Installing:
        text   = i18n::t("Updating to %1…").arg(s.available.toString());
        action = i18n::t("Show");
        break;
    case Updater::Phase::RestartPending:
    case Updater::Phase::Restarting:
        text   = i18n::t("Restart TeamSpeak to finish updating to %1.").arg(s.pending.toString());
        action = i18n::t("Restart now");
        break;
    case Updater::Phase::Available:
        text   = i18n::t("%1 is available.").arg(s.available.toString());
        action = i18n::t("Update…");
        break;
    default:
        if (!s.lastError.isEmpty()) {
            text = s.automatic ? i18n::t("Last check failed: %1. It will try again later.").arg(s.lastError)
                               : i18n::t("Last check failed: %1.").arg(s.lastError);
        } else if (s.skipped.isValid() && s.skipped > s.installed) {
            text   = i18n::t("You skipped %1.").arg(s.skipped.toString());
            action = i18n::t("Undo");
        } else if (m_checking && s.manualUpToDate) {
            text = version + QString::fromUtf8(" · ") + i18n::t("You have the latest version.");
        } else if (s.lastSuccess.isValid()) {
            text = version + QString::fromUtf8(" · ") + i18n::t("Checked %1").arg(whenText(s.lastSuccess));
        } else {
            text = version + QString::fromUtf8(" · ") + i18n::t("Never checked");
        }
        break;
    }
    m_status->setText(text);
    m_status->setAccessibleDescription(text);
    m_action->setText(action);
    m_action->setAccessibleName(action);
    m_action->setVisible(!action.isEmpty());
    const bool checking = s.phase == Updater::Phase::Checking;
    m_checkNow->setEnabled(!checking && s.phase != Updater::Phase::Downloading && s.phase != Updater::Phase::Installing
                           && s.phase != Updater::Phase::Restarting);
    m_checkNow->setText(checking ? i18n::t("Checking…") : i18n::t("Check &now"));
}

} // namespace upd
