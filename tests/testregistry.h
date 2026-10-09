#pragma once

// Lets test classes in their own files run in the tsmedia_tests executable: a file registers its class
// with TSMEDIA_REGISTER_TEST(Class), and tst_tsmedia.cpp's main() runs every registered class after
// TestTsMedia. The exit code is the total number of failures.

#include <QList>
#include <QObject>
#include <QtTest>

#include <memory>

namespace tsmedia_tests {

using Factory = QObject* (*)();

inline QList<Factory>& registry()
{
    static QList<Factory> factories;
    return factories;
}

struct Registration {
    explicit Registration(Factory factory) { registry().append(factory); }
};

// "-o out.txt,txt" would be overwritten by every class: each registered class writes out_<Class>.txt.
inline QList<QByteArray> argumentsFor(const char* className, int argc, char** argv)
{
    QList<QByteArray> args;
    for (int i = 0; i < argc; ++i)
        args << QByteArray(argv[i]);
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) != "-o")
            continue;
        QByteArray&      spec  = args[i + 1];
        const int        comma = spec.lastIndexOf(',');
        QByteArray       file  = comma >= 0 ? spec.left(comma) : spec;
        const QByteArray rest  = comma >= 0 ? spec.mid(comma) : QByteArray();
        if (file == "-")
            continue;
        const int  dot   = file.lastIndexOf('.');
        const int  slash = qMax(file.lastIndexOf('/'), file.lastIndexOf('\\'));
        const auto tag   = QByteArray("_") + className;
        file             = dot > slash ? file.left(dot) + tag + file.mid(dot) : file + tag;
        spec             = file + rest;
    }
    return args;
}

inline int runRegistered(int argc, char** argv)
{
    int failed = 0;
    for (Factory factory : registry()) {
        std::unique_ptr<QObject> test(factory());
        QList<QByteArray>        args = argumentsFor(test->metaObject()->className(), argc, argv);
        QVector<char*>           pointers;
        for (QByteArray& arg : args)
            pointers << arg.data();
        failed += QTest::qExec(test.get(), pointers.size(), pointers.data());
    }
    return failed;
}

} // namespace tsmedia_tests

#define TSMEDIA_REGISTER_TEST(Class) \
    static const tsmedia_tests::Registration tsmediaRegistration##Class([]() -> QObject* { return new Class; })
