#include "serverssection.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

#include "i18n.h"
#include "settings.h"

namespace {

constexpr int kNameChars      = 40;  // longer server names are shortened in the list...
constexpr int kNameWidth      = 150; // ... and to this many pixels
constexpr int kServerMinWidth = 220;

// Index 0 of both choice boxes is "Same as all servers (…)".
enum Choice { SameAsAll = 0, ChoiceOn = 1, ChoiceOff = 2 };

QLabel* hintLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setProperty("role", QString::fromLatin1("hint")); // coloured by SettingsDialog::applyTheme
    QSizePolicy policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    label->setSizePolicy(policy);
    return label;
}

QString elided(const QString& name)
{
    return name.size() > kNameChars ? name.left(kNameChars - 1) + QChar(0x2026) : name;
}

std::optional<bool> boolChoice(int index)
{
    if (index == ChoiceOn)
        return true;
    if (index == ChoiceOff)
        return false;
    return std::nullopt;
}

int choiceOf(const std::optional<bool>& value)
{
    return !value ? SameAsAll : *value ? ChoiceOn : ChoiceOff;
}

// "Automatic downloads: On" is the data saver off.
std::optional<bool> dataSaverChoice(int index)
{
    const std::optional<bool> downloads = boolChoice(index);
    return downloads ? std::optional<bool>(!*downloads) : std::nullopt;
}

int choiceOfDataSaver(const std::optional<bool>& dataSaver)
{
    return choiceOf(dataSaver ? std::optional<bool>(!*dataSaver) : std::nullopt);
}

} // namespace

ServersSection::ServersSection(QWidget* parent)
    : QGroupBox(i18n::t("Servers"), parent)
    , m_globalFolder(QString::fromLatin1(Settings::defaultUploadDirectory))
{
    setObjectName(QString::fromLatin1("tsmediaServersSection"));

    // Access keys are left to the dialog that places this section (the 2.1 dialog has used up the
    // letters these labels share); every field still gets its label as accessible name (buddy).
    m_server = new QComboBox(this);
    m_server->setMinimumWidth(kServerMinWidth);
    m_server->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_server->setMinimumContentsLength(24);
    m_forget = new QPushButton(i18n::t("For&get"), this);
    m_forget->setAutoDefault(false);
    m_forget->setToolTip(i18n::t("Remove this server's own settings. It then uses the settings for all servers."));
    m_forget->setAccessibleDescription(m_forget->toolTip());

    // "Automatic downloads": data saver off = On, data saver on = Paused.
    m_downloads = new QComboBox(this);
    m_downloads->addItems({QString(), i18n::t("On"), i18n::t("Paused (data saver)")});
    // The note for people without the plugin.
    m_note = new QComboBox(this);
    m_note->addItems({QString(), i18n::t("Add the note"), i18n::t("Don't add the note")});

    m_folder = new QLineEdit(this);
    m_folder->setMaxLength(255);
    m_limit = new QSpinBox(this);
    m_limit->setRange(0, Settings::uploadMaxMBRange.max); // 0: the special "same as all servers" value
    m_limit->setSingleStep(10);
    m_limit->setSuffix(i18n::t(" MB"));
    m_limit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_limit->setAccelerated(true);

    m_hint = hintLabel(QString(), this);

    // The server picker spans the section (long names need the room); its values follow as a form.
    auto* serverLabel = new QLabel(i18n::t("&Server"), this);
    serverLabel->setBuddy(m_server);
    auto* serverRow = new QHBoxLayout;
    serverRow->addWidget(serverLabel);
    serverRow->addWidget(m_server, 1);
    serverRow->addWidget(m_forget);

    auto* layout = new QFormLayout;
    layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows); // a narrow column puts long labels above their field
    layout->setLabelAlignment(Qt::AlignLeading | Qt::AlignVCenter);
    auto* outer = new QVBoxLayout(this);
    outer->addLayout(serverRow);
    outer->addLayout(layout);
    const struct {
        const char* label;
        QWidget*    field;
    } rows[] = {
        // Access keys: unique on the Servers tab (with Server access below and the dialog's buttons).
        {"Automatic &downloads", m_downloads},
        {"Upload &folder", m_folder},
        {"Upload size &limit", m_limit},
        {"&Note for people without the plugin", m_note},
    };
    for (const auto& row : rows) {
        auto* label = new QLabel(i18n::t(row.label), this);
        label->setBuddy(row.field);
        layout->addRow(label, row.field);
        m_fields << label << row.field;
    }
    layout->addRow(m_hint);

    connect(m_server, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_loading)
            return;
        storeForm();
        m_current = index >= 0 && index < m_rows.size() ? index : -1;
        showRow();
    });
    const auto edited = [this] {
        if (m_loading)
            return;
        storeForm();
        updateState();
        emit changed();
    };
    connect(m_downloads, QOverload<int>::of(&QComboBox::activated), this, edited);
    connect(m_note, QOverload<int>::of(&QComboBox::activated), this, edited);
    connect(m_folder, &QLineEdit::textEdited, this, edited);
    connect(m_limit, QOverload<int>::of(&QSpinBox::valueChanged), this, edited);
    connect(m_forget, &QPushButton::clicked, this, [this] {
        Row* row = currentRow();
        if (!row)
            return;
        const QString name = row->edit.name;
        row->edit          = ServerOverrides();
        row->edit.name     = name;
        row->forget        = true;
        showRow();
        emit changed();
        m_server->setFocus(Qt::OtherFocusReason); // Forget is disabled now
    });

    updateTexts();
    reload({});
}

