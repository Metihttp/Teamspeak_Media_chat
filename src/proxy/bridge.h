#pragma once

// 2.6 chat proxy: the bridge (design 3.4). An HTTP CONNECT proxy on 127.0.0.1 only, inside the plugin, that the
// Fetcher, the updater, the YouTube player and the Test speak to (with Basic tsm-<client><id>:<token>); only the bridge
// talks to the user's proxy (upstream.h). One per plugin session (proxy::Control), plus a temporary one per Test.
//
// Threads: pure Win32 + Winsock, no QObject, no Qt timer, no metatype. One listener thread (accept + stop events, a 5 s
// wake that prunes the handles of finished tunnels) and one thread per accepted connection. Every thread pins the DLL
// and ends with FreeLibraryAndExitThread; every wait is bounded and wakes on the stop event (and, once the tunnel has an
// upstream, on its generation's epoch event, so setUpstream() closes idle relayed tunnels at once, F4).
//
// Caps (S17): at most 8 connections whose head isn't authenticated yet (the 9th is closed at once), each with a 2 s
// head deadline; at most 64 authenticated tunnels (the 65th gets 503 busy). Idle relayed tunnels close after 120 s.
// Records (records.h): a ring of 256, created at accept and updated at every step.

#include <QByteArray>
#include <QHash>
#include <QStringList>

#include <functional>
#include <memory>

#include "records.h"
#include "upstream.h"

namespace proxy {

struct BridgeState;

class Bridge : public RecordSource
{
  public:
    struct Options {
        quint16 preferredPort = 0; // the saved proxyBridgePort; 0 or not bindable: an ephemeral port
        std::function<QStringList()> localSuffixes; // default: net::localDnsSuffixes()
        UpstreamOptions upstream;
        int headTimeoutMs = 2000;
        int maxUnauthenticated = 8;
        int maxTunnels  = 64;
        int idleMs      = 120 * 1000;
        int threadStackBytes = 64 * 1024; // reservation
    };
    struct Counters {
        int     tunnelsOpen = 0;  // authenticated tunnels now
        quint64 accepted    = 0;  // since start
        quint64 refused     = 0;  // 403
        quint64 busy        = 0;  // 503
        quint64 unauthenticated = 0; // 400 / 405 / 407 answers and connections dropped over the cap
        QHash<int, quint64> failures; // by Failure (route-level ones and Off / Internal)
    };

    // Binds 127.0.0.1 (SO_EXCLUSIVEADDRUSE, then the preferred port or an ephemeral one) and starts the listener. Null
    // with *error (a Winsock error, or -1 when Winsock or a thread is unusable) when it can't.
    static std::shared_ptr<Bridge> create(const Options& options, int* error = nullptr);
    ~Bridge() override; // stop(0) if still running: never waits

    quint16    port() const;
    QByteArray token() const; // 43 base64url characters
    QByteArray realm() const; // "tsmedia-<16 hex>"

    // A new generation of the user's proxy (spec), or none (null: new CONNECTs get 502 + X-TSMedia-Proxy: off). Every
    // tunnel of the earlier generation is closed through its epoch event.
    void setUpstream(std::unique_ptr<UpstreamSpec> spec);
    bool hasUpstream() const;

    // Stops accepting, then closes every tunnel: the listener first (nothing new after that), then the tunnels, at most
    // waitMs in all. True when every thread ended in time (a late one keeps its own pin and ends by itself).
    bool stop(int waitMs);
    bool isRunning() const;

    // RecordSource
    bool            outcome(char client, quint64 id, Record* out) const override;
    QVector<Record> recent(char client, quint64 sinceMs) const override;
    Counters        counters() const;

    // Tests and diagnostics.
    int liveThreads() const;   // listener + tunnels still running
    int openHandles() const;   // tunnel thread handles the listener still holds
    static int liveThreadsAll(); // every bridge of the process

  private:
    explicit Bridge(std::shared_ptr<BridgeState> state);
    std::shared_ptr<BridgeState> m_state;
};

// 32 random bytes (BCryptGenRandom) as base64url without padding (43 characters).
QByteArray newBridgeToken();
QByteArray newBridgeRealm(); // "tsmedia-" + 16 hex

} // namespace proxy
