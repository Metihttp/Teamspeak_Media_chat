// 2.2 emoji: developer tool for the emoji table (tools/emoji/gen_emoji.py).
//
//   emoji_probe <candidates.txt> <probe.txt>
//       For every candidate (hex code points joined by '-', one per line): 1 when Segoe UI Emoji draws it
//       as one colour picture on this PC (emoji::drawsInColor), else 0.
//   emoji_probe --sheet <out.png> [logical px] [dpr]
//       Every emoji of the table that is supported here, as a contact sheet (to look at the quality).
//   emoji_probe --ink
//       Ink boxes of a few emoji at 200 px (how they sit in their square).

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTextStream>

#include <cstdio>

#include "emojidata.h"
#include "emojirender.h"

namespace {

QString fromHex(const QByteArray& line)
{
    QString text;
    for (const QByteArray& part : line.trimmed().split('-')) {
        bool       ok = false;
        const uint cp = part.toUInt(&ok, 16);
        if (!ok || cp > 0x10FFFF)
            return {};
        if (QChar::requiresSurrogates(cp)) {
            text += QChar(QChar::highSurrogate(cp));
            text += QChar(QChar::lowSurrogate(cp));
        } else {
            text += QChar(static_cast<ushort>(cp));
        }
    }
    return text;
}

int probe(const QString& in, const QString& out)
{
    QFile source(in);
    QFile target(out);
    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "can't open the files\n");
        return 1;
    }
    std::printf("engine: %s\n", qPrintable(emoji::engineDescription()));
    if (!emoji::hasColor())
        return 1;
    QElapsedTimer timer;
    timer.start();
    int yes = 0;
    int all = 0;
    while (!source.atEnd()) {
        const QByteArray line = source.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const bool ok = emoji::drawsInColor(fromHex(line));
        target.write(line + (ok ? " 1\n" : " 0\n"));
        yes += ok ? 1 : 0;
        ++all;
    }
    std::printf("%d of %d candidates draw in colour (%lld ms)\n", yes, all, static_cast<long long>(timer.elapsed()));
    return 0;
}

int sheet(const QString& out, int px, qreal dpr)
{
    std::printf("engine: %s\n", qPrintable(emoji::engineDescription()));
    QVector<int> ids;
    QElapsedTimer timer;
    timer.start();
    for (int g = 0; g < emoji::kGroupCount; ++g) {
        for (int id : emoji::members(static_cast<emoji::Group>(g))) {
            if (emoji::supported(id))
                ids.append(id);
        }
    }
    const qint64 checked = timer.restart();
    if (qEnvironmentVariableIsSet("EMOJI_PROBE_ASYNC")) {
        // The worker thread draws them; the sheet is put together from the cache once all are there.
        for (int id : qAsConst(ids))
            emoji::requestImage(id, px, dpr, false);
        while (emoji::cacheInfo().pending > 0)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
        std::printf("worker drew %d in %lld ms\n", ids.size(), static_cast<long long>(timer.restart()));
    }
    const int    columns = 40;
    const int    cell    = px + 8;
    const int    rows    = (ids.size() + columns - 1) / columns;
    QImage       image(QSize(columns * cell, rows * cell) * dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(QColor(0xff, 0xff, 0xff));
    QPainter p(&image);
    for (int i = 0; i < ids.size(); ++i) {
        const QImage e = emoji::render(ids.at(i), px, dpr);
        p.drawImage(QPointF((i % columns) * cell + 4, (i / columns) * cell + 4), e);
    }
    p.end();
    const qint64 drawn = timer.elapsed();
    image.save(out);
    std::printf("%d emoji: support checked in %lld ms, drawn in %lld ms (%.3f ms each) -> %s\n", ids.size(), static_cast<long long>(checked),
                static_cast<long long>(drawn), ids.isEmpty() ? 0.0 : static_cast<double>(drawn) / ids.size(), qPrintable(out));
    return 0;
}

int ink()
{
    const char* const samples[] = {"\xF0\x9F\x98\x80", "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x94\xA5", "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88",
                                   "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", "\x31\xEF\xB8\x8F\xE2\x83\xA3"};
    for (const char* s : samples) {
        const QImage image = emoji::render(QString::fromUtf8(s), 200, 1.0);
        int          left = image.width(), right = -1, top = image.height(), bottom = -1;
        for (int y = 0; y < image.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(line[x]) > 16) {
                    left   = qMin(left, x);
                    right  = qMax(right, x);
                    top    = qMin(top, y);
                    bottom = qMax(bottom, y);
                }
            }
        }
        std::printf("%-40s ink x %d..%d  y %d..%d  (of 200)  colour=%d\n", emoji::wireCode(emoji::find(QString::fromUtf8(s))).constData(), left, right, top, bottom,
                    emoji::drawsInColor(QString::fromUtf8(s)) ? 1 : 0);
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    int               result = 2;
    if (args.size() >= 3 && args.at(1) == QLatin1String("--sheet"))
        result = sheet(args.at(2), args.size() > 3 ? args.at(3).toInt() : 32, args.size() > 4 ? args.at(4).toDouble() : 1.0);
    else if (args.size() >= 2 && args.at(1) == QLatin1String("--ink"))
        result = ink();
    else if (args.size() >= 3)
        result = probe(args.at(1), args.at(2));
    else
        std::fprintf(stderr, "usage: emoji_probe <candidates.txt> <probe.txt> | --sheet <out.png> [px] [dpr] | --ink\n");
    emoji::shutdown();
    return result;
}