void ServersSection::setGlobals(bool dataSaver, const QString& uploadDirectory, int uploadMaxMB, bool addRequiredNotice)
{
    m_globalDataSaver = dataSaver;
    m_globalFolder    = Settings::normalizeUploadDirectory(uploadDirectory);
    m_globalLimitMB   = uploadMaxMB;
    m_globalNote      = addRequiredNotice;
    updateTexts();
}

void ServersSection::updateTexts()
{
    m_downloads->setItemText(SameAsAll, m_globalDataSaver ? i18n::t("Same as all servers (Paused)") : i18n::t("Same as all servers (On)"));
    m_note->setItemText(SameAsAll, m_globalNote ? i18n::t("Same as all servers (Added)") : i18n::t("Same as all servers (Not added)"));
    m_folder->setPlaceholderText(i18n::t("Same as all servers (%1)").arg(m_globalFolder));
    m_limit->setSpecialValueText(i18n::t("Same as all servers (%1 MB)").arg(m_globalLimitMB));
    // The three choice fields share one width, wide enough for their "Same as all servers (…)" texts
    // (a spin box sizes itself by its range and suffix only).
    int width = m_limit->fontMetrics().horizontalAdvance(m_limit->specialValueText()) + 48;
    for (QComboBox* box : {m_downloads, m_note}) {
        box->setMinimumWidth(0);
        width = qMax(width, box->sizeHint().width());
    }
    for (QWidget* field : {static_cast<QWidget*>(m_downloads), static_cast<QWidget*>(m_note), static_cast<QWidget*>(m_limit)})
        field->setMinimumWidth(width);
}

ServersSection::Row* ServersSection::currentRow()
{
    return m_current >= 0 && m_current < m_rows.size() ? &m_rows[m_current] : nullptr;
}

QString ServersSection::rowLabel(const Row& row) const
{
    // Shortened so " (connected)" always shows in the field (the list itself is as wide as needed).
    const QString name = row.name.isEmpty() ? i18n::t("Unnamed server") : m_server->fontMetrics().elidedText(elided(row.name), Qt::ElideRight, kNameWidth);
    return row.connected ? i18n::t("%1 (connected)").arg(name) : name;
}

void ServersSection::reload(const QVector<ConnectedServer>& connected)
{
    storeForm();
    const QString                          selected = currentRow() ? currentRow()->key : QString();
    const QHash<QString, ServerOverrides>& saved    = Settings::instance().servers;

    QHash<QString, Row> previous;
    for (const Row& row : qAsConst(m_rows))
        previous.insert(row.key, row);

    // A row shows what is saved now, except values edited here (they stay relative to what they
    // were edited from, so Apply only writes those).
    auto rowFor = [&previous, &saved](const QString& key, const QString& liveName, bool isConnected) {
        Row                   row;
        const ServerOverrides disk = saved.value(key);
        const auto            old  = previous.constFind(key);
        if (old != previous.constEnd() && old->dirty()) {
            row          = old.value();
            auto refresh = [](auto& base, auto& edit, const auto& now) {
                if (edit == base)
                    edit = now;
                base = now;
            };
            if (!row.forget) {
                refresh(row.base.dataSaver, row.edit.dataSaver, disk.dataSaver);
                refresh(row.base.uploadDirectory, row.edit.uploadDirectory, disk.uploadDirectory);
                refresh(row.base.uploadMaxMB, row.edit.uploadMaxMB, disk.uploadMaxMB);
                refresh(row.base.addRequiredNotice, row.edit.addRequiredNotice, disk.addRequiredNotice);
            } else {
                row.base = disk;
            }
        } else {
            row.base = disk;
            row.edit = disk;
        }
        row.key       = key;
        row.connected = isConnected;
        row.name      = !liveName.isEmpty() ? liveName : disk.name;
        row.edit.name = row.name;
        return row;
    };

    QVector<Row>  rows;
    QSet<QString> listed;
    for (const ConnectedServer& server : connected) {
        if (server.key.isEmpty() || listed.contains(server.key))
            continue;
        listed.insert(server.key);
        rows.append(rowFor(server.key, serversettings::sanitizeName(server.name), true));
    }
    // Then the servers with own settings, by name; servers edited here that are gone stay listed.
    QVector<Row> others;
    for (auto it = saved.constBegin(); it != saved.constEnd(); ++it) {
        if (!listed.contains(it.key()))
            others.append(rowFor(it.key(), QString(), false));
        listed.insert(it.key());
    }
    for (const Row& row : qAsConst(m_rows)) {
        if (!listed.contains(row.key) && row.dirty()) {
            others.append(rowFor(row.key, QString(), false));
            listed.insert(row.key);
        }
    }
    std::sort(others.begin(), others.end(), [](const Row& a, const Row& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    rows += others;
    m_rows = rows;
    rebuildCombo(selected);
}

void ServersSection::rebuildCombo(const QString& selectKey)
{
    m_loading = true;
    m_server->clear();
    QHash<QString, int> seen; // the same name twice: "Name (2)"
    for (const Row& row : qAsConst(m_rows)) {
        QString    label = rowLabel(row);
        const int  count = ++seen[label];
        if (count > 1)
            label = i18n::t("%1 (%2)").arg(label).arg(count);
        m_server->addItem(label);
        // The whole name (a server's name is anyone's text: escaped, never read as markup).
        if (!row.name.isEmpty())
            m_server->setItemData(m_server->count() - 1, QString::fromLatin1("<p style='white-space:pre'>%1</p>").arg(row.name.toHtmlEscaped()), Qt::ToolTipRole);
    }
    m_current = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).key == selectKey)
            m_current = i;
    }
    if (m_current < 0 && !m_rows.isEmpty())
        m_current = 0;
    if (m_rows.isEmpty())
        m_server->addItem(i18n::t("Not connected"));
    m_server->setCurrentIndex(qMax(0, m_current));
    m_loading = false;
    showRow();
}

