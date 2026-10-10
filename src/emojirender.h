#pragma once

// 2.2 emoji: draws emoji in colour, crisp at any size and device pixel ratio.
//
// The colour engine is DirectWrite + Direct2D with Windows' colour emoji font (Segoe UI Emoji,
// D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT) into premultiplied 32-bit pictures. d2d1.dll and
// dwrite.dll are loaded at run time, so the plugin still loads where they are missing; without them
// (or when Segoe UI Emoji has no colour glyphs) the pictures are drawn with QPainter text instead,
// and hasColor() is false (the chat then keeps TeamSpeak's own rendering).
//
// A picture Windows 11 draws for the first time costs about 3 ms (its emoji are COLR v1 gradients):
// render() is for the few a chat line or a reaction row needs; the picker asks with requestImage(),
// which draws on a worker thread and announces the pictures through notifier().
//
// Threads: the API is for the GUI thread only. The GUI thread and the worker each have their own
// engine; every COM and GDI object is created, used and released on its thread. shutdown() joins the
// worker and releases everything, including the picture cache (emojicache.h, by text and device-pixel
// size); plugin shutdown calls it after the chat integration is gone.

#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

namespace emoji {

enum class Engine {
    None,  // nothing can draw emoji (not the GUI thread)
    Color, // DirectWrite + Direct2D, colour glyphs of Segoe UI Emoji
    Text,  // QPainter text (monochrome on most systems)
};

// Starts the engine on first use. GUI thread only (other threads get None).
Engine  engine();
bool    hasColor();
QString engineDescription(); // diagnostics: "DirectWrite + Direct2D, Segoe UI Emoji Version 1.70", "QPainter text (...)"

// text (one emoji, at most 64 UTF-16 units) as a square picture of logicalPx x logicalPx at device
// pixel ratio dpr (devicePixelRatio is set). Drawn now when it isn't cached. Null for empty or overlong
// text or without an engine.
QImage render(const QString& text, int logicalPx, qreal dpr);
QImage render(int id, int logicalPx, qreal dpr); // a table emoji (emojidata.h); null for an invalid id

// The cached picture, or null.
QImage cachedImage(const QString& text, int logicalPx, qreal dpr);
// The cached picture; otherwise null, and it is drawn on the worker thread (notifier() says when).
// urgent: before everything already waiting (what is on screen now). The text engine draws at once.
QImage requestImage(const QString& text, int logicalPx, qreal dpr, bool urgent = true);
QImage requestImage(int id, int logicalPx, qreal dpr, bool urgent = true);
// Drawn ahead, after everything urgent (the picker's first page, recently used emoji).
void prefetch(const QVector<int>& ids, int logicalPx, qreal dpr);

// Announces pictures the worker has drawn (they are in the cache now). GUI thread; lives until shutdown().
class ImageNotifier : public QObject
{
    Q_OBJECT

  public:
    ImageNotifier();

  signals:
    void imagesReady();

  protected:
    bool event(QEvent* event) override;
};
ImageNotifier* notifier(); // nullptr without an engine

// Segoe UI Emoji draws text as one colour picture on this PC (false without the colour engine).
// Uncached: tools and tests. Table emoji use supported(id), which remembers the answer.
bool drawsInColor(const QString& text);
// For a table emoji: drawn in colour here (colour engine), or simply valid (text engine).
bool supported(int id);

// Joins the worker, releases both engines (COM, GDI, the DLLs) and the picture cache. The next call
// starts again.
void shutdown();

struct CacheInfo {
    int    entries = 0;
    qint64 bytes   = 0;
    qint64 hits    = 0;
    qint64 misses  = 0;
    int    pending = 0; // waiting for the worker
    qint64 drawn   = 0; // pictures the worker has drawn since it started (each asked-for picture once)
};
CacheInfo cacheInfo();
void      setCacheLimits(int maxEntries, qint64 maxBytes);

// Tests and tools: false makes the next start use the QPainter text engine (takes effect after shutdown()).
void setColorEngineAllowed(bool allowed);

} // namespace emoji
