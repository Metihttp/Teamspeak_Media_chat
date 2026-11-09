// 2.4 link previews: linkfetch_smoke <url> <outdir> [--max-bytes N] [--cancel-after ms] [--twice] [--verbose] [--direct]
//
// Fetches one link through the plugin's real path (classify, every network rule, WinHTTP, the content
// checks, the Media Foundation probe) into <outdir> and prints what came back. For the live test only:
// it connects to the website named on the command line. The unit tests never run it.
//
//   --max-bytes N      the size cap (default: the Fetcher's 50 MB)
//   --cancel-after ms  cancel the load after ms milliseconds (checks that a cancel is immediate)
//   --twice            load it again after it finished (the second answer must come from the cache)
//   --verbose          print every route (direct or the Windows proxy), lookup, address check, request and
//                      response header
//   --direct           like --verbose, but never through the Windows proxy (to compare)
//
// Exit code: 0 loaded, 1 failed or canceled, 2 not a previewable link, 3 usage.

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <memory>

#include "linkpreview/fetcher.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/transport.h"
#include "video/mfvideo.h"

namespace {

void say(const QString& line)
{
    std::fprintf(stdout, "%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

// --verbose: every name lookup, address check, connection and response header of the real transport.
class TracingExchange : public linkpreview::http::Exchange
{
  public:
    explicit TracingExchange(std::unique_ptr<linkpreview::http::Exchange> inner)
        : m_inner(std::move(inner))
    {
    }
    linkpreview::http::Error head(linkpreview::http::Head* out) override
    {
        const linkpreview::http::Error error = m_inner->head(out);
        say(QStringLiteral("  head: %1 status=%2 type=%3 length=%4 peer=%5 location=%6 (system error %7)")
                .arg(linkpreview::http::errorName(error))
                .arg(out->status)
                .arg(QString::fromLatin1(out->contentType))
                .arg(out->contentLength)
                .arg(out->peer.toString(), linkpreview::logUrl(QUrl(out->location)))
                .arg(m_inner->systemError()));
        return error;
    }
    linkpreview::http::Error read(char* buffer, int capacity, int* got) override
    {
        const linkpreview::http::Error error = m_inner->read(buffer, capacity, got);
        if (error != linkpreview::http::Error::None)
            say(QStringLiteral("  read: %1 (system error %2)").arg(linkpreview::http::errorName(error)).arg(m_inner->systemError()));
        return error;
    }
    unsigned long systemError() const override { return m_inner->systemError(); }

  private:
    std::unique_ptr<linkpreview::http::Exchange> m_inner;
};

class TracingTransport : public linkpreview::http::Transport
{
  public:
    explicit TracingTransport(bool direct)
        : m_inner(linkpreview::http::winHttpTransport())
        , m_direct(direct)
    {
    }
    // The Windows proxy (netpolicy.h); --direct leaves it out to compare.
    linkpreview::http::Route route(const QUrl& url, const linkpreview::http::Cancel& cancel, quint64 deadline) override
    {
        const linkpreview::http::Route r = m_direct ? linkpreview::http::Route() : m_inner->route(url, cancel, deadline);
        say(QStringLiteral("  route %1: %2").arg(url.host(), r.viaProxy ? QStringLiteral("through the Windows proxy %1").arg(r.proxy) : QStringLiteral("direct")));
        return r;
    }
    linkpreview::http::Error resolve(const QString& host, QVector<linkpreview::net::Address>* out, const linkpreview::http::Cancel& cancel,
                                     quint64 deadline) override
    {
        const linkpreview::http::Error error = m_inner->resolve(host, out, cancel, deadline);
        QStringList addresses;
        for (const linkpreview::net::Address& address : *out)
            addresses << address.toString() + (linkpreview::net::isPublicAddress(address) ? QString() : QStringLiteral(" (not public)"));
        say(QStringLiteral("  resolve %1: %2 [%3]").arg(host, linkpreview::http::errorName(error), addresses.join(QStringLiteral(", "))));
        return error;
    }
    std::unique_ptr<linkpreview::http::Exchange> open(const linkpreview::http::HopOptions& options, const linkpreview::http::Cancel& cancel,
                                                      linkpreview::http::Error* error) override
    {
        linkpreview::http::HopOptions traced = options;
        traced.allowPeer                      = [allow = options.allowPeer](const linkpreview::net::Address& address) {
            const bool ok = allow && allow(address);
            say(QStringLiteral("  connection to %1: %2").arg(address.toString(), ok ? QStringLiteral("allowed") : QStringLiteral("refused")));
            return ok;
        };
        say(QStringLiteral("  GET %1 (Accept %2)").arg(linkpreview::logUrl(options.url), QString::fromLatin1(options.accept)));
        auto exchange = m_inner->open(traced, cancel, error);
        if (!exchange)
            say(QStringLiteral("  open: %1").arg(linkpreview::http::errorName(*error)));
        return exchange ? std::unique_ptr<linkpreview::http::Exchange>(new TracingExchange(std::move(exchange))) : nullptr;
    }

  private:
    std::shared_ptr<linkpreview::http::Transport> m_inner;
    bool                                          m_direct = false;
};

QString describe(const linkpreview::Result& r)
{
    QStringList parts;
    parts << (r.ok ? QStringLiteral("ok") : QStringLiteral("error=") + r.error);
    if (!r.localFile.isEmpty())
        parts << QStringLiteral("file=") + r.localFile;
    if (!r.thumbnailFile.isEmpty())
        parts << QStringLiteral("thumbnail=") + r.thumbnailFile;
    if (!r.mime.isEmpty())
        parts << QStringLiteral("mime=") + r.mime;
    parts << QStringLiteral("size=%1").arg(r.size);
    if (r.width > 0 || r.height > 0)
        parts << QStringLiteral("%1x%2").arg(r.width).arg(r.height);
    if (r.durationMs > 0)
        parts << QStringLiteral("duration=%1ms").arg(r.durationMs);
    if (!r.title.isEmpty())
        parts << QStringLiteral("title=\"%1\"").arg(r.title);
    if (!r.author.isEmpty())
        parts << QStringLiteral("author=\"%1\"").arg(r.author);
    if (r.viaProxy)
        parts << QStringLiteral("via-windows-proxy");
    return parts.join(QLatin1Char(' '));
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv); // picture formats come from Qt's image plugins
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        say(QStringLiteral("usage: linkfetch_smoke <url> <outdir> [--max-bytes N] [--cancel-after ms] [--twice] [--verbose] [--direct]"));
        return 3;
    }
    const QString url        = args.at(1);
    const QString outDir     = args.at(2);
    qint64        maxBytes   = 0;
    int           cancelMs   = -1;
    bool          twice      = false;
    bool          verbose    = false;
    bool          direct     = false; // --direct: never the Windows proxy (with --verbose)
    for (int i = 3; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--max-bytes") && i + 1 < args.size())
            maxBytes = args.at(++i).toLongLong();
        else if (args.at(i) == QLatin1String("--cancel-after") && i + 1 < args.size())
            cancelMs = args.at(++i).toInt();
        else if (args.at(i) == QLatin1String("--twice"))
            twice = true;
        else if (args.at(i) == QLatin1String("--direct"))
            direct = verbose = true;
        else if (args.at(i) == QLatin1String("--verbose"))
            verbose = true;
    }

    const linkpreview::Target target = linkpreview::classify(url);
    say(QStringLiteral("kind=%1 host=%2 key=%3 fetch=%4")
            .arg(QString::fromLatin1(linkpreview::kindName(target.kind)), target.host, target.key, linkpreview::logUrl(target.url)));
    if (!target.isValid()) {
        say(QStringLiteral("not a previewable link"));
        return 2;
    }

    mf::startup();
    int  exitCode = 1;
    int  rounds   = 0;
    auto fetcher  = verbose ? new linkpreview::Fetcher(std::make_shared<TracingTransport>(direct), linkpreview::Fetcher::Options(), &app)
                            : new linkpreview::Fetcher(&app);
    fetcher->setLogger([](const QString& line) { say(QStringLiteral("log: ") + line); });
    QElapsedTimer clock;
    clock.start();
    qint64 lastShown = -1000;
    QObject::connect(fetcher, &linkpreview::Fetcher::progress, &app, [&](const QString&, qint64 received, qint64 total) {
        if (clock.elapsed() - lastShown < 500)
            return;
        lastShown = clock.elapsed();
        say(QStringLiteral("progress %1 / %2 bytes").arg(received).arg(total));
    });
    QObject::connect(fetcher, &linkpreview::Fetcher::finished, &app, [&](const QString& key, const linkpreview::Result& result) {
        ++rounds;
        say(QStringLiteral("finished after %1 ms: %2").arg(clock.elapsed()).arg(describe(result)));
        exitCode = result.ok ? 0 : 1;
        if (twice && rounds == 1 && result.ok) {
            linkpreview::Result cached;
            say(QStringLiteral("cached() says: %1").arg(linkpreview::Fetcher::cached(target, outDir, &cached) ? describe(cached) : QStringLiteral("nothing")));
            clock.restart();
            fetcher->load(target, maxBytes, outDir);
            return;
        }
        Q_UNUSED(key)
        app.quit();
    });
    QTimer cancelTimer;
    cancelTimer.setSingleShot(true);
    QObject::connect(&cancelTimer, &QTimer::timeout, &app, [&] {
        say(QStringLiteral("canceling after %1 ms").arg(clock.elapsed()));
        fetcher->cancel(target.key);
    });
    if (cancelMs >= 0)
        cancelTimer.start(cancelMs);

    fetcher->load(target, maxBytes, outDir);
    app.exec();

    QElapsedTimer down;
    down.start();
    fetcher->shutdown();
    say(QStringLiteral("shutdown took %1 ms").arg(down.elapsed()));
    delete fetcher;
    mf::shutdown();
    return exitCode;
}
