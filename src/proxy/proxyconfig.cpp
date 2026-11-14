#include "proxyconfig.h"

#include <QSettings>
#include <QStringList>

#include "i18n.h"
#include "linkpreview/netpolicy.h"

namespace proxy {

namespace {

// Keys and values allocated by Qt, never literal data of the DLL image (settings.cpp: QSettings outlives the DLL).
QString key(const char* name)
{
    return QString::fromLatin1(name);
}

QString ownedCopy(const QString& text)
{
    return text.isNull() ? QString() : QString(text.constData(), text.size());
}

bool onlyChars(const QString& text, const char* allowed)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u > 0x7f)
            return false;
        bool found = false;
        for (const char* a = allowed; *a; ++a) {
            if (*a == static_cast<char>(u)) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

bool isDigits(const QString& text)
{
    if (text.isEmpty())
        return false;
    for (const QChar ch : text) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return false;
    }
    return true;
}

// An address the proxy can't be: unspecified, multicast, broadcast.
bool unusableAddress(const linkpreview::net::Address& a)
{
    using linkpreview::net::Address;
    if (a.family == Address::Family::IPv4) {
        const quint32 v = a.ipv4();
        return v == 0 || (v >> 28) == 0xE || v == 0xFFFFFFFFu;
    }
    if (a.family == Address::Family::IPv6) {
        bool zero = true;
        for (quint8 b : a.bytes)
            zero = zero && b == 0;
        return zero || a.bytes[0] == 0xff;
    }
    return true;
}

// The address in text (no brackets, no port), or null.
linkpreview::net::Address literalOf(const QString& host)
{
    using linkpreview::net::Address;
    if (host.isEmpty())
        return {};
    if (host.contains(QLatin1Char(':'))) {
        if (host.count(QLatin1Char(':')) < 2 || !onlyChars(host, "0123456789abcdefABCDEF:."))
            return {};
        const Address a = linkpreview::net::parseAddress(host);
        return a.family == Address::Family::IPv6 ? a : Address();
    }
    if (!onlyChars(host, "0123456789."))
        return {};
    const Address a = linkpreview::net::parseAddress(host);
    return a.family == Address::Family::IPv4 ? a : Address();
}

bool validName(const QString& host)
{
    if (host.isEmpty() || host.size() > 253 || !onlyChars(host, "abcdefghijklmnopqrstuvwxyz0123456789-."))
        return false;
    QString h = host;
    if (h.endsWith(QLatin1Char('.')))
        h.chop(1);
    const QStringList labels = h.split(QLatin1Char('.'));
    for (const QString& label : labels) {
        if (label.isEmpty() || label.size() > 63 || label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-')))
            return false;
    }
    return !isDigits(labels.last()); // "1.2.3", "999.1.1.1": addresses, not names
}

QString stripped(const QString& text)
{
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']')) && s.size() > 2)
        s = s.mid(1, s.size() - 2);
    return s.toLower();
}

const char* typeKey(Type type)
{
    return type == Type::Http ? "http" : "socks5";
}

bool blobShapeOk(const QByteArray& blob)
{
    if (!blob.startsWith("dpapi1:") || blob.size() > kMaxBlobChars || blob.size() <= 7)
        return false;
    for (int i = 7; i < blob.size(); ++i) {
        const char c = blob.at(i);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
            return false;
    }
    return (blob.size() - 7) % 4 == 0;
}

} // namespace

Mode Config::effectiveMode() const
{
    if (on && saved())
        return Mode::Custom;
    return whenOff == WhenOff::Direct ? Mode::Direct : Mode::System;
}

Endpoint Config::endpoint() const
{
    Endpoint e;
    e.type = type;
    e.host = host;
    e.port = port;
    e.user = user;
    return e;
}

bool Config::sameRoute(const Config& other) const
{
    const Mode mine = effectiveMode();
    if (mine != other.effectiveMode())
        return false;
    if (mine != Mode::Custom)
        return true;
    return endpoint() == other.endpoint() && passwordBlob == other.passwordBlob;
}

HostError hostError(const QString& text)
{
    const QString s = stripped(text);
    if (s.isEmpty())
        return HostError::Empty;
    const bool bracketed = text.trimmed().startsWith(QLatin1Char('['));
    const linkpreview::net::Address literal = literalOf(s);
    if (!literal.isNull())
        return unusableAddress(literal) ? HostError::Invalid : HostError::None;
    if (bracketed)
        return HostError::Invalid; // "[name]"
    return validName(s) ? HostError::None : HostError::Invalid;
}

