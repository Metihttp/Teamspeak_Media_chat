#pragma once

// 2.6 chat proxy: the bytes on the wire, pure (no sockets): SOCKS5 (RFC 1928, RFC 1929) and HTTP CONNECT (RFC 9110
// 9.3.6) towards the user's proxy, and the bridge's own strict CONNECT head parser and target policy (design 3.4.3,
// S11). The bridge (bridge.cpp, upstream.cpp) and the tests use these; nothing here ever resolves a name.
//
// Secrets (the RFC 1929 message, the Basic header) are written into a caller's fixed buffer, which the caller wipes
// after the write (S18): no QByteArray or QString ever holds them.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "failure.h"

namespace proxy::wire {

constexpr int kMaxHeadBytes = 8 * 1024; // a request head (the bridge) or a CONNECT response head (an HTTP proxy)
constexpr int kMaxNameBytes = 253;      // a target name
constexpr int kMaxLoginBytes = 255;     // RFC 1929: user and password, each

// ---- SOCKS5 -------------------------------------------------------------------------------------------------------

QByteArray socksGreeting(bool withLogin); // 05 01 00, or 05 02 00 02

enum class MethodVerdict { NeedMore, Proceed, SendLogin, Fail };
// The method reply (2 bytes): 00 -> Proceed (*loginUnused when a login was offered: Throne answers 00 anyway);
// 02 with a login -> SendLogin; 02 without one or FF without one -> NeedsLogin; FF with a login -> LoginMethod;
// byte 0 != 05 or another method byte -> NotSocks5 (step Greeting, decisive).
MethodVerdict methodReply(const QByteArray& data, bool haveLogin, Failure* failure, bool* loginUnused);

// 01 ULEN USER PLEN PASS into out; the length, or -1 when a part is empty/over 255 bytes or out is too small.
int socksLoginMessage(const char* user, int userBytes, const char* password, int passwordBytes, char* out, int capacity);
// The RFC 1929 status (2 bytes): NeedMore, Proceed (status 0), or Fail with LoginRefused.
MethodVerdict loginReply(const QByteArray& data, Failure* failure);

// 05 01 00 03 <len> <name> 01 BB: always ATYP 3 (by name), port 443. Empty for a name that doesn't fit.
QByteArray socksConnectRequest(const QByteArray& name);

struct SocksReply {
    bool    complete = false; // the whole reply (BND address and port included) is in the data
    int     consumed = 0;     // its bytes; anything after them is the site's
    int     rep      = -1;
    Failure failure  = Failure::None; // None: REP 0
};
// 05 REP 00 ATYP BND: ATYP 1 (4 + 2), 4 (16 + 2), 3 (1 + len + 2). Byte 0 != 05 or an unknown ATYP: complete with
// NotSocks5 (step Request: not decisive).
SocksReply socksReply(const QByteArray& data);
Failure    failureForRep(int rep); // 0 None; 2 NotAllowed; 5 SiteRefused; 7, 8 NoNameConnect; 1, 3, 4, 6, other CantReachSite

// ---- HTTP CONNECT to the user's proxy ----------------------------------------------------------------------------

// "CONNECT <name>:443 HTTP/1.1\r\nHost: <name>:443\r\n" + (withLogin) "Proxy-Authorization: Basic b64(user:password)\r\n"
// + "\r\n" into out; the length or -1. No User-Agent.
int httpConnectRequest(const QByteArray& name, const char* user, int userBytes, const char* password, int passwordBytes, bool withLogin,
                       char* out, int capacity);

struct ResponseHead {
    bool   complete        = false; // "\r\n\r\n" seen
    bool   tooLarge        = false; // no end within kMaxHeadBytes
    int    headBytes       = 0;     // with the empty line
    bool   http1           = false; // the status line starts "HTTP/1."
    int    status          = 0;
    bool   basicOffered    = false; // a Proxy-Authenticate: Basic ...
    bool   connectionClose = false; // Connection: close or Proxy-Connection: close
    bool   http10          = false; // "HTTP/1.0": the connection closes unless it says keep-alive
    bool   keepAlive       = false; // Connection: keep-alive or Proxy-Connection: keep-alive
    qint64 contentLength   = -1;
    bool   chunked         = false;
    // The proxy keeps the connection after this answer (HTTP/1.1 unless it says close; HTTP/1.0 only with keep-alive).
    bool persistent() const { return !connectionClose && (!http10 || keepAlive); }
};
ResponseHead responseHead(const QByteArray& data);

// The challenge-first sequence (S8): pre-emptive Basic only to a proxy on this PC; elsewhere the CONNECT goes without
// credentials and Basic is sent once, after a 407 that offers it.
class ConnectFlow
{
  public:
    ConnectFlow(bool loopback, bool haveLogin);
    bool sendLogin() const { return m_sendLogin; } // the next CONNECT carries Proxy-Authorization
    enum class Next { Success, ResendSameConnection, ResendNewConnection, Fail };
    // After a complete head. *failure on Fail; *loginUnused on Success when a login was set and none was sent.
    Next onResponse(const ResponseHead& head, Failure* failure, bool* loginUnused);
    static constexpr qint64 kMaxChallengeBody = 64 * 1024;

