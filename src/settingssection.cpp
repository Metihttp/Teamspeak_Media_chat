#include "settingssection.h"

#include <QFormLayout>
#include <QLabel>
#include <QVariant>

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
