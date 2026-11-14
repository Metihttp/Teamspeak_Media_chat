#pragma once

// 2.6 chat proxy: the one route resolver (design 3.2). proxy::Control (GUI thread) is the only writer: it publishes an
// immutable Snapshot whenever the route changes. Readers on any thread take a copy of the shared_ptr and use it for one
// request (a Fetcher hop, an updater request, a player open). The Fetcher and the transport never read Settings.
//
// The initial snapshot refuses everything (Unset / NotReady), and init() puts it back on every plugin load: anything
// that read it before Control::start would fail closed, never take the Windows route by default (S1).
// Pure QtCore + Win32 (an SRWLOCK guards the pointer). FROZEN (design 3.1).

#include <QByteArray>

#include <memory>

#include "failure.h"
#include "proxyconfig.h"
#include "records.h"

namespace proxy {

// How one request leaves the PC (transport.h Route::Via): Unset (nobody decided: refused, never direct by default),
// direct, the Windows proxy, the bridge (the chat proxy), or refused.
enum class Via { Unset, Direct, System, Bridge, Refused };

struct Snapshot {
    Mode    mode       = Mode::Unset; // Unset: before Control::start (or after init() on a reload)
    quint64 generation = 0;           // 0 only for the initial snapshot; then never 0
    // Custom only:
    quint16    bridgePort = 0; // 1024..65535 when not refused
    QByteArray token;          // 43 base64url characters (32 random bytes), stable for the plugin session, never logged
    QByteArray realm;          // "tsmedia-" + 16 hex, stable for the session
    Failure    refused = Failure::NotReady; // != None: every request refused, nothing sent. Custom: BridgeUnavailable /
                                            // PasswordUnreadable; the initial snapshot: NotReady (S1)
    bool       proxyOnThisPc = false;       // Custom: the user's proxy is on this PC (health's probation delay)
};

std::shared_ptr<const Snapshot> snapshot();         // any thread; never null
void publish(std::shared_ptr<const Snapshot> next); // GUI thread (Control)
// The session bridge's records (any thread), or an empty source.
std::shared_ptr<RecordSource> liveRecords();
void setLiveRecords(std::shared_ptr<RecordSource> records); // Control (and tests)
// When the route last became, or changed inside, custom (wall clock, ms since the epoch; 0: never). A cached "walled"
// memo written before it is not trusted any more (design 6.4). Control sets it; tests too.
qint64 lastProxyRouteChangeMs();
void   markProxyRouteChange(qint64 wallMs);

// Every plugin load (ts3plugin_init): the refusing snapshot, no records, no route change. A pinned thread of an earlier
// load can keep the image mapped across a reload, so statics must not assume a fresh image.
void init();

// The Test and Find workers (and anything else pinned that proxy::shutdown() must wait for): their thread handles.
// registerWorker takes ownership of the handle; joinWorkers waits for them (bounded) and closes the handles.
void registerWorker(void* thread);
void joinWorkers(int waitMs);
int  workerCount(); // registered and not yet finished (finished ones are pruned on each call)

} // namespace proxy
