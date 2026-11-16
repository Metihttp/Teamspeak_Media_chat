#include "darkglyphs.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>

// 2.7 Dark mode (design_dark 2.5): the drawing follows the prototype (darkproto.cpp makeGlyphs). Paths and names are
// built with fromLatin1 (CRASH RULE: they end up in the application style sheet).

namespace darkglyphs {

namespace {

constexpr int kGlyphSet = 1; // raise when a drawing changes: every folder is drawn again

enum class Kind { Box, Radio, Chevron, Close, MenuCheck, MenuDot };

struct Spec {
    const char* name;
    Kind        kind;
    qreal       size;      // at k = 1
    QColor darkpalette::Tokens::*fill;
    QColor darkpalette::Tokens::*edge;
    QColor darkpalette::Tokens::*mark; // nullptr: none
    int         variant;   // Box: 0 empty, 1 check, 2 dash; Chevron: 0 down, 1 up, 2 right, 3 left
};

using T = darkpalette::Tokens;

const Spec kSpecs[] = {
    {"cb-off", Kind::Box, 16, &T::field, &T::control, nullptr, 0},
    {"cb-off-hover", Kind::Box, 16, &T::raisedHover, &T::controlHover, nullptr, 0},
    {"cb-on", Kind::Box, 16, &T::accent, &T::accent, &T::onAccent, 1},
    {"cb-on-hover", Kind::Box, 16, &T::accentHover, &T::accentHover, &T::onAccent, 1},
    {"cb-mixed", Kind::Box, 16, &T::accent, &T::accent, &T::onAccent, 2},
    {"cb-off-dis", Kind::Box, 16, &T::disabledFill, &T::border, nullptr, 0},
    {"cb-on-dis", Kind::Box, 16, &T::accentOff, &T::accentOff, &T::onAccentOff, 1},
    {"cb-mixed-dis", Kind::Box, 16, &T::accentOff, &T::accentOff, &T::onAccentOff, 2},
    {"rb-off", Kind::Radio, 16, &T::field, &T::control, nullptr, 0},
    {"rb-off-hover", Kind::Radio, 16, &T::raisedHover, &T::controlHover, nullptr, 0},
    {"rb-on", Kind::Radio, 16, &T::accent, &T::accent, &T::onAccent, 0},
    {"rb-on-hover", Kind::Radio, 16, &T::accentHover, &T::accentHover, &T::onAccent, 0},
    {"rb-off-dis", Kind::Radio, 16, &T::disabledFill, &T::border, nullptr, 0},
    {"rb-on-dis", Kind::Radio, 16, &T::accentOff, &T::accentOff, &T::onAccentOff, 0},
    {"chev-down", Kind::Chevron, 10, nullptr, &T::secondary, nullptr, 0},
    {"chev-up", Kind::Chevron, 10, nullptr, &T::secondary, nullptr, 1},
    {"chev-right", Kind::Chevron, 10, nullptr, &T::secondary, nullptr, 2},
    {"chev-left", Kind::Chevron, 10, nullptr, &T::secondary, nullptr, 3},
    {"chev-down-dis", Kind::Chevron, 10, nullptr, &T::disabled, nullptr, 0},
    {"chev-down-sm", Kind::Chevron, 8, nullptr, &T::secondary, nullptr, 0},
    {"chev-up-sm", Kind::Chevron, 8, nullptr, &T::secondary, nullptr, 1},
    {"chev-down-sm-dis", Kind::Chevron, 8, nullptr, &T::disabled, nullptr, 0},
    {"chev-up-sm-dis", Kind::Chevron, 8, nullptr, &T::disabled, nullptr, 1},
    {"close", Kind::Close, 12, nullptr, &T::secondary, nullptr, 0},
    {"mcheck", Kind::MenuCheck, 14, nullptr, &T::text, nullptr, 0},
    {"mdot", Kind::MenuDot, 14, nullptr, &T::text, nullptr, 0},
};

QImage draw(const Spec& spec, const darkpalette::Tokens& t, qreal k)
{
    const int size = qMax(1, darkpalette::px(spec.size, k));
    QImage    image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal  s    = size;
    const qreal  lw   = qMax<qreal>(1.0, k);
    const QColor edge = t.*(spec.edge);
    switch (spec.kind) {
    case Kind::Box:
    case Kind::Radio: {
        const qreal  inset = 1.0 * k + lw / 2;
        const QRectF r(inset, inset, s - 2 * inset, s - 2 * inset);
        p.setPen(QPen(edge, lw));
        p.setBrush(t.*(spec.fill));
        if (spec.kind == Kind::Box) {
            p.drawRoundedRect(r, 3.5 * k, 3.5 * k);
            if (spec.mark && spec.variant != 0) {
                p.setPen(QPen(t.*(spec.mark), 1.9 * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                if (spec.variant == 1) {
                    QPainterPath path;
                    path.moveTo(s * 0.28, s * 0.52);
                    path.lineTo(s * 0.44, s * 0.67);
                    path.lineTo(s * 0.73, s * 0.35);
                    p.drawPath(path);
                } else {
                    p.drawLine(QPointF(s * 0.3, s * 0.5), QPointF(s * 0.7, s * 0.5));
                }
            }
        } else {
            p.drawEllipse(r);
            if (spec.mark) {
                p.setPen(Qt::NoPen);
                p.setBrush(t.*(spec.mark));
                p.drawEllipse(QPointF(s / 2, s / 2), 3.0 * k, 3.0 * k);
            }
        }
        break;
    }
    case Kind::Chevron: {
        p.setPen(QPen(edge, qMax<qreal>(1.2, 1.5 * k), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.translate(s / 2, s / 2);
        p.rotate(spec.variant == 0 ? 0 : spec.variant == 1 ? 180 : spec.variant == 2 ? -90 : 90);
        const qreal  w = s * 0.32, h = s * 0.17;
        QPainterPath path;
        path.moveTo(-w, -h);
        path.lineTo(0, h);
        path.lineTo(w, -h);
        p.drawPath(path);
        break;
    }
    case Kind::Close: {
        p.setPen(QPen(edge, qMax<qreal>(1.2, 1.4 * k), Qt::SolidLine, Qt::RoundCap));
        const qreal a = s * 0.3, b = s * 0.7;
        p.drawLine(QPointF(a, a), QPointF(b, b));
        p.drawLine(QPointF(b, a), QPointF(a, b));
        break;
    }
    case Kind::MenuCheck: {
        p.setPen(QPen(edge, 1.8 * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path;
        path.moveTo(s * 0.22, s * 0.52);
        path.lineTo(s * 0.42, s * 0.7);
        path.lineTo(s * 0.78, s * 0.3);
        p.drawPath(path);
        break;
    }
    case Kind::MenuDot:
        p.setPen(Qt::NoPen);
        p.setBrush(edge);
        p.drawEllipse(QPointF(s / 2, s / 2), 3 * k, 3 * k);
        break;
    }
    p.end();
    return image;
}

QString folderFor(const QString& baseDir, qreal k)
{
    return QDir::fromNativeSeparators(baseDir) + QString::fromLatin1("/darkmode/") + QString::number(qRound(k * 100));
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace

QStringList names()
{
    QStringList out;
    for (const Spec& s : kSpecs)
        out << QString::fromLatin1(s.name);
    return out;
}

int sizeOf(const QString& name, qreal k)
{
    for (const Spec& s : kSpecs) {
        if (name == QLatin1String(s.name))
            return qMax(1, darkpalette::px(s.size, k));
    }
    return 0;
}

QString stamp(qreal k, const darkpalette::Tokens& tokens)
{
    QByteArray colours;
    for (const Spec& s : kSpecs) {
        for (QColor darkpalette::Tokens::*field : {s.fill, s.edge, s.mark}) {
            if (field)
                colours += (tokens.*field).name(QColor::HexArgb).toLatin1();
        }
        colours += ';';
    }
    const QByteArray hash = QCryptographicHash::hash(colours, QCryptographicHash::Sha1).toHex().left(16);
    return QString::fromLatin1("glyphs %1 k %2 colors %3").arg(kGlyphSet).arg(qRound(k * 100)).arg(QString::fromLatin1(hash));
}

QString ensure(const QString& baseDir, qreal k, const darkpalette::Tokens& tokens)
{
    if (baseDir.isEmpty())
        return QString();
    const qreal   scale  = k > 0 ? k : 1.0;
    const QString folder = folderFor(baseDir, scale);
    const QString want   = stamp(scale, tokens);
    const QString marker = folder + QString::fromLatin1("/glyphs.txt");
    {
        QFile file(marker);
        if (file.open(QIODevice::ReadOnly) && QString::fromLatin1(file.readAll()).trimmed() == want) {
            bool complete = true;
            for (const Spec& s : kSpecs)
                complete = complete && QFileInfo::exists(folder + QLatin1Char('/') + QString::fromLatin1(s.name) + QString::fromLatin1(".png"));
            if (complete)
                return folder;
        }
    }
    if (!QDir().mkpath(folder))
        return QString();
    for (const Spec& s : kSpecs) {
        QByteArray bytes;
        {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (!draw(s, tokens, scale).save(&buffer, "PNG"))
                return QString();
        }
        if (!writeFile(folder + QLatin1Char('/') + QString::fromLatin1(s.name) + QString::fromLatin1(".png"), bytes))
            return QString();
    }
    if (!writeFile(marker, want.toLatin1() + '\n'))
        return QString();
    return folder;
}

} // namespace darkglyphs
