#include "i18n.h"

#include <QLocale>

namespace i18n {

namespace {
Language g_language = Language::Auto;
}

void setLanguage(Language language)
{
    g_language = language;
}

Language language()
{
    return g_language;
}

bool isPersian()
{
    if (g_language == Language::Auto) {
        // Called for every rendered string; the Windows display language does not change while we run.
        static const bool systemPersian = QLocale::system().language() == QLocale::Persian;
        return systemPersian;
    }
    return g_language == Language::Persian;
}

QString t(const char* english, const char* persian)
{
    return QString::fromUtf8(isPersian() ? persian : english);
}

Qt::LayoutDirection direction()
{
    return isPersian() ? Qt::RightToLeft : Qt::LeftToRight;
}

} // namespace i18n