  private:
    bool m_haveLogin = false;
    bool m_sendLogin = false;
};

// ---- the bridge's client side ------------------------------------------------------------------------------------

struct ClientHead {
    enum class Verdict {
        NeedMore,
        Ok,
        BadRequest,       // 400: malformed, too long, obs-fold, two Proxy-Authorization, a body, Transfer-Encoding
        MethodNotAllowed, // 405 without Proxy-Authenticate: any method but CONNECT (a web page gets no prompt)
        Forbidden,        // 403 refused: the authority's grammar or the target policy
        AuthRequired,     // 407 with the realm: no, or a wrong, Proxy-Authorization
    };
    Verdict verdict = Verdict::NeedMore;
    QString host;       // Ok: the validated, normalized name (the only thing ever forwarded)
    char    client = 0; // Ok: 'f', 'u', 'p' or 't'
    quint64 id     = 0;
    int     headBytes = 0; // Ok: the head's bytes; anything after them is the client's early data
};
// The CONNECT head in data (design 3.4.3, in order): the request line before any URL parsing ("CONNECT <authority>
// HTTP/1.1|1.0", single spaces, the method case-sensitive); the authority ^([a-z0-9_.-]{1,253}):443$ after ASCII
// lower-casing, one trailing dot removed; the headers (no obs-fold, at most one Proxy-Authorization: Basic with strict
// base64 of "tsm-<client>[<id>]:<token>", the token compared in constant time, Content-Length absent or 0, no
// Transfer-Encoding); then targetAllowed().
ClientHead clientHead(const QByteArray& data, const QByteArray& token, const QStringList& localSuffixes);

// The target rules (the Fetcher's own): net::isAllowedUrl("https://<host>/") (https, publicHostName: no IP literal of
// any form, at least 2 labels, LDH, no numeric last label, no local TLD), not under the PC's DNS suffixes, never the
// YouTube player's virtual host.
bool targetAllowed(const QString& host, const QStringList& localSuffixes);
constexpr char kPlayerVirtualHost[] = "io.github.metihttp.tsmediachat";

// "tsm-" + one of f u p t + an optional decimal id (at most 20 digits).
bool parseClientUser(const QByteArray& user, char* client, quint64* id);
bool constantTimeEquals(const QByteArray& a, const QByteArray& b);
bool decodeBase64Strict(const QByteArray& text, QByteArray* out);

// The bridge's answers: no body, Content-Length: 0, Connection: close, never an echo of the request (S17). realm only
// for 407; tag (the X-TSMedia-Proxy value: a failure name, "refused", "busy", "off") when not null.
QByteArray errorResponse(int status, const QByteArray& realm = QByteArray(), const char* tag = nullptr);
constexpr char kEstablished[] = "HTTP/1.1 200 Connection established\r\n\r\n";

} // namespace proxy::wire
