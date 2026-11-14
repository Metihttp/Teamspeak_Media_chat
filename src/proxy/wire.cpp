#include "wire.h"

#include <windows.h>

#include <QList>
#include <QUrl>

#include <cstring>

#include "linkpreview/linkurl.h"
#include "linkpreview/netpolicy.h"
#include "protect.h"

namespace proxy::wire {

namespace {

bool equalsCi(const QByteArray& a, const char* b)
{
    return a.compare(QByteArray(b), Qt::CaseInsensitive) == 0;
}

bool startsWithCi(const QByteArray& a, const char* prefix)
{
    const int n = static_cast<int>(strlen(prefix));
    return a.size() >= n && a.left(n).compare(QByteArray(prefix), Qt::CaseInsensitive) == 0;
}

QByteArray trimmedSpTab(const QByteArray& value)
{
    int b = 0, e = value.size();
    while (b < e && (value.at(b) == ' ' || value.at(b) == '\t'))
        ++b;
    while (e > b && (value.at(e - 1) == ' ' || value.at(e - 1) == '\t'))
        --e;
    return value.mid(b, e - b);
}

bool isDigits(const QByteArray& text)
{
    if (text.isEmpty())
        return false;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

// Appends text to out at *pos; false when it doesn't fit.
bool put(char* out, int capacity, int* pos, const char* text, int size)
{
    if (size < 0 || *pos + size > capacity)
        return false;
    memcpy(out + *pos, text, static_cast<size_t>(size));
    *pos += size;
    return true;
}

bool put(char* out, int capacity, int* pos, const QByteArray& text)
{
    return put(out, capacity, pos, text.constData(), text.size());
}

// The authority of a CONNECT line: lower-cased ASCII "<name>:443", one trailing dot of the name removed.
bool validAuthority(const QByteArray& authority, QString* host)
{
    if (authority.size() < 5 || authority.size() > kMaxNameBytes + 1 + 4)
        return false;
    QByteArray lower = authority.toLower();
    if (!lower.endsWith(":443"))
        return false;
    QByteArray name = lower.left(lower.size() - 4);
    if (name.isEmpty())
        return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false; // '#', '/', '\', '@', '%', '[', ':', a space, anything else
    }
    if (name.size() > kMaxNameBytes)
        return false;
    if (name.endsWith('.'))
        name.chop(1);
    if (name.isEmpty())
        return false;
    *host = QString::fromLatin1(name);
    return true;
}

} // namespace

// ---- SOCKS5 -------------------------------------------------------------------------------------------------------

QByteArray socksGreeting(bool withLogin)
{
    return withLogin ? QByteArray("\x05\x02\x00\x02", 4) : QByteArray("\x05\x01\x00", 3);
}

MethodVerdict methodReply(const QByteArray& data, bool haveLogin, Failure* failure, bool* loginUnused)
{
    *failure     = Failure::None;
    *loginUnused = false;
    if (data.size() < 2)
        return MethodVerdict::NeedMore;
    if (static_cast<quint8>(data.at(0)) != 0x05) {
        *failure = Failure::NotSocks5;
        return MethodVerdict::Fail;
    }
    const quint8 method = static_cast<quint8>(data.at(1));
    switch (method) {
    case 0x00:
        *loginUnused = haveLogin;
        return MethodVerdict::Proceed;
    case 0x02:
        if (haveLogin)
            return MethodVerdict::SendLogin;
        *failure = Failure::NeedsLogin; // we never offered 02: a proxy that insists on a login
        return MethodVerdict::Fail;
    case 0xFF:
        *failure = haveLogin ? Failure::LoginMethod : Failure::NeedsLogin;
        return MethodVerdict::Fail;
    default:
        *failure = Failure::NotSocks5;
        return MethodVerdict::Fail;
    }
}

int socksLoginMessage(const char* user, int userBytes, const char* password, int passwordBytes, char* out, int capacity)
{
    if (userBytes < 1 || userBytes > kMaxLoginBytes || passwordBytes < 1 || passwordBytes > kMaxLoginBytes)
        return -1;
    const int size = 3 + userBytes + passwordBytes;
    if (size > capacity)
        return -1;
    out[0] = 0x01;
    out[1] = static_cast<char>(userBytes);
    memcpy(out + 2, user, static_cast<size_t>(userBytes));
    out[2 + userBytes] = static_cast<char>(passwordBytes);
    memcpy(out + 3 + userBytes, password, static_cast<size_t>(passwordBytes));
    return size;
}

MethodVerdict loginReply(const QByteArray& data, Failure* failure)
{
    *failure = Failure::None;
    if (data.size() < 2)
        return MethodVerdict::NeedMore;
    if (data.at(1) != 0) {
        *failure = Failure::LoginRefused;
        return MethodVerdict::Fail;
    }
    return MethodVerdict::Proceed;
}

QByteArray socksConnectRequest(const QByteArray& name)
{
    if (name.isEmpty() || name.size() > 255)
        return {};
    QByteArray r("\x05\x01\x00\x03", 4);
    r.append(static_cast<char>(name.size()));
    r.append(name);
    r.append("\x01\xBB", 2); // 443
    return r;
}

Failure failureForRep(int rep)
{
    switch (rep) {
    case 0:
        return Failure::None;
    case 2:
        return Failure::NotAllowed;
    case 5:
        return Failure::SiteRefused;
    case 7:
    case 8:
        return Failure::NoNameConnect; // never retried with a local lookup
    default:
        return Failure::CantReachSite; // 1, 3, 4, 6 and anything else
    }
}

SocksReply socksReply(const QByteArray& data)
{
    SocksReply r;
    if (data.isEmpty())
        return r;
    if (static_cast<quint8>(data.at(0)) != 0x05) {
        r.complete = true;
        r.failure  = Failure::NotSocks5;
        return r;
    }
    if (data.size() < 4)
        return r;
    const quint8 atyp = static_cast<quint8>(data.at(3));
    int          need = 0;
    switch (atyp) {
    case 0x01:
        need = 4 + 4 + 2;
        break;
    case 0x04:
        need = 4 + 16 + 2;
        break;
    case 0x03:
        if (data.size() < 5)
            return r;
        need = 4 + 1 + static_cast<quint8>(data.at(4)) + 2;
        break;
    default:
        r.complete = true;
        r.failure  = Failure::NotSocks5;
        return r;
    }
    if (data.size() < need)
        return r;
    r.complete = true;
    r.consumed = need;
    r.rep      = static_cast<quint8>(data.at(1));
    r.failure  = failureForRep(r.rep);
    return r;
}

// ---- HTTP CONNECT -------------------------------------------------------------------------------------------------

int httpConnectRequest(const QByteArray& name, const char* user, int userBytes, const char* password, int passwordBytes, bool withLogin,
                       char* out, int capacity)
{
    if (name.isEmpty() || name.size() > kMaxNameBytes)
        return -1;
    int pos = 0;
    if (!put(out, capacity, &pos, QByteArray("CONNECT ") + name + ":443 HTTP/1.1\r\nHost: " + name + ":443\r\n"))
        return -1;
    if (withLogin) {
        if (userBytes < 0 || passwordBytes < 0 || userBytes > kMaxLoginBytes || passwordBytes > kMaxLoginBytes)
            return -1;
        // user:password in a local buffer, encoded straight into out, the local buffer wiped
        char      plain[2 * kMaxLoginBytes + 1];
        const int plainSize = userBytes + 1 + passwordBytes;
        memcpy(plain, user, static_cast<size_t>(userBytes));
        plain[userBytes] = ':';
        memcpy(plain + userBytes + 1, password, static_cast<size_t>(passwordBytes));
        bool ok = put(out, capacity, &pos, "Proxy-Authorization: Basic ", 27);
        int  n  = ok ? protect::base64Encode(plain, plainSize, out + pos, capacity - pos) : -1;
        SecureZeroMemory(plain, sizeof(plain));
        if (n < 0)
            return -1;
        pos += n;
        if (!put(out, capacity, &pos, "\r\n", 2))
            return -1;
    }
    if (!put(out, capacity, &pos, "\r\n", 2))
        return -1;
    return pos;
}

ResponseHead responseHead(const QByteArray& data)
{
    ResponseHead h;
    const int    end = data.indexOf("\r\n\r\n");
    if (end < 0) {
        h.tooLarge = data.size() > kMaxHeadBytes;
        // A status line that already isn't HTTP/1.x needs no more bytes to tell.
        if (data.size() >= 7 && !data.startsWith("HTTP/1.")) {
            h.complete = true;
            h.http1    = false;
        }
        return h;
    }
    if (end + 4 > kMaxHeadBytes) {
        h.tooLarge = true;
        return h;
    }
    h.complete                 = true;
    h.headBytes                = end + 4;
    const QList<QByteArray> lines = data.left(end).split('\n');
    QByteArray              status = lines.value(0);
    if (status.endsWith('\r'))
        status.chop(1);
    if (!status.startsWith("HTTP/1.") || status.size() < 12 || status.at(8) != ' ')
        return h; // not HTTP
    const QByteArray code = status.mid(9, 3);
    if (!isDigits(code) || (status.size() > 12 && status.at(12) != ' '))
        return h;
    h.http1  = true;
    h.http10 = status.startsWith("HTTP/1.0 ");
    h.status = code.toInt();
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray name  = trimmedSpTab(line.left(colon));
        const QByteArray value = trimmedSpTab(line.mid(colon + 1));
        if (equalsCi(name, "proxy-authenticate")) {
            if (startsWithCi(value, "basic") && (value.size() == 5 || value.at(5) == ' ' || value.at(5) == '\t'))
                h.basicOffered = true;
            if (value.contains(", Basic") || value.contains(",Basic"))
                h.basicOffered = true; // several challenges in one line
        } else if (equalsCi(name, "connection") || equalsCi(name, "proxy-connection")) {
            // Proxy-Connection: the old non-standard twin many proxies still send (review fix).
            const QByteArray lower = value.toLower();
            if (lower.contains("close"))
                h.connectionClose = true;
            if (lower.contains("keep-alive"))
                h.keepAlive = true;
        } else if (equalsCi(name, "content-length")) {
            if (isDigits(value) && value.size() <= 15)
                h.contentLength = value.toLongLong();
        } else if (equalsCi(name, "transfer-encoding")) {
            if (value.toLower().contains("chunked"))
                h.chunked = true;
        }
    }
    return h;
}

