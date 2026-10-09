#pragma once

#include <QString>

namespace i18n {

// Every user-visible string goes through here: i18n::t("Click to download").
// Not QStringLiteral on purpose: such strings may end up in Qt state that outlives the plugin DLL
// (style sheets, settings, TeamSpeak's widgets; see settings.cpp), so they are allocated by Qt.
QString t(const char* text);

} // namespace i18n