QString normalizeHost(const QString& text)
{
    if (hostError(text) != HostError::None)
        return {};
    QString s = stripped(text);
    if (literalOf(s).isNull() && s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s;
}

QString hostPort(const QString& host, quint16 port)
{
    const QString h = host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host;
    return h + QLatin1Char(':') + QString::number(port);
}

Place placeOf(const QString& host)
{
    using linkpreview::net::Address;
    const QString h = stripped(host);
    if (h == QLatin1String("localhost"))
        return Place::ThisPc;
    const Address a = literalOf(h);
    if (a.family == Address::Family::IPv4) {
        const quint32 v = a.ipv4();
        if ((v >> 24) == 127)
            return Place::ThisPc;
        if ((v >> 24) == 10 || (v >> 20) == ((172u << 4) | 1u) || (v >> 16) == ((192u << 8) | 168u) || (v >> 16) == ((169u << 8) | 254u))
            return Place::LocalNetwork;
        return Place::Elsewhere;
    }
    if (a.family == Address::Family::IPv6) {
        bool loopback = a.bytes[15] == 1;
        for (int i = 0; i < 15; ++i)
            loopback = loopback && a.bytes[static_cast<size_t>(i)] == 0;
        if (loopback)
            return Place::ThisPc;
        if ((a.bytes[0] & 0xfe) == 0xfc || (a.bytes[0] == 0xfe && (a.bytes[1] & 0xc0) == 0x80))
            return Place::LocalNetwork;
        return Place::Elsewhere;
    }
    return Place::Elsewhere;
}

bool isBridgeSelfLoop(const QString& host, quint16 port, quint16 bridgePort)
{
    return bridgePort != 0 && port == bridgePort && placeOf(host) == Place::ThisPc;
}

QString typeName(Type type)
{
    return type == Type::Http ? i18n::t("HTTP") : i18n::t("SOCKS5");
}

QString describe(const Config& config, Where where)
{
    const bool http = config.type == Type::Http;
    if (where == Where::ForChat) {
        switch (placeOf(config.host)) {
        case Place::ThisPc:
            break;
        case Place::LocalNetwork:
            return http ? i18n::t("HTTP proxy on the local network") : i18n::t("SOCKS5 proxy on the local network");
        case Place::Elsewhere:
            return http ? i18n::t("HTTP proxy on another computer") : i18n::t("SOCKS5 proxy on another computer");
        }
    }
    const QString where_ = hostPort(config.host, config.port);
    return (http ? i18n::t("HTTP proxy %1") : i18n::t("SOCKS5 proxy %1")).arg(where_);
}

bool userValid(Type type, const QString& user)
{
    if (user.toUtf8().size() > kMaxUserBytes)
        return false;
    for (const QChar ch : user) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
            return false;
    }
    return !(type == Type::Http && user.contains(QLatin1Char(':')));
}

Config loadConfig(const QString& file)
{
    const QSettings s(file, QSettings::IniFormat);
    Config          c;
    c.on                  = s.value(key("proxyOn")).toString() == QLatin1String("true");
    c.whenOff             = s.value(key("proxyWhenOff")).toString() == QLatin1String("direct") ? WhenOff::Direct : WhenOff::System;
    c.type                = s.value(key("proxyType")).toString() == QLatin1String("http") ? Type::Http : Type::Socks5;
    c.host                = normalizeHost(s.value(key("proxyHost")).toString().left(300));
    bool      ok          = false;
    const int port        = s.value(key("proxyPort")).toString().toInt(&ok);
    c.port                = ok && port >= 1 && port <= 65535 ? static_cast<quint16>(port) : 0;
    const int bridge      = s.value(key("proxyBridgePort")).toString().toInt(&ok);
    c.bridgePort          = ok && bridge >= 1024 && bridge <= 65535 ? static_cast<quint16>(bridge) : 0;
    const QByteArray blob = s.value(key("proxyPassword")).toString().left(kMaxBlobChars + 1).toLatin1();
    c.passwordBlob        = blobShapeOk(blob) ? blob : QByteArray();
    const QString user    = s.value(key("proxyUser")).toString();
    if (userValid(c.type, user)) {
        c.user = user;
    } else {
        c.user.clear();
        c.passwordBlob.clear(); // a password saved for another login is never sent with an empty one
    }
    if (!c.saved())
        c.on = false;
    return c;
}

void saveConfig(const QString& file, const Config& config)
{
    QSettings s(file, QSettings::IniFormat);
    s.setValue(key("proxyOn"), key(config.on && config.saved() ? "true" : "false"));
    s.setValue(key("proxyWhenOff"), key(config.whenOff == WhenOff::Direct ? "direct" : "system"));
    s.setValue(key("proxyType"), key(typeKey(config.type)));
    s.setValue(key("proxyHost"), ownedCopy(normalizeHost(config.host)));
    s.setValue(key("proxyPort"), QString::number(config.port));
    s.setValue(key("proxyUser"), ownedCopy(userValid(config.type, config.user) ? config.user : QString()));
    if (blobShapeOk(config.passwordBlob))
        s.setValue(key("proxyPassword"), QString::fromLatin1(config.passwordBlob));
    else
        s.remove(key("proxyPassword"));
    s.setValue(key("proxyBridgePort"), QString::number(config.bridgePort >= 1024 ? config.bridgePort : 0));
    s.sync();
}

} // namespace proxy