ConnectFlow::ConnectFlow(bool loopback, bool haveLogin)
    : m_haveLogin(haveLogin)
    , m_sendLogin(loopback && haveLogin)
{
}

ConnectFlow::Next ConnectFlow::onResponse(const ResponseHead& head, Failure* failure, bool* loginUnused)
{
    *failure     = Failure::None;
    *loginUnused = false;
    if (!head.http1) {
        *failure = Failure::NotHttp;
        return Next::Fail;
    }
    if (head.status >= 200 && head.status <= 299) {
        *loginUnused = m_haveLogin && !m_sendLogin;
        return Next::Success;
    }
    if (head.status == 407) {
        if (!m_haveLogin) {
            *failure = Failure::NeedsLogin;
            return Next::Fail;
        }
        if (!head.basicOffered) {
            *failure = Failure::LoginMethod;
            return Next::Fail;
        }
        if (m_sendLogin) {
            *failure = Failure::LoginRefused; // our Basic was refused
            return Next::Fail;
        }
        m_sendLogin = true;
        // The same connection only when the proxy keeps it (review fix: an HTTP/1.0 407 without keep-alive closes it)
        // and the body's length is known and small.
        const bool sameOne = head.persistent() && !head.chunked && head.contentLength >= 0 && head.contentLength <= kMaxChallengeBody;
        return sameOne ? Next::ResendSameConnection : Next::ResendNewConnection;
    }
    if (head.status == 502 || head.status == 503 || head.status == 504) {
        *failure = Failure::CantReachSite;
        return Next::Fail;
    }
    *failure = Failure::NotAllowed; // 403 and anything else
    return Next::Fail;
}

