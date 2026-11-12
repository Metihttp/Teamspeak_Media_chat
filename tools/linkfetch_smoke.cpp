// Link previews: linkfetch_smoke, the live smoke test of the Fetcher through the plugin's real path (classify, every
// network rule, WinHTTP, the content checks, the Media Foundation probe). For the live test only: it connects to the
// websites named on the command line or in the list. The unit tests never run it.
//
//   linkfetch_smoke <url> <outdir> [--max-bytes N] [--cancel-after ms] [--twice] [--verbose] [--direct] [--card]
//       one link: what came back (2.4 links: maxBytes as 2.4.0; 2.5 links: the click caps)
//       --max-bytes N      the media cap (default: the Fetcher's 50 MB for 2.4 links, 25 MB for 2.5 links)
//       --cancel-after ms  cancel the load after ms milliseconds (checks that a cancel is immediate)
//       --twice            load it again after it finished (the second answer must come from the cache)
//       --verbose          print every route (direct or the Windows proxy), lookup, address check, request and
//                          response header
//       --direct           like --verbose, but never through the Windows proxy (to compare)
//       --card             print a card's texts (cut at 80 characters) for a human look
//   linkfetch_smoke --classify <url>...
//       no network: class, provider, kind, key, page, fetch address, expected requests of each link
//   linkfetch_smoke --list <tsv> <outdir> [--card] [--seq N] [--only TEXT] [--verbose]
//       every row of a list (url TAB expectation TAB flags TAB note; flags: flaky, seq, direct), one shared HostGate,
//       2 s apart; then the rows marked seq N times back to back (the HostGate and soft-cooldown check). Prints
//       "PASS|DIFF <url> <class> <kind> <requests> <ms> [title N chars, desc N chars, picture WxH type size, layout,
//       finalHost]" per row and a summary; a hop refused by a row's hop list is always a DIFF.
//
// Exit code: 0 loaded / every row as expected (or flaky), 1 failed, 2 not a previewable link, 3 usage.

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMutex>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>

#include "linkpreview/fetcher.h"
#include "linkpreview/hostgate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/transport.h"
#include "video/mfvideo.h"

