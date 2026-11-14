#include "testrun.h"

#include <windows.h>

#include <QMetaObject>
#include <QMutex>
#include <QPointer>

#include "bridge.h"
#include "i18n.h"
#include "linkpreview/transport.h"
#include "router.h"

namespace proxy {

namespace {

constexpr unsigned long kSecureFlagInvalidCa = 0x00000010; // WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA

} // namespace

struct TestRun::Link {
    QMutex            mutex;
    QPointer<TestRun> receiver; // cleared (under mutex) by cancel() and ~TestRun: nothing is posted after that
};

namespace {

struct TestJob {
    std::shared_ptr<TestRun::Link>             link;
    std::shared_ptr<linkpreview::http::Cancel> cancel;
    TestRun::Options                           options;
    Config                                     form;
    protect::SecretBytes                       typed;
    QByteArray                                 savedBlob;
    HMODULE                                    module = nullptr;
};

TestResult classify(const Record* rec, linkpreview::http::Error error, const linkpreview::http::Head* head, unsigned long secureFlags)
{
    using linkpreview::http::Error;
    TestResult r;
    if (rec) {
        r.loginUnused = rec->loginUnused;
        r.step        = rec->step;
    }
    if (head) {
        if (head->proxyConnectResponse) {
            if (rec && isRouteLevel(rec->failure)) {
                r.outcome = TestResult::Outcome::Failed;
                r.failure = rec->failure;
            } else if (rec && rec->failure == Failure::SiteRefused) {
                r.outcome = TestResult::Outcome::Failed;
                r.failure = Failure::SiteRefused;
            } else {
                r.outcome = TestResult::Outcome::BridgeFailed;
            }
            return r;
        }
        r.status  = head->status;
        r.outcome = head->status == 204 ? TestResult::Outcome::Works : TestResult::Outcome::Unexpected;
        return r;
    }
    if (error == Error::Canceled) {
        r.outcome = TestResult::Outcome::Canceled;
        return r;
    }
    if (rec && isRouteLevel(rec->failure)) {
        r.outcome = TestResult::Outcome::Failed;
        r.failure = rec->failure;
        return r;
    }
    if (rec && rec->failure == Failure::SiteRefused) {
        r.outcome = TestResult::Outcome::Failed;
        r.failure = Failure::SiteRefused;
        return r;
    }
    if (error == Error::Timeout) {
        r.outcome = TestResult::Outcome::Slow;
        return r;
    }
    if (error == Error::Tls && (secureFlags & kSecureFlagInvalidCa) && rec && rec->down > 0) {
        r.outcome = TestResult::Outcome::Intercepted;
        return r;
    }
    if (!rec) {
        r.outcome = TestResult::Outcome::BridgeFailed; // the request never reached the temporary bridge
        return r;
    }
    // The tunnel stood, then the site's side failed (a closed connection, a broken TLS channel).
    r.outcome = TestResult::Outcome::Failed;
    r.failure = Failure::CantReachSite;
    return r;
}

TestResult runTest(TestJob& job)
{
    TestResult    result;
    const quint64 started = GetTickCount64();
    const auto    finish  = [&](TestResult r) {
        r.ms = static_cast<int>(GetTickCount64() - started);
        return r;
    };
    auto spec  = std::make_unique<UpstreamSpec>();
    spec->type = job.form.type;
    spec->host = job.form.host;
    spec->port = job.form.port;
    spec->user = job.form.user;
    spec->generation = 1;
    if (!job.form.user.isEmpty()) {
        if (!job.typed.isEmpty()) {
            spec->opened = std::make_shared<protect::SecretBytes>(std::move(job.typed));
        } else if (!job.savedBlob.isEmpty()) {
            auto                      plain  = std::make_shared<protect::SecretBytes>();
            const protect::OpenResult opened = protect::open(job.savedBlob, spec->endpoint(), plain.get());
            if (opened == protect::OpenResult::Unreadable) {
                result.outcome = TestResult::Outcome::PasswordUnreadable;
                return finish(result);
            }
            if (opened == protect::OpenResult::Transient) {
                result.outcome = TestResult::Outcome::PasswordTransient;
                return finish(result);
            }
            spec->opened = plain;
        }
    }
    job.typed.reset();
    if (job.cancel->isSet()) {
        result.outcome = TestResult::Outcome::Canceled;
        return finish(result);
    }
    Bridge::Options options;
    options.upstream = job.options.upstream;
    int  error       = 0;
    auto bridge      = Bridge::create(options, &error); // an ephemeral port and its own token
    if (!bridge) {
        result.outcome = TestResult::Outcome::BridgeFailed;
        return finish(result);
    }
    bridge->setUpstream(std::move(spec));

    std::shared_ptr<linkpreview::http::Transport> transport = job.options.transport ? job.options.transport : linkpreview::http::winHttpTransport();
    linkpreview::http::HopOptions hop;
    hop.url               = job.options.url;
    hop.route.via         = linkpreview::http::Route::Via::Bridge;
    hop.route.proxy       = QLatin1String("127.0.0.1:") + QString::number(bridge->port());
    hop.route.bridgeToken = bridge->token();
    hop.route.bridgeUser  = QByteArray("tsm-t1");
    hop.accept            = QByteArray("*/*");
    hop.deadline          = linkpreview::http::nowMs() + static_cast<quint64>(qMax(1000, job.options.timeoutMs));
    hop.connectTimeoutMs  = qMax(1000, job.options.timeoutMs);
    hop.receiveTimeoutMs  = qMax(1000, job.options.timeoutMs);
    hop.allowPeer         = [](const linkpreview::net::Address& a) { return a == linkpreview::net::Address::fromIPv4(0x7F000001u); };
    linkpreview::http::Error                     httpError = linkpreview::http::Error::None;
    std::unique_ptr<linkpreview::http::Exchange> exchange  = transport->open(hop, *job.cancel, &httpError);
    linkpreview::http::Head                      head;
    bool                                         gotHead = false;
    if (exchange && httpError == linkpreview::http::Error::None) {
        httpError = exchange->head(&head);
        gotHead   = httpError == linkpreview::http::Error::None;
    } else if (httpError == linkpreview::http::Error::None) {
        httpError = linkpreview::http::Error::BadResponse;
    }
    const unsigned long secureFlags = exchange ? exchange->secureFailureFlags() : 0;
    exchange.reset();
    Record     rec;
    const bool haveRecord = bridge->outcome('t', 1, &rec);
    result                = classify(haveRecord ? &rec : nullptr, httpError, gotHead ? &head : nullptr, secureFlags);
    if (job.cancel->isSet())
        result.outcome = TestResult::Outcome::Canceled;
    bridge->stop(job.options.bridgeStopMs);
    return finish(result);
}

DWORD WINAPI testThread(void* parameter)
{
    auto*      job    = static_cast<TestJob*>(parameter);
    HMODULE    module = job->module;
    TestResult result;
    try {
        result = runTest(*job);
    } catch (...) {
        result.outcome = TestResult::Outcome::BridgeFailed;
    }
    {
        QMutexLocker lock(&job->link->mutex);
        if (TestRun* receiver = job->link->receiver.data())
            QMetaObject::invokeMethod(receiver, [receiver, result]() { receiver->deliver(result); }, Qt::QueuedConnection);
    }
    delete job;
    if (linkpreview::http::mustStayLoaded())
        ExitThread(0); // WinHTTP may still call into the DLL: its pin stays
    FreeLibraryAndExitThread(module, 0);
}

} // namespace

// ---- texts ------------------------------------------------------------------------------------------------------------

bool testResultOk(const TestResult& result)
{
    return result.outcome == TestResult::Outcome::Works;
}

QString testResultText(const TestResult& r, const QString& hostPort, quint16 port)
{
    switch (r.outcome) {
    case TestResult::Outcome::None:
    case TestResult::Outcome::Canceled:
        return QString();
    case TestResult::Outcome::Works:
        return r.loginUnused ? i18n::t("Works · %1 ms (the proxy didn't ask for the login)").arg(r.ms) : i18n::t("Works · %1 ms").arg(r.ms);
    case TestResult::Outcome::Intercepted:
        return i18n::t("The connection was intercepted: the website's certificate isn't trusted.");
    case TestResult::Outcome::Slow:
        return i18n::t("The proxy is too slow or isn't answering.");
    case TestResult::Outcome::Unexpected:
        return i18n::t("Unexpected answer from the test site (HTTP %1).").arg(r.status);
    case TestResult::Outcome::PasswordUnreadable:
        return i18n::t("The saved password can't be read on this PC. Enter it again.");
    case TestResult::Outcome::PasswordTransient:
        return i18n::t("Windows couldn't read the saved password just now. Try again.");
    case TestResult::Outcome::BridgeFailed:
        return i18n::t("TS Media couldn't open its local connection to the proxy.");
    case TestResult::Outcome::Failed:
        break;
    }
    switch (r.failure) {
    case Failure::Unreachable:
        return i18n::t("Can't reach the proxy at %1. Is your VPN app running, and is %2 its port?").arg(hostPort).arg(port);
    case Failure::NameNotFound:
        return i18n::t("Can't find the proxy's address. Check the server name.");
    case Failure::ProxyNameFiltered:
        return i18n::t("The proxy's name leads to a filtered address. Use the proxy's IP address, or 127.0.0.1 for a VPN app on this PC.");
    case Failure::NotSocks5:
        return i18n::t("This port doesn't answer as a SOCKS5 proxy. Try HTTP proxy.");
    case Failure::NotHttp:
        return i18n::t("This port doesn't answer as an HTTP proxy. Try SOCKS5 proxy.");
    case Failure::NeedsLogin:
        return i18n::t("The proxy asks for a user name and password.");
    case Failure::LoginRefused:
        return i18n::t("The proxy refused the login. Check the user name and password.");
    case Failure::LoginMethod:
        return i18n::t("The proxy wants a login method TS Media doesn't support (only a user name and password).");
    case Failure::NotAllowed:
        return i18n::t("The proxy doesn't allow this connection.");
    case Failure::NoNameConnect:
        return i18n::t("The proxy can't connect to websites by name. TS Media needs a proxy that looks up names itself.");
    case Failure::SiteRefused:
        return i18n::t("The website refused the connection through the proxy.");
    case Failure::Slow:
        return i18n::t("The proxy is too slow or isn't answering.");
    case Failure::Intercepted:
        return i18n::t("The connection was intercepted: the website's certificate isn't trusted.");
    case Failure::PasswordUnreadable:
        return i18n::t("The saved password can't be read on this PC. Enter it again.");
    case Failure::BridgeUnavailable:
    case Failure::Internal:
    case Failure::Off:
        return i18n::t("TS Media couldn't open its local connection to the proxy.");
    case Failure::CantReachSite:
    default:
        return i18n::t("The proxy can't reach the website. Is your VPN app connected to a server?");
    }
}

// ---- TestRun ------------------------------------------------------------------------------------------------------------

TestRun::TestRun(QObject* parent)
    : TestRun(Options(), parent)
{
}

TestRun::TestRun(const Options& options, QObject* parent)
    : QObject(parent)
    , m_options(options)
{
}

TestRun::~TestRun()
{
    cancel(); // cuts the link; the worker is pinned and ends by itself (proxy::shutdown() joins it)
}

bool TestRun::start(const Config& form, protect::SecretBytes typed, const QByteArray& savedBlob)
{
    if (m_running)
        return false;
    m_link           = std::make_shared<Link>();
    m_link->receiver = this;
    m_cancel         = std::make_shared<linkpreview::http::Cancel>();
    m_result         = TestResult();
    auto* job        = new TestJob;
    job->link        = m_link;
    job->cancel      = m_cancel;
    job->options     = m_options;
    job->form        = form;
    job->form.passwordBlob.clear();
    const bool typedOne = !typed.isEmpty();
    job->typed          = std::move(typed);
    job->savedBlob      = typedOne ? QByteArray() : savedBlob;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&testThread), &job->module)) {
        delete job;
        return false;
    }
    HMODULE module = job->module;
    HANDLE  thread = CreateThread(nullptr, 0, &testThread, job, 0, nullptr);
    if (!thread) {
        delete job;
        FreeLibrary(module);
        return false;
    }
    registerWorker(thread);
    m_running = true;
    return true;
}

void TestRun::cancel()
{
    if (m_link) {
        QMutexLocker lock(&m_link->mutex);
        m_link->receiver = nullptr;
    }
    if (m_cancel)
        m_cancel->set();
    m_running = false;
}

void TestRun::deliver(const TestResult& result)
{
    m_running = false;
    m_result  = result;
    emit finished();
}

} // namespace proxy
