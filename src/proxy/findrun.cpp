#include "findrun.h"

#include "winsock.h" // winsock2.h before windows.h

#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QStringList>

#include "i18n.h"
#include "linkpreview/transport.h"
#include "router.h"

namespace proxy {

struct FindRun::Link {
    QMutex            mutex;
    QPointer<FindRun> receiver; // cleared by cancel() and ~FindRun
};

namespace {

struct FindJob {
    std::shared_ptr<FindRun::Link>             link;
    std::shared_ptr<linkpreview::http::Cancel> cancel;
    FindRun::Options                           options;
    HMODULE                                    module = nullptr;
};

QVector<quint16> runFind(const FindJob& job)
{
    QVector<quint16> found;
    HANDLE           cancel = static_cast<HANDLE>(job.cancel->event());
    for (const quint16 port : job.options.ports) {
        if (job.cancel->isSet())
            break;
        if (port == 0 || port == job.options.bridgePort)
            continue; // never TS Media's own port
        ws::Conn           conn;
        const ws::Conn::Io io = conn.connectTo(linkpreview::net::Address::fromIPv4(0x7F000001u), port,
                                               ws::nowMs() + static_cast<quint64>(qMax(50, job.options.connectMs)), &cancel, 1);
        if (io != ws::Conn::Io::Ok)
            continue;
        static const char greeting[] = {0x05, 0x01, 0x00}; // no CONNECT, no name: nothing leaves the PC
        const quint64     replyBy    = ws::nowMs() + static_cast<quint64>(qMax(50, job.options.replyMs));
        if (conn.sendAll(greeting, 3, replyBy, &cancel, 1) != ws::Conn::Io::Ok)
            continue;
        QByteArray reply;
        while (reply.size() < 2) {
            char               buffer[16];
            int                got = 0;
            const ws::Conn::Io r   = conn.recvSome(buffer, sizeof(buffer), &got, replyBy, &cancel, 1);
            if (r != ws::Conn::Io::Ok)
                break;
            reply.append(buffer, got);
        }
        if (!reply.isEmpty() && static_cast<quint8>(reply.at(0)) == 0x05)
            found << port;
    }
    return found;
}

DWORD WINAPI findThread(void* parameter)
{
    auto*            job    = static_cast<FindJob*>(parameter);
    HMODULE          module = job->module;
    QVector<quint16> found;
    try {
        found = runFind(*job);
    } catch (...) {
        found.clear();
    }
    {
        QMutexLocker lock(&job->link->mutex);
        if (FindRun* receiver = job->link->receiver.data())
            QMetaObject::invokeMethod(receiver, [receiver, found]() { receiver->deliver(found); }, Qt::QueuedConnection);
    }
    delete job;
    FreeLibraryAndExitThread(module, 0);
}

} // namespace

QString findResultText(const QVector<quint16>& found)
{
    if (found.isEmpty())
        return i18n::t("No VPN app answered at its usual ports. Open your VPN app's settings and look for its local SOCKS or HTTP port.");
    if (found.size() == 1)
        return i18n::t("Found a proxy on this PC at port %1. Press Test, then OK.").arg(found.first());
    QStringList ports;
    for (const quint16 p : found)
        ports << QString::number(p);
    return i18n::t("Found proxies on this PC at ports %1. TS Media chose %2. Press Test, then OK.").arg(ports.join(QLatin1String(", "))).arg(found.first());
}

FindRun::FindRun(QObject* parent)
    : FindRun(Options(), parent)
{
}

FindRun::FindRun(const Options& options, QObject* parent)
    : QObject(parent)
    , m_options(options)
{
}

FindRun::~FindRun()
{
    cancel();
}

bool FindRun::start()
{
    if (m_running)
        return false;
    m_link           = std::make_shared<Link>();
    m_link->receiver = this;
    m_cancel         = std::make_shared<linkpreview::http::Cancel>();
    m_found.clear();
    auto* job    = new FindJob;
    job->link    = m_link;
    job->cancel  = m_cancel;
    job->options = m_options;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&findThread), &job->module)) {
        delete job;
        return false;
    }
    HMODULE module = job->module;
    HANDLE  thread = CreateThread(nullptr, 0, &findThread, job, 0, nullptr);
    if (!thread) {
        delete job;
        FreeLibrary(module);
        return false;
    }
    registerWorker(thread);
    m_running = true;
    return true;
}

void FindRun::cancel()
{
    if (m_link) {
        QMutexLocker lock(&m_link->mutex);
        m_link->receiver = nullptr;
    }
    if (m_cancel)
        m_cancel->set();
    m_running = false;
}

void FindRun::deliver(const QVector<quint16>& found)
{
    m_running = false;
    m_found   = found;
    emit finished();
}

} // namespace proxy
