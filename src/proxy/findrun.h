#pragma once

// 2.6 chat proxy: Find my VPN app (design 1.3.7). A loopback-only SOCKS5 greeting (05 01 00) to 127.0.0.1 at the usual
// ports of VPN apps, in order, skipping the bridge's own port: a non-blocking connect (300 ms), the greeting, then 500 ms
// for the answer; a reply whose first byte is 05 counts as found. Nothing else is sent (no CONNECT, no name) and
// nothing leaves the PC. The TestRun pattern (testrun.h): a pinned, registered worker; a posting link cut by cancel()
// and the destructor, which never joins.

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

namespace linkpreview::http {
class Cancel;
} // namespace linkpreview::http

namespace proxy {

// The result row's text for Find my VPN app (design 1.3.7): found (first chosen), several, or none.
QString findResultText(const QVector<quint16>& found);

class FindRun : public QObject
{
    Q_OBJECT

  public:
    struct Options {
        QVector<quint16> ports      = {10808, 2080, 7897, 12334}; // v2rayN, Throne / Nekoray, Clash Verge Rev, Hiddify
        quint16          bridgePort = 0;                          // skipped
        int              connectMs  = 300;
        int              replyMs    = 500;
    };
    explicit FindRun(QObject* parent = nullptr);
    FindRun(const Options& options, QObject* parent);
    ~FindRun() override;

    bool             start();
    void             cancel();
    bool             isRunning() const { return m_running; }
    QVector<quint16> found() const { return m_found; } // in the order tried; the first is the one chosen

    struct Link;
    void deliver(const QVector<quint16>& found); // the worker's result, on the GUI thread (internal)

  signals:
    void finished();

  private:

    Options                                    m_options;
    std::shared_ptr<Link>                      m_link;
    std::shared_ptr<linkpreview::http::Cancel> m_cancel;
    bool                                       m_running = false;
    QVector<quint16>                           m_found;
};

} // namespace proxy