namespace {

using namespace linkpreview;

void say(const QString& line)
{
    std::fprintf(stdout, "%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

// --verbose: every name lookup, address check, connection and response header of the real transport.
class TracingExchange : public http::Exchange
{
  public:
    TracingExchange(std::unique_ptr<http::Exchange> inner, bool verbose)
        : m_inner(std::move(inner))
        , m_verbose(verbose)
    {
    }
    http::Error head(http::Head* out) override
    {
        const http::Error error = m_inner->head(out);
        if (m_verbose)
            say(QStringLiteral("  head: %1 status=%2 type=%3 length=%4 encoding=%5 retry-after=%6 rate=%7/%8 challenge=%9 peer=%10 location=%11 (system error %12)")
                    .arg(http::errorName(error))
                    .arg(out->status)
                    .arg(QString::fromLatin1(out->contentType))
                    .arg(out->contentLength)
                    .arg(QString::fromLatin1(out->contentEncoding))
                    .arg(out->retryAfterMs)
                    .arg(out->rateRemaining)
                    .arg(out->rateResetMs)
                    .arg(out->challenge ? 1 : 0)
                    .arg(out->peer.toString(), logUrl(QUrl(out->location)))
                    .arg(m_inner->systemError()));
        return error;
    }
    http::Error read(char* buffer, int capacity, int* got) override
    {
        const http::Error error = m_inner->read(buffer, capacity, got);
        if (error != http::Error::None && m_verbose)
            say(QStringLiteral("  read: %1 (system error %2)").arg(http::errorName(error)).arg(m_inner->systemError()));
        return error;
    }
    unsigned long systemError() const override { return m_inner->systemError(); }

  private:
    std::unique_ptr<http::Exchange> m_inner;
    bool                            m_verbose = false;
};

// The real transport, traced (--verbose) and recorded (the hosts and addresses a row asked, for --list).
class TracingTransport : public http::Transport
{
  public:
    TracingTransport(bool verbose, bool direct)
        : m_inner(http::winHttpTransport())
        , m_verbose(verbose)
        , m_direct(direct)
    {
    }
    void setDirect(bool direct) { m_direct = direct; }
    QStringList takeRequested()
    {
        QMutexLocker lock(&m_mutex);
        QStringList  out = m_requested;
        m_requested.clear();
        return out;
    }
    http::Route route(const QUrl& url, const http::Cancel& cancel, quint64 deadline) override
    {
        const http::Route r = m_direct ? http::Route() : m_inner->route(url, cancel, deadline);
        if (m_verbose)
            say(QStringLiteral("  route %1: %2").arg(url.host(), r.viaProxy ? QStringLiteral("through the Windows proxy %1").arg(r.proxy) : QStringLiteral("direct")));
        return r;
    }
    http::Error resolve(const QString& host, QVector<net::Address>* out, const http::Cancel& cancel, quint64 deadline) override
    {
        const http::Error error = m_inner->resolve(host, out, cancel, deadline);
        if (m_verbose) {
            QStringList addresses;
            for (const net::Address& address : *out)
                addresses << address.toString() + (net::isPublicAddress(address) ? QString() : QStringLiteral(" (not public)"));
            say(QStringLiteral("  resolve %1: %2 [%3]").arg(host, http::errorName(error), addresses.join(QStringLiteral(", "))));
        }
        return error;
    }
    std::unique_ptr<http::Exchange> open(const http::HopOptions& options, const http::Cancel& cancel, http::Error* error) override
    {
        http::HopOptions traced = options;
        const bool       verbose = m_verbose;
        traced.allowPeer        = [allow = options.allowPeer, verbose](const net::Address& address) {
            const bool ok = allow && allow(address);
            if (verbose)
                say(QStringLiteral("  connection to %1: %2").arg(address.toString(), ok ? QStringLiteral("allowed") : QStringLiteral("refused")));
            return ok;
        };
        {
            QMutexLocker lock(&m_mutex);
            m_requested << options.url.toString(QUrl::FullyEncoded);
        }
        if (m_verbose)
            say(QStringLiteral("  GET %1 (Accept %2; UA %3)").arg(logUrl(options.url), QString::fromLatin1(options.accept), QString::fromLatin1(options.userAgent)));
        auto exchange = m_inner->open(traced, cancel, error);
        if (!exchange && m_verbose)
            say(QStringLiteral("  open: %1").arg(http::errorName(*error)));
        return exchange ? std::unique_ptr<http::Exchange>(new TracingExchange(std::move(exchange), m_verbose)) : nullptr;
    }

  private:
    std::shared_ptr<http::Transport> m_inner;
    bool                             m_verbose = false;
    bool                             m_direct  = false;
    QMutex                           m_mutex;
    QStringList                      m_requested;
};

QString describe(const Result& r)
{
    QStringList parts;
    parts << (r.ok ? QStringLiteral("ok") : QStringLiteral("error=") + r.error);
    parts << QStringLiteral("kind=%1").arg(QString::fromLatin1(kindName(r.kind)));
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
    if (r.hasAudio)
        parts << QStringLiteral("audio");
    if (!r.title.isEmpty())
        parts << QStringLiteral("title=\"%1\"").arg(r.title);
    if (!r.author.isEmpty())
        parts << QStringLiteral("author=\"%1\"").arg(r.author);
    if (r.retryAfterMs > 0)
        parts << QStringLiteral("retry-after=%1ms%2").arg(r.retryAfterMs).arg(r.softCooling ? QStringLiteral(" (soft)") : QString());
    parts << QStringLiteral("requests=%1").arg(r.requests);
    if (!r.finalHost.isEmpty())
        parts << QStringLiteral("finalHost=") + r.finalHost;
    if (!r.hopRefused.isEmpty())
        parts << QStringLiteral("hop-refused=") + r.hopRefused;
    if (r.viaProxy)
        parts << QStringLiteral("via-windows-proxy");
    return parts.join(QLatin1Char(' '));
}

QString cut(const QString& text, int chars = 80)
{
    return text.size() > chars ? text.left(chars - 1) + QChar(0x2026) : text;
}

void sayCard(const Result& r)
{
    if (r.kind != Kind::Card)
        return;
    say(QStringLiteral("  card site: %1").arg(cut(r.card.site)));
    say(QStringLiteral("  card author: %1").arg(cut(r.card.author)));
    say(QStringLiteral("  card title: %1").arg(cut(r.card.title)));
    say(QStringLiteral("  card description: %1").arg(cut(r.card.description)));
    say(QStringLiteral("  card picture: %1x%2 %3, layout %4, colour %5, play %6 %7 ms, sensitive %8")
            .arg(r.card.pictureWidth)
            .arg(r.card.pictureHeight)
            .arg(r.card.pictureFile.isEmpty() ? QStringLiteral("none") : QFileInfo(r.card.pictureFile).fileName(), QString::fromLatin1(layoutName(r.card.layout)),
                 r.card.hasColor ? QColor(r.card.color).name() : QStringLiteral("-"))
            .arg(r.card.play ? 1 : 0)
            .arg(r.card.durationMs)
            .arg(r.card.sensitive ? 1 : 0));
}

Caps capsFor(const Target& target, qint64 maxBytes)
{
    Caps caps;
    caps.media     = maxBytes > 0 ? maxBytes : (target.isLegacy() ? Fetcher::kDefaultMaxBytes : 25LL * 1024 * 1024);
    caps.picture   = 2 * 1024 * 1024;
    caps.page      = 512 * 1024;
    caps.userAsked = true;
    if (target.provider == QLatin1String("nextcloud"))
        caps.video = maxBytes > 0 ? maxBytes : 50LL * 1024 * 1024; // a click's video cap (autoload::clickCaps)
    return caps;
}

int classifyOnly(const QStringList& urls)
{
    for (const QString& url : urls) {
        // Plain concatenation: addresses hold "%2F" and the like, which QString::arg would take for its markers.
        const Target  t    = classify(trimLinkEnd(url));
        const auto    dash = [](const QString& s) { return s.isEmpty() ? QStringLiteral("-") : s; };
        const QString page = t.page.isEmpty() ? (t.url.isEmpty() ? QString() : t.url.toString(QUrl::FullyEncoded)) : t.page.toString(QUrl::FullyEncoded);
        say(url + QStringLiteral("\n  class=") + QString::fromLatin1(className(t.cls)) + QStringLiteral(" provider=") + dash(t.provider)
            + QStringLiteral(" kind=") + QString::fromLatin1(kindName(t.kind)) + QStringLiteral(" item=") + dash(t.itemId) + QStringLiteral(" sub=") + dash(t.sub)
            + QStringLiteral(" requests=") + QString::number(t.expectedRequests) + QStringLiteral("\n  key=") + dash(t.key) + QStringLiteral("\n  page=")
            + dash(page) + QStringLiteral("\n  fetch=") + dash(t.fetchUrl.toString(QUrl::FullyEncoded)));
    }
    return 0;
}

// ---- --list ---------------------------------------------------------------------------------------------------

struct Row {
    QString url, expect, note;
    bool    flaky = false, seq = false, direct = false;
};

// What a load turned out to be, in the words of the list's expectations.
QString outcomeWords(const Target& t, const Result& r)
{
    if (!t.isValid())
        return QStringLiteral("none");
    if (r.ok) {
        if (r.kind == Kind::Card)
            return QStringLiteral("card");
        if (r.kind == Kind::YouTube)
            return QStringLiteral("youtube");
        return (t.isLegacy() ? QStringLiteral("direct ") : QStringLiteral("media ")) + QString::fromLatin1(kindName(r.kind));
    }
    if (r.error == QLatin1String(errors::kWalled))
        return QStringLiteral("nothing (walled)");
    if (r.error == QLatin1String(errors::kNothing))
        return QStringLiteral("nothing");
    if (r.error == QLatin1String(errors::kRateLimited) || r.error == QLatin1String(errors::kGateBusy))
        return r.softCooling ? QStringLiteral("cooling (soft)") : QStringLiteral("cooling");
    return QStringLiteral("error ") + r.error;
}

bool matches(const QString& expectation, const QString& got, const Result& r)
{
    const QString e = expectation.toLower();
    for (const QString& alt0 : e.split(QStringLiteral(" or "))) {
        QString alt = alt0.trimmed();
        // "media video or loop": the second alternative inherits the first's word.
        if (!alt.startsWith(QLatin1String("media")) && !alt.startsWith(QLatin1String("direct")) && !alt.startsWith(QLatin1String("card"))
            && !alt.startsWith(QLatin1String("nothing")) && !alt.startsWith(QLatin1String("none")) && !alt.startsWith(QLatin1String("youtube"))
            && !alt.startsWith(QLatin1String("cooling")) && e.startsWith(QLatin1String("media")))
            alt = QStringLiteral("media ") + alt;
        if (alt.startsWith(QLatin1String("none")) && got == QLatin1String("none"))
            return true;
        if (alt.startsWith(QLatin1String("error ")) && got.compare(alt, Qt::CaseInsensitive) == 0)
            return true; // "error tooLarge": an expected refusal (a share's video over the cap)
        if (alt.startsWith(QLatin1String("nothing (walled)")) && got == QLatin1String("nothing (walled)"))
            return true;
        if (alt == QLatin1String("nothing") && got.startsWith(QLatin1String("nothing")))
            return true;
        if (alt.startsWith(QLatin1String("youtube")) && got == QLatin1String("youtube"))
            return true;
        if (alt.startsWith(QLatin1String("cooling (soft)")) && got == QLatin1String("cooling (soft)"))
            return true;
        if (alt == QLatin1String("cooling") && got.startsWith(QLatin1String("cooling")))
            return true;
        if (alt.startsWith(QLatin1String("card")) && got == QLatin1String("card")) {
            if (alt.contains(QLatin1String("text only")) && !r.card.pictureFile.isEmpty())
                continue;
            if (alt.contains(QLatin1String("360 px high")) && r.card.pictureHeight < 360)
                continue;
            return true;
        }
        for (const char* family : {"media", "direct"}) {
            if (!alt.startsWith(QLatin1String(family)) || !got.startsWith(QLatin1String(family)))
                continue;
            const QStringList words = alt.mid(static_cast<int>(strlen(family))).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            QStringList       kinds;
            for (const QString& word : words) {
                if (word == QLatin1String("loop") || word == QLatin1String("video") || word == QLatin1String("gif") || word == QLatin1String("image"))
                    kinds << word;
            }
            if (kinds.isEmpty() || kinds.contains(got.section(QLatin1Char(' '), 1, 1)))
                return true;
        }
    }
    return false;
}

QString detail(const Result& r)
{
    QStringList parts;
    if (r.kind == Kind::Card) {
        parts << QStringLiteral("title %1 chars").arg(r.card.title.size()) << QStringLiteral("desc %1 chars").arg(r.card.description.size());
        if (!r.card.pictureFile.isEmpty())
            parts << QStringLiteral("picture %1x%2 %3 %4").arg(r.card.pictureWidth).arg(r.card.pictureHeight).arg(r.mime).arg(r.size);
        else
            parts << QStringLiteral("no picture");
        parts << QStringLiteral("layout %1").arg(QString::fromLatin1(layoutName(r.card.layout)));
    } else if (r.ok) {
        parts << QStringLiteral("%1 %2x%3 %4 %5%6").arg(r.mime).arg(r.width).arg(r.height).arg(r.size).arg(r.durationMs > 0 ? QStringLiteral("%1 ms").arg(r.durationMs) : QString(), r.hasAudio ? QStringLiteral(" audio") : QString());
    } else {
        if (r.error == QLatin1String(errors::kTooLarge))
            parts << QStringLiteral("size ") + QString::number(r.size) + QStringLiteral(" kind ") + QString::fromLatin1(kindName(r.kind));
        parts << r.error + (r.retryAfterMs > 0 ? QStringLiteral(" (retry in %1 s%2)").arg((r.retryAfterMs + 999) / 1000).arg(r.softCooling ? QStringLiteral(", soft") : QString()) : QString());
    }
    parts << QStringLiteral("finalHost %1").arg(r.finalHost.isEmpty() ? QStringLiteral("-") : r.finalHost);
    return QLatin1Char('[') + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
}

int runList(QGuiApplication& app, const QString& listPath, const QString& outDir, bool cards, int seqTimes, const QString& only, bool verbose)
{
    QFile file(listPath);
    if (!file.open(QIODevice::ReadOnly)) {
        say(QStringLiteral("can't read %1").arg(listPath));
        return 3;
    }
    QVector<Row> rows;
    for (const QByteArray& raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList cols = line.split(QLatin1Char('\t'));
        Row               row;
        row.url               = cols.value(0).trimmed();
        row.expect            = cols.value(1).trimmed();
        const QStringList flags = cols.value(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
        row.flaky  = flags.contains(QStringLiteral("flaky"));
        row.seq    = flags.contains(QStringLiteral("seq"));
        row.direct = flags.contains(QStringLiteral("direct"));
        row.note   = cols.value(3).trimmed();
        if (only.isEmpty() || row.url.contains(only))
            rows << row;
    }
    QDir().mkpath(outDir);
    auto transport = std::make_shared<TracingTransport>(verbose, false);
    Fetcher::Options options;
    options.gate = std::make_shared<HostGate>(); // one gate for the whole run, as in the plugin
    Fetcher fetcher(transport, options);
    fetcher.setLogger([verbose](const QString& line) {
        if (verbose)
            say(QStringLiteral("  log: ") + line);
    });

    struct Job {
        Row  row;
        int  round = 0; // seq rounds
    };
    QVector<Job> jobs;
    for (const Row& row : qAsConst(rows))
        jobs << Job{row, 0};
    for (int round = 1; round <= seqTimes; ++round) {
        for (const Row& row : qAsConst(rows)) {
            if (row.seq)
                jobs << Job{row, round};
        }
    }
    int        passed = 0, diffs = 0, flaky = 0;
    QStringList failedRows;
    int        index = 0;
    QTimer     pause;
    pause.setSingleShot(true);
    QElapsedTimer clock;
    std::function<void()> next;
    QMetaObject::Connection done;
    next = [&]() {
        if (index >= jobs.size()) {
            app.quit();
            return;
        }
        const Job&    job    = jobs.at(index);
        const Target  target = classify(trimLinkEnd(job.row.url));
        const QString what   = QStringLiteral("%1 %2").arg(QString::fromLatin1(className(target.cls)), target.provider.isEmpty() ? QStringLiteral("-") : target.provider);
        transport->setDirect(job.row.direct);
        transport->takeRequested();
        if (!target.isValid()) {
            const bool ok = matches(job.row.expect, QStringLiteral("none"), Result());
            say(QStringLiteral("%1 %2 none none 0 0ms [classify]").arg(ok ? QStringLiteral("PASS") : QStringLiteral("DIFF"), job.row.url));
            (ok ? passed : (job.row.flaky ? flaky : diffs))++;
            if (!ok)
                failedRows << job.row.url;
            ++index;
            QTimer::singleShot(0, &app, next);
            return;
        }
        clock.restart();
        // A seq round loads into a folder of its own: never answered from the cache, so it meets the gate.
        fetcher.load(target, capsFor(target, 0), job.round > 0 ? outDir + QStringLiteral("/seq") + QString::number(job.round) : outDir);
    };
    done = QObject::connect(&fetcher, &Fetcher::finished, &app, [&](const QString&, const Result& r) {
        const Job&    job    = jobs.at(index);
        const Target  target = classify(trimLinkEnd(job.row.url));
        QString       expect = job.row.expect;
        if (job.row.seq && expect.contains(QLatin1String(", then "))) {
            // The list's own pass and the first back-to-back round expect the first step (the site may have rested
            // since the list's pass); the later rounds the next steps.
            const QStringList steps = expect.split(QStringLiteral(", then "));
            expect                  = steps.value(qMin(qMax(0, job.round - 1), steps.size() - 1));
        } else if (job.round > 0) {
            expect = expect + QStringLiteral(" or cooling");
        }
        const QString got      = outcomeWords(target, r);
        const bool    hopDiff  = !r.hopRefused.isEmpty();
        const bool    ok       = !hopDiff && matches(expect, got, r);
        const QString verdict  = ok ? QStringLiteral("PASS") : QStringLiteral("DIFF");
        // Plain concatenation: addresses hold "%2F" and the like, which QString::arg would take for its markers.
        say(verdict + QLatin1Char(' ') + job.row.url + QLatin1Char(' ') + QString::fromLatin1(className(target.cls)) + QLatin1Char(' ')
            + QString(got).replace(QLatin1Char(' '), QLatin1Char('-')) + QLatin1Char(' ') + QString::number(r.requests) + QLatin1Char(' ')
            + QString::number(clock.elapsed()) + QStringLiteral("ms ") + detail(r) + (hopDiff ? QStringLiteral(" hop refused ") + r.hopRefused : QString())
            + (job.round > 0 ? QStringLiteral(" (seq ") + QString::number(job.round) + QLatin1Char(')') : QString()) + QStringLiteral(" (expected: ") + expect
            + QLatin1Char(')'));
        if (cards || job.row.note.contains(QLatin1String("--card")))
            sayCard(r);
        if (verbose || job.row.note.contains(QLatin1String("requests")))
            say(QStringLiteral("  requested: ") + transport->takeRequested().join(QStringLiteral(" ")));
        if (ok)
            ++passed;
        else if (job.row.flaky && !hopDiff)
            ++flaky;
        else {
            ++diffs;
            failedRows << job.row.url;
        }
        ++index;
        const bool seqNow = index < jobs.size() && jobs.at(index).round > 0;
        pause.start(seqNow ? 0 : 2000);
    });
    QObject::connect(&pause, &QTimer::timeout, &app, [&] { next(); });
    QTimer::singleShot(0, &app, next);
    app.exec();
    QObject::disconnect(done);
    fetcher.shutdown();
    say(QStringLiteral("summary: %1 rows, %2 pass, %3 diff, %4 flaky diff").arg(jobs.size()).arg(passed).arg(diffs).arg(flaky));
    for (const QString& url : qAsConst(failedRows))
        say(QStringLiteral("  DIFF %1").arg(url));
    return diffs == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication   app(argc, argv); // picture formats come from Qt's image plugins
    const QStringList args = app.arguments();
    if (args.size() >= 2 && args.at(1) == QLatin1String("--classify"))
        return classifyOnly(args.mid(2));
    if (args.size() >= 4 && args.at(1) == QLatin1String("--list")) {
        bool    cards   = false;
        bool    verbose = false;
        int     seq     = 0;
        QString only;
        for (int i = 4; i < args.size(); ++i) {
            if (args.at(i) == QLatin1String("--card"))
                cards = true;
            else if (args.at(i) == QLatin1String("--verbose"))
                verbose = true;
            else if (args.at(i) == QLatin1String("--seq") && i + 1 < args.size())
                seq = args.at(++i).toInt();
            else if (args.at(i) == QLatin1String("--only") && i + 1 < args.size())
                only = args.at(++i);
        }
        mf::startup();
        const int code = runList(app, args.at(2), args.at(3), cards, seq, only, verbose);
        mf::shutdown();
        return code;
    }
    if (args.size() < 3) {
        say(QStringLiteral("usage: linkfetch_smoke <url> <outdir> [--max-bytes N] [--cancel-after ms] [--twice] [--verbose] [--direct] [--card]\n"
                           "       linkfetch_smoke --classify <url>...\n"
                           "       linkfetch_smoke --list <tsv> <outdir> [--card] [--seq N] [--only TEXT] [--verbose]"));
        return 3;
    }
    const QString url      = args.at(1);
    const QString outDir   = args.at(2);
    qint64        maxBytes = 0;
    int           cancelMs = -1;
    bool          twice    = false;
    bool          verbose  = false;
    bool          direct   = false; // --direct: never the Windows proxy (with --verbose)
    bool          cards    = false;
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
        else if (args.at(i) == QLatin1String("--card"))
            cards = true;
    }

    const Target target = classify(trimLinkEnd(url));
    say(QStringLiteral("kind=%1 class=%2 provider=%3 host=%4 key=%5 fetch=%6")
            .arg(QString::fromLatin1(kindName(target.kind)), QString::fromLatin1(className(target.cls)), target.provider.isEmpty() ? QStringLiteral("-") : target.provider,
                 target.host, target.key, logUrl(target.isNewClass() ? target.fetchUrl : target.url)));
    if (!target.isValid()) {
        say(QStringLiteral("not a previewable link"));
        return 2;
    }

    mf::startup();
    int  exitCode = 1;
    int  rounds   = 0;
    auto fetcher  = new Fetcher(verbose ? std::shared_ptr<http::Transport>(std::make_shared<TracingTransport>(true, direct)) : http::winHttpTransport(),
                                Fetcher::Options(), &app);
    fetcher->setLogger([](const QString& line) { say(QStringLiteral("log: ") + line); });
    QElapsedTimer clock;
    clock.start();
    qint64 lastShown = -1000;
    QObject::connect(fetcher, &Fetcher::progress, &app, [&](const QString&, qint64 received, qint64 total) {
        if (clock.elapsed() - lastShown < 500)
            return;
        lastShown = clock.elapsed();
        say(QStringLiteral("progress %1 / %2 bytes").arg(received).arg(total));
    });
    const Caps caps = capsFor(target, maxBytes);
    QObject::connect(fetcher, &Fetcher::finished, &app, [&](const QString& key, const Result& result) {
        ++rounds;
        say(QStringLiteral("finished after %1 ms: %2").arg(clock.elapsed()).arg(describe(result)));
        if (cards)
            sayCard(result);
        exitCode = result.ok ? 0 : 1;
        if (twice && rounds == 1 && result.ok) {
            Result cached;
            say(QStringLiteral("cached() says: %1").arg(Fetcher::cached(target, outDir, &cached) ? describe(cached) : QStringLiteral("nothing")));
            clock.restart();
            fetcher->load(target, caps, outDir);
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

    fetcher->load(target, caps, outDir);
    app.exec();

    QElapsedTimer down;
    down.start();
    fetcher->shutdown();
    say(QStringLiteral("shutdown took %1 ms").arg(down.elapsed()));
    delete fetcher;
    mf::shutdown();
    return exitCode;
}
