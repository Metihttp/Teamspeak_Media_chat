#pragma once

// Live-test builds only (TSMEDIA_TESTHOOKS): every request, name lookup and refused connection the plugin's two
// Fetchers make, kept in memory for the driver's {"cmd":"links.net"} (which prints and clears them). The live pass
// reads exactly which addresses a link asked (a share page, its preview, a LAN name never resolved) instead of only
// the request count of the "Link preview (...)" log line. Release builds have none of this.

#ifdef TSMEDIA_TESTHOOKS

#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QTime>

#include <memory>

#include "transport.h"

namespace linkpreview::nettrace {

struct Book {
    QMutex      mutex;
    QStringList lines;
};

inline Book& book()
{
    static Book b;
    return b;
}

inline void note(const char* who, const QString& what)
{
    Book&        b = book();
    QMutexLocker lock(&b.mutex);
    if (b.lines.size() >= 4000)
        b.lines.removeFirst();
    b.lines << QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")) + QLatin1Char(' ') + QLatin1String(who) + QLatin1Char(' ') + what;
}

inline QStringList take()
{
    Book&        b = book();
    QMutexLocker lock(&b.mutex);
    QStringList  out = b.lines;
    b.lines.clear();
    return out;
}

class TracedExchange : public http::Exchange
{
  public:
    TracedExchange(std::unique_ptr<http::Exchange> inner, const char* who, QString address)
        : m_inner(std::move(inner))
        , m_who(who)
        , m_address(std::move(address))
    {
    }
    http::Error head(http::Head* out) override
    {
        const http::Error error = m_inner->head(out);
        // Concatenated, never arg() over the address: its %2F would be taken for a placeholder.
        note(m_who, QStringLiteral("  -> ") + m_address + QLatin1Char(' ') + http::errorName(error) + QStringLiteral(" status ") + QString::number(out->status)
                        + QStringLiteral(" type ") + QString::fromLatin1(out->contentType.left(60)) + QStringLiteral(" length ")
                        + QString::number(out->contentLength)
                        + (out->location.isEmpty() ? QString() : QStringLiteral(" location ") + out->location.left(300))
                        + (out->challenge ? QStringLiteral(" challenge") : QString()));
        return error;
    }
    http::Error   read(char* buffer, int capacity, int* got) override { return m_inner->read(buffer, capacity, got); }
    unsigned long systemError() const override { return m_inner->systemError(); }
    bool          connectedToServer() const override { return m_inner->connectedToServer(); }
    unsigned long secureFailureFlags() const override { return m_inner->secureFailureFlags(); }

  private:
    std::unique_ptr<http::Exchange> m_inner;
    const char*                     m_who;
    QString                         m_address;
};

class TracedTransport : public http::Transport
{
  public:
    TracedTransport(std::shared_ptr<http::Transport> inner, const char* who)
        : m_inner(std::move(inner))
        , m_who(who)
    {
    }
    http::Error resolve(const QString& host, QVector<net::Address>* out, const http::Cancel& cancel, quint64 deadline) override
    {
        const http::Error error = m_inner->resolve(host, out, cancel, deadline);
        QStringList       addresses;
        for (const net::Address& address : *out)
            addresses << address.toString() + (net::isPublicAddress(address) ? QString() : QStringLiteral(" (not public)"));
        note(m_who, QStringLiteral("RESOLVE %1: %2 [%3]").arg(host, http::errorName(error), addresses.join(QStringLiteral(", "))));
        return error;
    }
    std::unique_ptr<http::Exchange> open(const http::HopOptions& options, const http::Cancel& cancel, http::Error* error) override
    {
        http::HopOptions traced  = options;
        const char*      who     = m_who;
        traced.allowPeer         = [allow = options.allowPeer, who](const net::Address& address) {
            const bool ok = allow && allow(address);
            if (!ok)
                note(who, QStringLiteral("REFUSED connection to %1").arg(address.toString()));
            return ok;
        };
        const QString address = options.url.toString(QUrl::FullyEncoded);
        // 2.6: "(proxy)" for the Windows proxy, "(chat proxy)" for the bridge (never its port or token)
        const QString via = options.route.via == http::Route::Via::Bridge ? QStringLiteral(" (chat proxy)")
                            : options.route.via == http::Route::Via::System ? QStringLiteral(" (proxy)")
                                                                              : QString();
        note(m_who, QStringLiteral("GET %1%2").arg(address, via));
        auto exchange = m_inner->open(traced, cancel, error);
        if (!exchange) {
            note(m_who, QStringLiteral("  -> %1 open failed: %2").arg(address, http::errorName(*error)));
            return nullptr;
        }
        return std::unique_ptr<http::Exchange>(new TracedExchange(std::move(exchange), m_who, address));
    }
    http::Route route(const QUrl& url, const http::Cancel& cancel, quint64 deadline) override { return m_inner->route(url, cancel, deadline); }

  private:
    std::shared_ptr<http::Transport> m_inner;
    const char*                      m_who;
};

inline std::shared_ptr<http::Transport> traced(std::shared_ptr<http::Transport> inner, const char* who)
{
    return std::make_shared<TracedTransport>(std::move(inner), who);
}

} // namespace linkpreview::nettrace

#endif
