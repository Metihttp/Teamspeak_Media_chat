#pragma once

// 2.2 foundation: one test executable, several test classes. A test file registers its class with
// TSMEDIA_REGISTER_TEST(Class); tst_tsmedia.cpp's TSMEDIA_TEST_MAIN runs TestTsMedia first, then every
// registered class. Test function names on the command line ("tsmedia_tests keyMatchesV1") run only in
// the classes that have them. A QApplication: some classes render text (the audio card tests), and the
// emoji tests use widgets.

#include <QApplication>
#include <QGuiApplication>
#include <QtTest>

#include <memory>
#include <vector>

namespace testmain {

using Factory = QObject* (*)();

inline std::vector<Factory>& registry()
{
    static std::vector<Factory> factories;
    return factories;
}

struct Registration {
    explicit Registration(Factory factory) { registry().push_back(factory); }
};

inline bool takesValue(const QString& option)
{
    return option == QLatin1String("-o") || option == QLatin1String("-maxwarnings") || option == QLatin1String("-eventdelay") || option == QLatin1String("-keydelay")
           || option == QLatin1String("-mousedelay");
}

// The command line for one test object: the options, and of the named test functions ("name" or
// "name:row") only those it has. Empty if functions were named and it has none of them.
inline QStringList argsFor(const QObject* test, const QStringList& args)
{
    QStringList out{args.value(0)};
    bool        named = false;
    bool        mine  = false;
    for (int i = 1; i < args.size(); ++i) {
        const QString& arg = args.at(i);
        if (arg.startsWith(QLatin1Char('-'))) {
            out << arg;
            if (takesValue(arg) && i + 1 < args.size())
                out << args.at(++i);
            continue;
        }
        named                  = true;
        const QByteArray slot = arg.section(QLatin1Char(':'), 0, 0).toLatin1() + "()";
        if (test->metaObject()->indexOfMethod(slot.constData()) >= 0) {
            out << arg;
            mine = true;
        }
    }
    return named && !mine ? QStringList() : out;
}

// "-o file,format" would be rewritten by every class: each writes its own part, and the parts are
// joined into the file at the end (in run order).
inline int run(QObject* first, int argc, char** argv)
{
    QStringList args;
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);
    QString target;
    int     outputArg = -1;
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) == QLatin1String("-o") && !args.at(i + 1).startsWith(QLatin1Char('-'))) {
            outputArg = i + 1;
            target    = args.at(i + 1);
        }
    }
    const QString path   = target.section(QLatin1Char(','), 0, 0);
    const QString format = target.contains(QLatin1Char(',')) ? QLatin1Char(',') + target.section(QLatin1Char(','), 1) : QString();

    std::vector<QObject*> tests{first};
    std::vector<std::unique_ptr<QObject>> owned;
    for (Factory factory : registry()) {
        owned.emplace_back(factory());
        tests.push_back(owned.back().get());
    }

    int         failed = 0;
    QStringList parts;
    for (size_t n = 0; n < tests.size(); ++n) {
        QStringList classArgs = argsFor(tests[n], args);
        if (classArgs.isEmpty())
            continue;
        if (outputArg > 0) {
            const QString part = path + QStringLiteral(".part%1").arg(n);
            const int     at   = classArgs.indexOf(target);
            if (at > 0)
                classArgs[at] = part + format;
            parts << part;
        }
        failed += QTest::qExec(tests[n], classArgs);
    }
    if (outputArg > 0) {
        QFile out(path);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            for (const QString& part : qAsConst(parts)) {
                QFile in(part);
                if (in.open(QIODevice::ReadOnly))
                    out.write(in.readAll());
                in.close();
                QFile::remove(part);
            }
        }
    }
    return failed;
}

} // namespace testmain

#define TSMEDIA_REGISTER_TEST(Class) static const testmain::Registration tsmediaRegister##Class([]() -> QObject* { return new Class; });

// 2.2 emoji: a QApplication (a QGuiApplication too), so tests can use TeamSpeak-like widgets (a chat
// browser and its input).
#define TSMEDIA_TEST_MAIN(Class)                          \
    int main(int argc, char* argv[])                      \
    {                                                     \
        QApplication app(argc, argv);                     \
        app.setAttribute(Qt::AA_Use96Dpi, true);          \
        Class tc;                                         \
        QTEST_SET_MAIN_SOURCE_PATH                        \
        return testmain::run(&tc, argc, argv);            \
    }
