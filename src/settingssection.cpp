#include "settingssection.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QVariant>

#include "settingstheme.h"
#include "settingswidgets.h"

SettingsSection::SettingsSection(QWidget* parent)
    : QWidget(parent)
{
}

QLabel* SettingsSection::hint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop); // right under what it explains, also when the row is taller
    label->setProperty("role", QString::fromLatin1("hint"));
    // As wide as its column (a form layout keeps a Preferred field at its size hint, which for wrapped
    // text is narrow). setWordWrap() has already set height-for-width; keep it.
    QSizePolicy policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    label->setSizePolicy(policy);
    return label;
}

QFormLayout* SettingsSection::form(QWidget* parent)
{
    auto* layout = new QFormLayout(parent);
    layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    layout->setLabelAlignment(Qt::AlignLeading | Qt::AlignVCenter);
    return layout;
}

void SettingsSection::addUnderField(QFormLayout* layout, QWidget* widget)
{
    layout->addRow(static_cast<QWidget*>(nullptr), widget);
}

// ---- 2.7 F3: cards and rows -------------------------------------------------------------------------------------------
void SettingsSection::makeCard(QGroupBox* box)
{
    box->setProperty("tsmediaRows", true); // the dialog's sheet: no padding (the rows have their own)
    if (!box->layout()) {
        auto* layout = new QVBoxLayout(box);
        layout->setSpacing(0);
    }
    const int edge = settingstheme::tokensFor(box).px(2);
    box->layout()->setContentsMargins(0, edge, 0, edge);
}

QGroupBox* SettingsSection::card(const QString& title, QWidget* parent)
{
    auto* box = new QGroupBox(title, parent);
    makeCard(box);
    if (parent && parent->layout())
        parent->layout()->addWidget(box);
    return box;
}

namespace {

SettingRow* addRow(QGroupBox* card, SettingRow::Kind kind, int indent)
{
    if (!card->layout())
        SettingsSection::makeCard(card);
    auto* row = new SettingRow(kind, indent, card);
    card->layout()->addWidget(row);
    return row;
}

} // namespace

SwitchBox* SettingsSection::switchRow(QGroupBox* card, const QString& text, const QString& hintText, int indent, QWidget* trailing)
{
    SettingRow* row = addRow(card, SettingRow::Kind::Switch, indent);
    auto*       box = new SwitchBox(text, row);
    QLabel*     h   = hintText.isEmpty() ? nullptr : hint(hintText, row);
    if (h)
        box->setAccessibleDescription(hintText);
    row->setParts(box, h, trailing);
    return box;
}

QLabel* SettingsSection::fieldRow(QGroupBox* card, const QString& label, QWidget* field, const QString& hintText, int indent)
{
    SettingRow* row   = addRow(card, SettingRow::Kind::Field, indent);
    auto*       title = new QLabel(label, row);
    title->setWordWrap(true);
    QSizePolicy policy = title->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    title->setSizePolicy(policy);
    if (field && field->focusPolicy() != Qt::NoFocus)
        title->setBuddy(field);
    QLabel* h = hintText.isEmpty() ? nullptr : hint(hintText, row);
    if (h && field && field->accessibleDescription().isEmpty())
        field->setAccessibleDescription(hintText);
    row->setParts(title, h, field);
    return title;
}

QLabel* SettingsSection::stackedRow(QGroupBox* card, const QString& label, QWidget* field, const QString& hintText, int indent, QWidget* extra)
{
    SettingRow* row   = addRow(card, SettingRow::Kind::Stacked, indent);
    auto*       title = new QLabel(label, row);
    title->setWordWrap(true);
    if (field)
        title->setBuddy(field);
    QLabel* h = hintText.isEmpty() ? nullptr : hint(hintText, row);
    if (h && field && field->accessibleDescription().isEmpty())
        field->setAccessibleDescription(hintText);
    row->setParts(title, h, field, extra);
    return title;
}

SettingRow* SettingsSection::customRow(QGroupBox* card, QWidget* content, int indent)
{
    SettingRow* row = addRow(card, SettingRow::Kind::Custom, indent);
    row->setParts(nullptr, nullptr, content);
    return row;
}

SettingRow* SettingsSection::rowOf(const QWidget* control)
{
    for (QWidget* w = control ? control->parentWidget() : nullptr; w; w = w->parentWidget()) {
        if (auto* row = qobject_cast<SettingRow*>(w))
            return row;
        if (qobject_cast<QGroupBox*>(w))
            break;
    }
    return nullptr;
}

void SettingsSection::setRowEnabled(QWidget* control, bool enabled)
{
    if (SettingRow* row = rowOf(control))
        row->setEnabled(enabled);
    else if (control)
        control->setEnabled(enabled);
}

QLabel* SettingsSection::hintOf(const QWidget* control)
{
    SettingRow* row = rowOf(control);
    if (!row)
        return nullptr;
    for (QLabel* label : row->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (label->property("role").toString() == QLatin1String("hint"))
            return label;
    }
    return nullptr;
}