void ServersSection::showRow()
{
    const Row* row = currentRow();
    m_loading      = true;
    {
        const QSignalBlocker blockLimit(m_limit); // also keeps the dialog's own valueChanged handler out
        m_downloads->setCurrentIndex(row ? choiceOfDataSaver(row->edit.dataSaver) : SameAsAll);
        m_note->setCurrentIndex(row ? choiceOf(row->edit.addRequiredNotice) : SameAsAll);
        m_folder->setText(row && row->edit.uploadDirectory ? *row->edit.uploadDirectory : QString());
        m_folder->setCursorPosition(0);
        m_limit->setValue(row && row->edit.uploadMaxMB ? *row->edit.uploadMaxMB : 0);
    }
    m_loading = false;
    updateState();
}

void ServersSection::storeForm()
{
    Row* row = currentRow();
    if (!row)
        return;
    row->edit.dataSaver         = dataSaverChoice(m_downloads->currentIndex());
    row->edit.addRequiredNotice = boolChoice(m_note->currentIndex());
    const QString folder        = m_folder->text().trimmed();
    if (folder.isEmpty())
        row->edit.uploadDirectory.reset();
    else
        row->edit.uploadDirectory = Settings::normalizeUploadDirectory(folder);
    if (m_limit->value() > 0)
        row->edit.uploadMaxMB = qBound(Settings::uploadMaxMBRange.min, m_limit->value(), Settings::uploadMaxMBRange.max);
    else
        row->edit.uploadMaxMB.reset();
}

void ServersSection::updateState()
{
    const Row* row = currentRow();
    m_server->setEnabled(row != nullptr);
    for (QWidget* field : qAsConst(m_fields))
        field->setEnabled(row != nullptr);
    m_forget->setEnabled(row && !row->edit.isEmpty());
    m_hint->setText(m_rows.isEmpty() ? i18n::t("Connect to a server to give it its own settings.")
                                     : i18n::t("Only servers you change here are remembered, by their ID and name, on this computer."));
}

bool ServersSection::hasChanges() const
{
    for (const Row& row : m_rows) {
        if (row.dirty())
            return true;
    }
    return false;
}

void ServersSection::applyTo(Settings& settings)
{
    storeForm();
    for (Row& row : m_rows) {
        if (!row.dirty())
            continue;
        ServerOverrides target;
        if (row.forget) {
            target = row.edit; // values set again after Forget
        } else {
            target = settings.servers.value(row.key);
            if (row.edit.dataSaver != row.base.dataSaver)
                target.dataSaver = row.edit.dataSaver;
            if (row.edit.uploadDirectory != row.base.uploadDirectory)
                target.uploadDirectory = row.edit.uploadDirectory;
            if (row.edit.uploadMaxMB != row.base.uploadMaxMB)
                target.uploadMaxMB = row.edit.uploadMaxMB;
            if (row.edit.addRequiredNotice != row.base.addRequiredNotice)
                target.addRequiredNotice = row.edit.addRequiredNotice;
        }
        if (!row.name.isEmpty())
            target.name = row.name;
        if (target.isEmpty()) {
            settings.servers.remove(row.key); // only servers with own values are remembered
        } else if (settings.servers.contains(row.key) || settings.servers.size() < serversettings::kMaxServers) {
            settings.servers.insert(row.key, target);
        }
        row.base   = settings.servers.value(row.key);
        row.edit   = row.base;
        row.forget = false;
        row.edit.name = row.name;
    }
    showRow();
}
