#include "logtext.h"

#include <QHash>

namespace ts3 {

namespace {

// A value can never carry markers of its own (it could fake or end a span).
QString withoutMarkers(QString text)
{
    text.remove(kLogMarkStart);
    text.remove(kLogMarkEnd);
    return text;
}

QString oneLine(QString text)
{
    text.replace(QLatin1String("\r\n"), QLatin1String(" "));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QChar(0x2028), QLatin1Char(' '));
    text.replace(QChar(0x2029), QLatin1Char(' '));
    return text;
}

QString markedSpan(char kind, const QString& text)
{
    return kLogMarkStart + QLatin1Char(kind) + text + kLogMarkEnd;
}

} // namespace

LogArg::LogArg(char kind, const QString& text)
    : m_kind(kind)
    , m_text(withoutMarkers(text))
{
}

LogArg pub(const QString& text)
{
    return LogArg(0, text);
}

LogArg file(const QString& text)
{
    return LogArg('f', text);
}

LogArg local(const QString& text)
{
    return LogArg('l', text);
}

LogArg name(const QString& text)
{
    return LogArg('n', text);
}

LogText formatLog(const char* format, std::initializer_list<LogArg> args)
{
    const QString fmt = withoutMarkers(QString::fromUtf8(format ? format : ""));
    LogText       out;
    out.plain.reserve(fmt.size() + 32);
    out.marked.reserve(fmt.size() + 32);
    for (int i = 0; i < fmt.size(); ++i) {
        const QChar c = fmt.at(i);
        if (c == QLatin1Char('%') && i + 1 < fmt.size()) {
            const int n = fmt.at(i + 1).unicode() - '0';
            if (n >= 1 && n <= 9 && static_cast<size_t>(n) <= args.size()) {
                const LogArg& arg = *(args.begin() + (n - 1));
                out.plain += arg.text();
                out.marked += arg.kind() ? markedSpan(arg.kind(), arg.text()) : arg.text();
                ++i;
                continue;
            }
        }
        out.plain += c;
        out.marked += c;
    }
    out.plain  = oneLine(out.plain);
    out.marked = oneLine(out.marked);
    return out;
}

LogText unclassifiedLog(const QString& text)
{
    LogText out;
    out.plain  = oneLine(withoutMarkers(text));
    out.marked = out.plain.isEmpty() ? QString() : markedSpan('u', out.plain);
    return out;
}

LogText quotedNamesLog(const QString& text)
{
    LogText out;
    out.plain = oneLine(withoutMarkers(text));
    const QChar open(0x201C);
    const QChar close(0x201D);
    for (int from = 0; from < out.plain.size();) {
        const int start = out.plain.indexOf(open, from);
        if (start < 0) {
            out.marked += out.plain.midRef(from);
            break;
        }
        const int end = out.plain.indexOf(close, start + 1);
        if (end < 0) { // no closing quote: the rest may be a name
            out.marked += out.plain.midRef(from, start + 1 - from);
            out.marked += markedSpan('n', out.plain.mid(start + 1));
            break;
        }
        out.marked += out.plain.midRef(from, start + 1 - from);
        out.marked += markedSpan('n', out.plain.mid(start + 1, end - start - 1));
        out.marked += close;
        from = end + 1;
    }
    return out;
}

QString redactedLog(const QString& marked)
{
    QHash<QString, int> numbers; // the same value keeps its number
    QString             out;
    for (int i = 0; i < marked.size(); ++i) {
        if (marked.at(i) != kLogMarkStart) {
            if (marked.at(i) != kLogMarkEnd)
                out += marked.at(i);
            continue;
        }
        const int end = marked.indexOf(kLogMarkEnd, i + 1);
        if (end < 0 || end == i + 1) {
            out += QLatin1String("<hidden>"); // unbalanced: fail closed, to the end of the line
            break;
        }
        const QChar   kind  = marked.at(i + 1);
        const QString value = marked.mid(i + 2, end - i - 2);
        const QString word  = kind == QLatin1Char('f') ? QStringLiteral("file") : kind == QLatin1Char('l') ? QStringLiteral("path") : kind == QLatin1Char('n') ? QStringLiteral("name") : QStringLiteral("text");
        const QString key   = word + QLatin1Char('\n') + value;
        if (!numbers.contains(key)) {
            int count = 0;
            for (auto it = numbers.cbegin(); it != numbers.cend(); ++it)
                count += it.key().startsWith(word + QLatin1Char('\n')) ? 1 : 0;
            numbers.insert(key, count + 1);
        }
        out += QStringLiteral("<%1 %2>").arg(word).arg(numbers.value(key));
        i = end;
    }
    return out;
}

} // namespace ts3
