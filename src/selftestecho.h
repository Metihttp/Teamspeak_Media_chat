#pragma once

// 2.6 chat proxy (design 2.4, S18): the live-test driver (selftest.cpp, test builds only) echoes every script line into
// the log ("> {...}"). A line that sets the chat proxy's password ({"cmd":"proxy.set","password":"..."}) must not put
// the password there: echoLine() shows its value as *** (a JSON "password" member, and a password=... argument). Pure,
// header-only, so tsmedia_tests checks it without a test build. No static Qt objects (the DLL unloads). The driver
// masks the whole line first and cuts it afterwards.

#include <QRegularExpression>
#include <QString>

namespace selftest {

inline QString echoLine(const QString& line)
{
    QString out = line;
    // "password": "...", with escaped quotes inside the value (also a value cut off without its closing quote).
    const QRegularExpression json(QString::fromLatin1("(\"password\"\\s*:\\s*\")((?:[^\"\\\\]|\\\\.)*)(\"|$)"),
                                  QRegularExpression::CaseInsensitiveOption);
    out.replace(json, QString::fromLatin1("\\1***\\3"));
    // password=... up to the next space, comma, quote, ampersand or closing brace.
    const QRegularExpression arg(QString::fromLatin1("(password=)([^\\s,\"&}]*)"), QRegularExpression::CaseInsensitiveOption);
    out.replace(arg, QString::fromLatin1("\\1***"));
    return out;
}

} // namespace selftest
