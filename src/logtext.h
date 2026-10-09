#pragma once

// 2.2 foundation: structured log lines. Values that may identify someone (file names, paths, other
// names) are tagged where the line is written, so the plugin's own log file can mark them and the
// diagnostic info can leave them out exactly instead of guessing. QtCore only (unit-tested).
//
//   ts3::log(LogLevel_WARNING, sch, "Download of %1 failed: %2", {ts3::file(path), ts3::pub(reason)});
//
// TeamSpeak's log gets the plain text. The plugin log gets the marked text: every tagged value as
// U+E000, its kind letter, the value, U+E001. Kinds:
//   f  a remote file name or path (on the server)
//   l  a local path or file name
//   n  any other name (nickname, channel, server, caption)
//   u  unclassified text from a 2.1-style call (the whole line): treat it as private
// pub() values and the format string itself are not marked.

#include <QString>
#include <QStringList>

#include <initializer_list>
#include <type_traits>

namespace ts3 {

constexpr QChar kLogMarkStart = QChar(0xE000);
constexpr QChar kLogMarkEnd   = QChar(0xE001);

class LogArg
{
  public:
    char           kind() const { return m_kind; } // 0 for public values
    const QString& text() const { return m_text; }

  private:
    LogArg(char kind, const QString& text);
    friend LogArg pub(const QString& text);
    friend LogArg file(const QString& text);
    friend LogArg local(const QString& text);
    friend LogArg name(const QString& text);

    char    m_kind;
    QString m_text;
};

LogArg pub(const QString& text);   // nothing private: numbers, sizes, error texts, states
LogArg file(const QString& text);  // kind 'f'
LogArg local(const QString& text); // kind 'l'
LogArg name(const QString& text);  // kind 'n'

template <typename T, std::enable_if_t<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value && !std::is_same<T, char>::value, int> = 0>
LogArg pub(T number)
{
    return pub(QString::number(number));
}

// One substitution pass over format (UTF-8): "%1".."%9" become the arguments, so a "%2" inside a file
// name is never expanded again. A "%n" without an argument stays as it is. Line breaks in the result
// become spaces (one log line per call).
struct LogText {
    QString plain;  // for TeamSpeak's log
    QString marked; // for the plugin log
};
LogText formatLog(const char* format, std::initializer_list<LogArg> args);

// A 2.1-style line of free text: the whole line is one 'u' span.
LogText unclassifiedLog(const QString& text);

// The texts printWarning() writes into the chat: names are in curly quotes there (“…”), and those
// spans become 'n'. Everything else counts as public.
LogText quotedNamesLog(const QString& text);

// The marked text with every private span replaced by "<kind N>" (the diagnostics feature builds on
// this); an unbalanced marker hides the rest of the line. Mostly for tests and a first diagnostics.
QString redactedLog(const QString& marked);

} // namespace ts3
