#pragma once

#include <QString>
#include <Qt>

// UI language. Auto follows the Windows display language (Persian -> Persian, else English).
enum class Language { Auto = 0, English = 1, Persian = 2 };

namespace i18n {

void     setLanguage(Language language);
Language language(); // as configured (may be Auto)
bool     isPersian(); // effective language

// Every user-visible string is written at the call site in both languages (UTF-8 literals):
//   i18n::t("Click to download", "برای دانلود کلیک کنید")
QString t(const char* english, const char* persian);

Qt::LayoutDirection direction();

} // namespace i18n