// ---- the bridge's client side ----------------------------------------------------------------------------------------

bool decodeBase64Strict(const QByteArray& text, QByteArray* out)
{
    out->clear();
    if (text.isEmpty() || text.size() % 4 != 0)
        return false;
    for (int i = 0; i < text.size(); ++i) {
        const char c = text.at(i);
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (alpha)
            continue;
        if (c == '=' && i >= text.size() - 2) {
            // padding only at the end, and nothing but padding after it
            for (int j = i; j < text.size(); ++j) {
                if (text.at(j) != '=')
                    return false;
            }
            break;
        }
        return false;
    }
    const QByteArray decoded = QByteArray::fromBase64(text);
    // Round trip: refuses non-canonical encodings (stray bits in the last character).
    if (decoded.toBase64() != text)
        return false;
    *out = decoded;
    return true;
}

bool parseClientUser(const QByteArray& user, char* client, quint64* id)
{
    if (user.size() < 5 || user.size() > 5 + 20 || !user.startsWith("tsm-"))
        return false;
    const char c = user.at(4);
    if (c != 'f' && c != 'u' && c != 'p' && c != 't')
        return false;
    const QByteArray digits = user.mid(5);
    quint64          value  = 0;
    if (!digits.isEmpty()) {
        if (!isDigits(digits))
            return false;
        bool ok = false;
        value   = digits.toULongLong(&ok);
        if (!ok)
            return false;
    }
    *client = c;
    *id     = value;
    return true;
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    // Over the full length of the expected token, whatever the other's length.
    const int n    = b.size();
    unsigned  diff = static_cast<unsigned>(a.size() ^ b.size());
    for (int i = 0; i < n; ++i) {
        const char x = i < a.size() ? a.at(i) : 0;
        diff |= static_cast<unsigned>(static_cast<quint8>(x ^ b.at(i)));
    }
    return diff == 0 && n > 0;
}

