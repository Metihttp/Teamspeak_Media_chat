#pragma once

// 2.6 chat proxy: the bridge's side towards the user's proxy (design 3.4.4). Connects to the proxy and runs its
// handshake on one socket, always by name (SOCKS5 ATYP 3, CONNECT <name>:443): the target is never resolved here.
// Stop- and epoch-aware: every wait also wakes on the bridge's stop event and the generation's epoch event.
//
//  * The proxy's address: an IP literal is used as typed (unspecified, multicast and broadcast refused, and a
//    loopback address at the bridge's own port: a self-loop). A name is resolved (the only local lookup in custom
//    mode), cached per generation for 60 s; an answer made only of sinkhole addresses (10.10.34.0/24) is refused
//    before anything is sent (ProxyNameFiltered, S8).
//  * TCP: each address in order, each with its share of 6 s in all (connectSliceMs; Unreachable when none answered).
//    Handshake: 6 s after the connect, and 9 s from the client's head to our answer in all (Slow).
//  * The password (protect.h): opened once per generation, kept under CryptProtectMemory, revealed into a wiped buffer
//    for each handshake. An unreadable one fails before anything is sent (PasswordUnreadable); a transient DPAPI error
//    is Internal (tried again at the next tunnel).
//  * HTTP: pre-emptive Basic only to a proxy on this PC (127.0.0.0/8, ::1); elsewhere challenge-first (S8). The 407's
//    body is drained by count; a resend on the same connection that finds it closed before any byte goes once more on
//    a new connection (a proxy that closes after its 407 without saying so).

#include <windows.h>

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

#include "failure.h"
#include "linkpreview/netpolicy.h"
#include "protect.h"
#include "proxyconfig.h"
#include "winsock.h"

namespace proxy {

// What the bridge sends to (one generation of the user's proxy). The plaintext password never enters it: either the
// sealed blob (opened lazily, once) or an already opened secret (the Test's typed password; Control's check).
struct UpstreamSpec {
    quint64    generation = 0;
    Type       type       = Type::Socks5;
    QString    host; // normalized
    quint16    port = 0;
    QString    user;
    QByteArray passwordBlob;                       // "dpapi1:..." for endpoint(); empty: none
    std::shared_ptr<protect::SecretBytes> opened;  // when set: the password itself (moved in, wiped when the spec goes)

    Endpoint endpoint() const
    {
        Endpoint e;
        e.type = type;
        e.host = host;
        e.port = port;
        e.user = user;
        return e;
    }
};

// One generation's upstream, shared by its tunnels (bridge.cpp keeps it in a shared_ptr).
class UpstreamState
{
  public:
    explicit UpstreamState(UpstreamSpec spec);
    ~UpstreamState();
    UpstreamState(const UpstreamState&)            = delete;
    UpstreamState& operator=(const UpstreamState&) = delete;

    const UpstreamSpec& spec() const { return m_spec; }
    HANDLE              epoch() const { return m_epoch; } // set when a newer generation replaces this one
    bool                haveLogin() const { return !m_spec.user.isEmpty(); }
    // The password into *out: Ok (also with no password), Unreadable, Transient.
    protect::OpenResult password(protect::SecretBytes* out);
    // The proxy name's cached answer (60 s): false when there is none.
    bool cachedAddresses(QVector<linkpreview::net::Address>* out);
    void cacheAddresses(const QVector<linkpreview::net::Address>& addresses);

  private:
    UpstreamSpec                       m_spec;
    HANDLE                             m_epoch = nullptr;
    SRWLOCK                            m_lock  = SRWLOCK_INIT;
    bool                               m_opened = false;
    protect::OpenResult                m_openResult = protect::OpenResult::Ok;
    protect::ProtectedSecret           m_secret;
    QVector<linkpreview::net::Address> m_addresses;
    quint64                            m_addressesUntil = 0;
};

struct UpstreamOptions {
    int  connectMs   = 6000; // TCP, every address in all
    int  handshakeMs = 6000; // after the connect
    int  totalMs     = 9000; // from the client's head to our answer
    int  lookupMs    = 5000;
    bool treatAsRemote = false; // tests: challenge-first even against a loopback fake
    // tests: the proxy name's lookup (0 found, 1 not found, 2 canceled / timed out); default ws::lookup
    std::function<int(const QString& name, QVector<linkpreview::net::Address>* out, const HANDLE* cancel, int cancelCount, int timeoutMs)> resolver;
};

struct UpstreamResult {
    Failure     failure     = Failure::None;
    Step        step        = Step::None;
    int         code        = 0; // the proxy's HTTP status or SOCKS REP when it refused
    bool        loginUnused = false;
    QByteArray  early;           // bytes after the proxy's answer: the site's (forwarded after our 200)
    ws::Conn*   conn        = nullptr; // filled on success (the caller's)
};

// The TCP connect budget's share for the next of addressesLeft addresses (review fix: an address whose packets are
// dropped, such as a broken IPv6 path, no longer uses the whole budget): every address but the last gets leftMs divided
// by the addresses left, at least 1.5 s (never more than leftMs); the last gets all that is left.
quint64 connectSliceMs(quint64 leftMs, int addressesLeft);

// Connects conn to the proxy of state and opens a tunnel to name:443. stop and epoch wake every wait; onStep(step)
// reports each step as it starts (the record). headAt: when the client's head was complete (the 9 s budget).
UpstreamResult openUpstream(UpstreamState& state, const QByteArray& name, ws::Conn& conn, HANDLE stop, quint16 bridgePort, quint64 headAt,
                            const UpstreamOptions& options, const std::function<void(Step)>& onStep);

} // namespace proxy
