#pragma once

// 2.2 emoji: a bounded least-recently-used cache of rendered emoji pictures, keyed by the emoji's text
// and the picture's size in device pixels. Bounded by both the number of pictures and their bytes.
// QtGui only (unit-tested); not thread-safe (the renderer uses it on the GUI thread).

#include <QHash>
#include <QImage>
#include <QString>

#include <list>

namespace emoji {

class ImageCache
{
  public:
    explicit ImageCache(int maxEntries = 2048, qint64 maxBytes = 24LL * 1024 * 1024)
        : m_maxEntries(qMax(1, maxEntries))
        , m_maxBytes(qMax<qint64>(1, maxBytes))
    {
    }

    // A null image when it isn't cached; a hit becomes the most recently used picture.
    QImage find(const QString& text, int pixels)
    {
        const auto it = m_index.find(keyOf(text, pixels));
        if (it == m_index.end()) {
            ++m_misses;
            return {};
        }
        m_lru.splice(m_lru.begin(), m_lru, it.value());
        ++m_hits;
        return it.value()->image;
    }

    void insert(const QString& text, int pixels, const QImage& image)
    {
        if (image.isNull())
            return;
        const QString key  = keyOf(text, pixels);
        const qint64  cost = costOf(image);
        if (cost > m_maxBytes)
            return; // larger than the whole cache: not kept
        const auto old = m_index.find(key);
        if (old != m_index.end()) {
            m_bytes -= old.value()->cost;
            m_lru.erase(old.value());
            m_index.erase(old);
        }
        m_lru.push_front(Node{key, image, cost});
        m_index.insert(key, m_lru.begin());
        m_bytes += cost;
        trim();
    }

    void clear()
    {
        m_lru.clear();
        m_index.clear();
        m_bytes = 0;
    }

    void setLimits(int maxEntries, qint64 maxBytes)
    {
        m_maxEntries = qMax(1, maxEntries);
        m_maxBytes   = qMax<qint64>(1, maxBytes);
        trim();
    }

    int    entries() const { return static_cast<int>(m_index.size()); }
    qint64 bytes() const { return m_bytes; }
    qint64 hits() const { return m_hits; }
    qint64 misses() const { return m_misses; }
    int    maxEntries() const { return m_maxEntries; }
    qint64 maxBytes() const { return m_maxBytes; }

    static qint64 costOf(const QImage& image) { return static_cast<qint64>(image.sizeInBytes()) + 64; }

  private:
    struct Node {
        QString key;
        QImage  image;
        qint64  cost = 0;
    };

    static QString keyOf(const QString& text, int pixels) { return text + QChar(0x1f) + QString::number(pixels); }

    void trim()
    {
        while (!m_lru.empty() && (static_cast<int>(m_index.size()) > m_maxEntries || m_bytes > m_maxBytes)) {
            const Node& last = m_lru.back();
            m_bytes -= last.cost;
            m_index.remove(last.key);
            m_lru.pop_back();
        }
    }

    std::list<Node>                                m_lru; // most recently used first
    QHash<QString, std::list<Node>::iterator>      m_index;
    int                                            m_maxEntries;
    qint64                                         m_maxBytes;
    qint64                                         m_bytes  = 0;
    qint64                                         m_hits   = 0;
    qint64                                         m_misses = 0;
};

} // namespace emoji
