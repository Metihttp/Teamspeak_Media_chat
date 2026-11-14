#pragma once

// 2.6 chat proxy: the Test button's job (design 3.7). Find my VPN app is findrun.h.
//
// TestRun: one request, GET https://www.youtube.com/generate_204, through a temporary bridge built from the form's
// values (an ephemeral port, its own token), with the real link-preview transport; the temporary bridge's record names
// the step that failed.
//
// One at a time, a pinned worker thread (FreeLibraryAndExitThread) registered with proxy::registerWorker (joined,
// bounded, by proxy::shutdown()); the result is posted to the GUI thread through a posting link that cancel() and the
// destructor cut, so nothing is posted after them. The destructor never joins (F22). Signals carry no arguments.

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

#include <memory>

#include "failure.h"
#include "protect.h"
#include "proxyconfig.h"
#include "upstream.h"

namespace linkpreview::http {
class Transport;
class Cancel;
} // namespace linkpreview::http

namespace proxy {

struct TestResult {
    enum class Outcome {
        None,
        Works,
        Failed,             // failure says why (a route-level failure of the proxy)
        Intercepted,        // TLS with an untrusted CA after the site's bytes came
        Slow,
        Unexpected,         // another HTTP status from the test site (status)
        PasswordUnreadable, // the saved password can't be read on this PC
        PasswordTransient,  // Windows couldn't read it just now
        BridgeFailed,       // the temporary bridge couldn't start
        Canceled,
    };
    Outcome outcome     = Outcome::None;
    Failure failure     = Failure::None;
    Step    step        = Step::None;
    int     status      = 0;
    int     ms          = 0;
    bool    loginUnused = false;
};
// The result row's text (design 1.3.5). hostPort: the form's "host:port"; port: its port.
QString testResultText(const TestResult& result, const QString& hostPort, quint16 port);
// The result row's icon: true for the check mark (Works), false for the warning icon.
bool testResultOk(const TestResult& result);

class TestRun : public QObject
{
    Q_OBJECT

  public:
    struct Options {
        QUrl url       = QUrl(QString::fromLatin1("https://www.youtube.com/generate_204"));
        int  timeoutMs = 20000;
        std::shared_ptr<linkpreview::http::Transport> transport; // null: the WinHTTP transport
        UpstreamOptions upstream;
        int  bridgeStopMs = 500;
    };
    explicit TestRun(QObject* parent = nullptr);
    TestRun(const Options& options, QObject* parent);
    ~TestRun() override; // cuts the posting link and cancels; never joins

    // form: the form's type, server, port, user name. typed: the typed password (moved in; empty: none). savedBlob: the
    // saved password, used only when typed is empty (the caller passes it only when the endpoint equals the saved one).
    bool       start(const Config& form, protect::SecretBytes typed, const QByteArray& savedBlob);
    void       cancel();
    bool       isRunning() const { return m_running; }
    TestResult result() const { return m_result; }

    struct Link;
    void deliver(const TestResult& result); // the worker's result, on the GUI thread (internal)

  signals:
    void finished();

  private:

    Options                                   m_options;
    std::shared_ptr<Link>                     m_link;
    std::shared_ptr<linkpreview::http::Cancel> m_cancel;
    bool                                      m_running = false;
    TestResult                                m_result;
};

} // namespace proxy