bool targetAllowed(const QString& host, const QStringList& localSuffixes)
{
    if (host.isEmpty() || host.size() > kMaxNameBytes)
        return false;
    const QUrl url(QLatin1String("https://") + host + QLatin1Char('/'), QUrl::StrictMode);
    if (!linkpreview::net::isAllowedUrl(url))
        return false;
    const QString name = linkpreview::publicHostName(url);
    if (name != host)
        return false; // only the validated, normalized string is ever forwarded
    if (name == QLatin1String(kPlayerVirtualHost) || name.endsWith(QLatin1Char('.') + QLatin1String(kPlayerVirtualHost)))
        return false; // WebView2 preconnects the player's virtual host
    return !linkpreview::net::underLocalSuffix(name, localSuffixes);
}

ClientHead clientHead(const QByteArray& data, const QByteArray& token, const QStringList& localSuffixes)
{
    ClientHead r;
    const int  end = data.indexOf("\r\n\r\n");
    if (end < 0) {
        if (data.size() > kMaxHeadBytes)
            r.verdict = ClientHead::Verdict::BadRequest;
        return r;
    }
    if (end + 4 > kMaxHeadBytes) {
        r.verdict = ClientHead::Verdict::BadRequest;
        return r;
    }
    const QByteArray        head  = data.left(end);
    const QList<QByteArray> lines = head.split('\n');
    // Every line ends with CRLF: a bare LF is malformed.
    for (int i = 0; i + 1 < lines.size(); ++i) {
        if (!lines.at(i).endsWith('\r')) {
            r.verdict = ClientHead::Verdict::BadRequest;
            return r;
        }
    }
    const auto lineAt = [&lines](int i) {
        QByteArray l = lines.at(i);
        if (l.endsWith('\r'))
            l.chop(1);
        return l;
    };

    // 1. The request line, before any URL parsing.
    const QByteArray        requestLine = lineAt(0);
    const QList<QByteArray> parts       = requestLine.split(' ');
    if (parts.size() != 3 || parts.at(0).isEmpty() || parts.at(1).isEmpty() || parts.at(2).isEmpty()) {
        r.verdict = ClientHead::Verdict::BadRequest;
        return r;
    }
    for (const char c : parts.at(0)) {
        if (c < 'A' || (c > 'Z' && c < 'a') || c > 'z') {
            r.verdict = ClientHead::Verdict::BadRequest;
            return r;
        }
    }
    if (parts.at(2) != "HTTP/1.1" && parts.at(2) != "HTTP/1.0") {
        r.verdict = ClientHead::Verdict::BadRequest;
        return r;
    }
    if (parts.at(0) != "CONNECT") {
        r.verdict = ClientHead::Verdict::MethodNotAllowed; // GET, POST, "connect", ...: no Proxy-Authenticate
        return r;
    }
    // 2. The authority.
    QString host;
    if (!validAuthority(parts.at(1), &host)) {
        r.verdict = ClientHead::Verdict::Forbidden;
        return r;
    }
    // 3. The headers.
    int        authCount = 0;
    QByteArray auth;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lineAt(i);
        if (line.isEmpty()) {
            r.verdict = ClientHead::Verdict::BadRequest;
            return r;
        }
        if (line.at(0) == ' ' || line.at(0) == '\t') {
            r.verdict = ClientHead::Verdict::BadRequest; // obs-fold
            return r;
        }
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            r.verdict = ClientHead::Verdict::BadRequest;
            return r;
        }
        const QByteArray name = line.left(colon);
        for (const char c : name) {
            if (c <= ' ' || c >= 127) {
                r.verdict = ClientHead::Verdict::BadRequest;
                return r;
            }
        }
        const QByteArray value = trimmedSpTab(line.mid(colon + 1));
        if (equalsCi(name, "proxy-authorization")) {
            ++authCount;
            auth = value;
        } else if (equalsCi(name, "content-length")) {
            if (value != "0") {
                r.verdict = ClientHead::Verdict::BadRequest;
                return r;
            }
        } else if (equalsCi(name, "transfer-encoding")) {
            r.verdict = ClientHead::Verdict::BadRequest;
            return r;
        }
    }
    if (authCount > 1) {
        r.verdict = ClientHead::Verdict::BadRequest;
        return r;
    }
    // 4. The login: Basic tsm-<client>[<id>]:<token>.
    bool authOk = false;
    if (authCount == 1 && startsWithCi(auth, "basic ")) {
        const QByteArray b64 = trimmedSpTab(auth.mid(6));
        QByteArray       decoded;
        if (decodeBase64Strict(b64, &decoded)) {
            const int colon = decoded.indexOf(':');
            if (colon > 0) {
                char    client = 0;
                quint64 id     = 0;
                if (parseClientUser(decoded.left(colon), &client, &id) && constantTimeEquals(decoded.mid(colon + 1), token)) {
                    authOk   = true;
                    r.client = client;
                    r.id     = id;
                }
            }
        }
    }
    if (!authOk) {
        r.client  = 0;
        r.id      = 0;
        r.verdict = ClientHead::Verdict::AuthRequired;
        return r;
    }
    // 5. The target policy.
    if (!targetAllowed(host, localSuffixes)) {
        r.verdict = ClientHead::Verdict::Forbidden;
        return r;
    }
    r.verdict   = ClientHead::Verdict::Ok;
    r.host      = host;
    r.headBytes = end + 4;
    return r;
}

QByteArray errorResponse(int status, const QByteArray& realm, const char* tag)
{
    const char* reason = "Error";
    switch (status) {
    case 400:
        reason = "Bad Request";
        break;
    case 403:
        reason = "Forbidden";
        break;
    case 405:
        reason = "Method Not Allowed";
        break;
    case 407:
        reason = "Proxy Authentication Required";
        break;
    case 502:
        reason = "Bad Gateway";
        break;
    case 503:
        reason = "Service Unavailable";
        break;
    default:
        break;
    }
    QByteArray r = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    if (status == 407 && !realm.isEmpty())
        r += "Proxy-Authenticate: Basic realm=\"" + realm + "\"\r\n";
    if (tag)
        r += QByteArray("X-TSMedia-Proxy: ") + tag + "\r\n";
    r += "Content-Length: 0\r\nConnection: close\r\n\r\n";
    return r;
}

} // namespace proxy::wire
