#pragma once

// 2.6 chat proxy: the saved state (design 1.2, 2.1). Pure QtCore. FROZEN (design 3.1).
//
//  * on: the chat proxy (a custom HTTP or SOCKS5 proxy) is in use. Off: whenOff decides (the Windows proxy settings,
//    2.5.0's behaviour and the default; or no proxy at all).
//  * The endpoint (type, host, port, user name, the DPAPI-sealed password) is kept while off; nothing is retyped.
//  * Effective mode = on && saved ? Custom : whenOff. "Saved" = a host and a port 1..65535.
//
// The keys live in settings.ini next to Settings' own, but belong to proxy::Control alone (loadConfig / saveConfig):
// Settings::save() never touches them and the Settings dialog's Restore defaults never changes them. A Config never
// holds a plain password (protect.h).

#include <QByteArray>
#include <QString>

namespace proxy {

enum class Mode { Unset, System, Direct, Custom }; // Unset: before Control::start (router.h)
enum class Type { Socks5, Http };
enum class WhenOff { System, Direct };

// What the proxy's password is bound to (protect.h entropy): any change makes a sealed password unreadable.
struct Endpoint {
    Type    type = Type::Socks5;
    QString host; // normalizeHost()
    quint16 port = 0;
    QString user;

    bool operator==(const Endpoint& other) const { return type == other.type && host == other.host && port == other.port && user == other.user; }
    bool operator!=(const Endpoint& other) const { return !(*this == other); }
};

struct Config {
    bool       on      = false;
    WhenOff    whenOff = WhenOff::System;
    Type       type    = Type::Socks5;
    QString    host;         // normalizeHost(); empty: none
    quint16    port = 0;     // 1..65535; 0: none
    QString    user;         // empty: no login
    QByteArray passwordBlob; // "dpapi1:<base64>" (protect.h); empty: no password saved
    quint16    bridgePort = 0; // TS Media's own loopback port of the last session (proxyBridgePort); not part of the route

    bool     saved() const { return !host.isEmpty() && port != 0; }
    Mode     effectiveMode() const;
    Endpoint endpoint() const;
    // The published route would be the same (design 2.3): the effective mode, and in custom mode type, host, port,
    // user and blob. whenOff only counts while off.
    bool sameRoute(const Config& other) const;
};

// ---- the proxy's address ------------------------------------------------------------------------------------------

enum class HostError {
    None,
    Empty,
    Invalid, // not an IPv4 / IPv6 address (brackets allowed) or a host name; an unspecified, multicast or broadcast address
};
HostError hostError(const QString& text);
// Trimmed, IPv6 brackets removed, lower case; empty when hostError() isn't None.
QString normalizeHost(const QString& text);
// "127.0.0.1:2080", "[::1]:8080", "proxy.example.net:3128".
QString hostPort(const QString& host, quint16 port);

enum class Place { ThisPc, LocalNetwork, Elsewhere };
// ThisPc: 127.0.0.0/8, ::1, localhost. LocalNetwork: 10/8, 172.16/12, 192.168/16, 169.254/16, fc00::/7, fe80::/10.
// Elsewhere: anything else, names included.
Place placeOf(const QString& host);
// A proxy on this PC at TS Media's own bridge port: a loop (design 1.3.3, S17).
bool isBridgeSelfLoop(const QString& host, quint16 port, quint16 bridgePort);

// "SOCKS5 proxy 127.0.0.1:2080" / "HTTP proxy [::1]:8080". ForChat (chat lines, which TeamSpeak keeps on disk, F24):
// host:port only for a proxy on this PC, else "SOCKS5 proxy on the local network" / "HTTP proxy on another computer".
// Never the user name.
enum class Where { Window, ForChat };
QString describe(const Config& config, Where where);
QString typeName(Type type); // "SOCKS5", "HTTP"

// ---- settings.ini -------------------------------------------------------------------------------------------------

constexpr int kMaxUserBytes     = 255; // UTF-8
constexpr int kMaxPasswordBytes = 255; // UTF-8
constexpr int kMaxBlobChars     = 4096;

// Reads the proxy keys (design 2.1); anything invalid reads as its default. proxyOn=true without a saved endpoint loads
// as off; an http user name with ':' (or one over 255 bytes) loads empty and drops the password.
Config loadConfig(const QString& file);
// Writes the proxy keys only (keys via QString::fromLatin1, values owned by Qt); an empty blob removes proxyPassword.
void saveConfig(const QString& file, const Config& config);
// The user name rules of 2.1 for a type (at most 255 UTF-8 bytes; no ':' for http).
bool userValid(Type type, const QString& user);

} // namespace proxy
