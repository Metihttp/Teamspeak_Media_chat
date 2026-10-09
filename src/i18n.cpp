#include "i18n.h"

namespace i18n {

QString t(const char* text)
{
    return QString::fromUtf8(text);
}

} // namespace i18n
