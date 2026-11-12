#pragma once

// 2.5 zoom (design2.md 1.9, 1.10, 1.12): one ChatScale per attached chat, owned by ChatIntegration.
//  * Live re-layout without jumps: a FontChange on the chat (Ctrl + wheel, TeamSpeak's chat font, a monitor move)
//    or a changed scale key asks for a pass. The first event of a burst captures the reader's place; every event
//    restarts a 150 ms settle timer (a wheel gesture is one pass); during the gesture a throttled, anchor-only hold
//    puts the line being read back after Qt's own relayout. At settle: the new scale, every module's new sizes in ONE
//    edit block (the document lays out once), the place restored once (a chat at the bottom stays at the bottom),
//    then only what is near the view is drawn.
//  * The picture budget: every preview picture added to the chat's document goes through putPicture(), which books
//    its bytes. Over the budget, pictures far from the view are swapped for a blank stand-in (the format keeps its
//    size, nothing moves), farthest first, down to 75% of the budget; their module draws them again when they come
//    near.
// It knows nothing of the modules: the host (ChatIntegration, or a test) computes and applies the scale. CRASH RULE:
// its timers are children, stopped in prepareShutdown(); no functor singleShot. It filters FontChange / StyleChange
// on the chat widget itself (seen before QTextEdit hands the font to its document) and removes that filter again.

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QString>

#include <functional>

#include "chatscroll.h"
#include "previewscale.h"

class QTextBrowser;
class QTimer;

class ChatScale : public QObject
{
    Q_OBJECT

  public:
    struct Host {
        std::function<PreviewScale()>              compute; // the scale the chat asks for now (previewscale::computeScale)
        std::function<void(const PreviewScale&)>  apply;   // inside the pass's edit block: every object's size, no drawing
        std::function<void()>                      after;   // after the place came back: draw what is near the view
        std::function<void(const PreviewScale&)>  changed; // the scale changed (the log line)
        std::function<void()>                      fontEvent; // a FontChange / StyleChange on the chat (may call request())
        std::function<void(const QString& name, int module)> swapped; // a picture became the blank stand-in: draw it again near the view
        std::function<void(const QString& line)>   log;
    };

    // The modules that book pictures (for the swap callback and the log).
    enum Module { Media = 0, Links, Polls, Replies, TextReactions };

    static constexpr int kSettleMs   = 150;
    static constexpr int kHoldMs     = 100; // at most one hold per this long during a gesture
    static constexpr int kSwapMargin = 800; // pictures within this many px of the view are never swapped (2 x 400)

    ChatScale(QTextBrowser* browser, Host host, QObject* parent = nullptr);
    ~ChatScale() override;

    QTextBrowser*       browser() const { return m_browser.data(); }
    const PreviewScale& scale() const { return m_scale; }
    void                setScale(const PreviewScale& scale) { m_scale = scale; } // the first one (attach): no pass
    // A pass is wanted (a FontChange, a changed key, the Preview size setting).
    void request();
    bool pending() const;
    // The pass now (tests; the settle timer's slot).
    void settle();
    // Unload: timers stopped, the filter removed, nothing pending; no host call after this.
    void prepareShutdown();
    int  passes() const { return m_passes; } // tests
    int  holds() const { return m_holds; }

    // ---- the picture budget (1.12) ----
    // 128 MiB per chat on x64, 48 MiB on x86 (a test can set another).
    static qint64 defaultBudget() { return sizeof(void*) == 8 ? 128LL * 1024 * 1024 : 48LL * 1024 * 1024; }
    void          setBudget(qint64 bytes) { m_budget = bytes; }
    qint64        budget() const { return m_budget; }
    // name's picture into the chat's document (QTextDocument::addResource) and into the books (w x h x 4 bytes).
    void putPicture(const QString& name, const QPixmap& pixmap, int module);
    // A picture of ours left the document (or became a stand-in the module made itself): 0 bytes.
    void forget(const QString& name);
    // TeamSpeak swapped the chat's document: every booked picture went with the old one (review fix).
    void resetLedger();
    qint64 pictureBytes() const { return m_total; }
    int    pictureCount() const { return m_pictures.size(); }
    bool   isSwapped(const QString& name) const;
    // Over the budget: the farthest pictures (more than kSwapMargin from the view) become blank stand-ins until the
    // total is at most 75% of the budget; booked pictures no fragment shows any more (a block TeamSpeak's line limit
    // trimmed, a preview that went) count as the farthest of all, and their stand-in frees the document's copy.
    // Returns how many were swapped. Runs after a put that grew the books (coalesced) and after the 300 ms visibility
    // pass; a scan that couldn't reach the target isn't repeated until the books grow, the document changes or the
    // chat scrolls (no full walk per GIF or video frame, review fix).
    int enforceBudget();
    int budgetScans() const { return m_scans; } // tests
    // The blank stand-in (1 x 1, transparent).
    static QPixmap blankPicture();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Picture {
        qint64 bytes  = 0;
        int    module = Media;
        bool   swapped = false;
    };

    void hold();

    QPointer<QTextBrowser> m_browser;
    Host                   m_host;
    PreviewScale           m_scale;
    QTimer*                m_settle = nullptr;
    QTimer*                m_hold   = nullptr;
    QTimer*                m_budgetTimer = nullptr;
    QElapsedTimer          m_lastHold;
    chatscroll::Place      m_place;
    bool                   m_burst    = false; // a place was captured for the pass that is pending
    bool                   m_down     = false;
    int                    m_passes   = 0;
    int                    m_holds    = 0;

    QHash<QString, Picture> m_pictures;
    qint64                  m_total  = 0;
    qint64                  m_budget = defaultBudget();
    bool                    m_overLogged = false; // the "within the view's reach alone over the budget" line, once
    // The last scan couldn't reach the target: skipped while nothing it depends on changed.
    bool                    m_stuck         = false;
    int                     m_stuckRevision = -1;
    int                     m_stuckScroll   = -1;
    qint64                  m_stuckTotal    = 0;
    int                     m_scans         = 0;
};
