#pragma once

// 2.5 GIFs: the GIFs page of the emoji picker (design_gifs 5): a grid of the saved GIFs (3 columns of 116 x 86 tiles),
// the footer under it, the options menu and the "Remove all" confirmation bar. It lives inside the picker (a child of
// the popup, below the Emoji / GIFs switch row) and only reads the library: thumbnails and animation frames come from
// GifLibrary's workers (this file never decodes a GIF), loops from one muted mf::VideoPlayer per opening.
//
//  * The order is a snapshot taken when the page first shows in an opening: nothing reorders under the pointer; removed
//    items leave at once. A press records the id under the pointer and the release sends only that id.
//  * Only the hovered or keyboard-focused tile animates, after a 150 ms rest (600 ms with Windows animations off).
//  * Enter sends only after keyboard navigation in this opening; Space never sends; no key removes anything.
// Fixed-pixel, like the rest of the picker (design_gifs 5.11): every size is in kTile* / kCell* below.

#include <QImage>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "giflibrary.h"
#include "pickerstyle.h"

class QTimer;
class GifGrid;
class GifConfirmBar;

namespace mf {
class VideoPlayer;
}

class GifPanel : public QWidget
{
    Q_OBJECT

  public:
    static constexpr int kTileW   = 116;
    static constexpr int kTileH   = 86;
    static constexpr int kCellW   = 120;
    static constexpr int kCellH   = 90;
    static constexpr int kColumns = 3;
    static constexpr int kGridW   = 368; // 360 for the tiles and the 8 px scroll bar
    static constexpr int kGridH   = 371;
    static constexpr int kFooterH = 52;
    static constexpr int kBarH    = 44;
    static constexpr int kTop     = 4; // the grid's top in the panel (the panel starts under the switch row's divider)
    // A mouse click on the grid counts only when its press comes this long after the page showed (the picker opened on
    // the GIFs page, or a page switch): a habitual click where the emoji page's recent emoji sit never sends a GIF.
    static constexpr int kOpenGuardMs = 400;

    // opened: the host's state when the picker opened (its fingerprint is what every send is checked against).
    GifPanel(const pickerstyle::Colors* colors, const gifs::Host& host, const gifs::SendState& opened, QWidget* parent);
    ~GifPanel() override;

    void            focusGrid(Qt::FocusReason reason = Qt::OtherFocusReason); // the grid has the keyboard; no item is focused
    void            setShown(bool shown);                                     // the page shows or hides (the animation stops)
    void            refreshState();                                           // the host's state again (the page shows)
    void            openMenu(const QPoint& globalPos, const QString& id);     // id empty: the options menu (the ⋯ button)
    QWidget*        grid() const;
    QList<QWidget*> keyWidgets() const; // the grid and the bar's buttons (the picker guards their shortcuts)
    bool            confirmShown() const;
    void            showConfirm();
    void            hideConfirm();

    // ---- tests and the live-test driver ----------------------------------------------------------------------------
    QStringList order() const { return m_order; }
    QRect       tileRect(int index) const; // panel coordinates (it may be scrolled out of view)
    QString     focusedId() const { return m_focus; }
    QString     hoveredId() const { return m_hover; }
    QString     animatingId() const { return m_anim; }
    bool        animating() const;
    int         framePaints() const { return m_framePaints; }
    int         playersCreated() const { return m_playersCreated; }
    QString     footerTitle() const;
    QString     footerText() const;
    QStringList footerTextLines() const; // line 2 as drawn (two lines for a long reason)
    bool        footerDanger() const;
    void        hoverIndex(int index);               // the pointer rests on tile index (-1: leaves)
    bool        sendIndex(int index, bool keepOpen); // a click on tile index
    void        scrollTo(int contentY);
    void        removeIndex(int index);              // the menu's Remove
    void        confirmRemoveAll();                  // the bar's Remove all
    static void setAnimationsEnabledForTests(int enabled); // 1 / 0; -1: Windows' setting
    // Remove all's question (none: the leftover copies; one: "the GIF"; else "all N GIFs").
    static QString removeAllQuestion(int count);
    // A tool tip as QToolTip must get it: escaped (a file name may be someone else's and look like HTML), lines kept.
    static QString toolTipHtml(const QString& text, bool oneLinePerLine);
    QString        toolTipFor(int index) const; // the tile's tool tip text (plain; tests)
    bool           pressGuarded() const;        // within kOpenGuardMs of the page showing

  signals:
    void sent(bool keepOpen);
    void typed(const QString& text); // a printable character on the page: the picker switches to Emoji and types it
    void upFromGrid();               // Up from the first row: the switch row
    void closeRequested();

  protected:
    void paintEvent(QPaintEvent* event) override; // the footer
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;   // the picker window shows (on this page): the click guard starts

  private:
    friend class GifGrid;
    friend class GifConfirmBar;

    struct Note {
        QString title, text;
        bool    danger = false;
    };

    const gifs::Item* item(const QString& id) const;
    QString           blockOf(const gifs::Item& item) const; // why it can't go now
    QString           hostOf(const gifs::Item& item) const;
    QString           labelOf(const gifs::Item& item) const; // accessible
    QString           toolTipOf(const gifs::Item& item) const;
    bool              covered(const gifs::Item& item) const { return item.spoiler && !m_revealed.contains(item.id); }
    QString           footerSubject() const; // the hovered or focused id
    Note              footerNote() const;
    static QStringList footerLines(const Note& note, const QFont& font, int width);
    void              activate(const QString& id, bool keepOpen, const QPoint& globalTip);
    void              setTarget(const QString& id);
    void              startAnimation();
    void              stopAnimation();
    void              onAnimationReady(const QString& id);
    void              nextFrame();
    void              ensurePlayer();
    void              libraryChanged();
    void              layoutChildren();
    void              reveal(const QString& id);

    const pickerstyle::Colors* m_colors;
    gifs::Host                 m_host;
    QPointer<GifLibrary>       m_lib;
    gifs::SendState            m_opened; // at opening (the fingerprint)
    gifs::SendState            m_state;  // shown now
    bool                       m_changed = false; // a send found another chat: every tile is disabled for this opening
    QStringList                m_order;
    QString                    m_hover, m_focus, m_press;
    bool                       m_keyboard     = false; // a navigation key was used in this opening
    QElapsedTimer              m_shownClock;            // since the page showed (kOpenGuardMs)
    bool                       m_hoverWasLast = true;
    QSet<QString>              m_revealed;
    QSet<QString>              m_noAnimation; // failed or refused this opening: the still
    Note                       m_note;        // a send's outcome (until the pointer or focus moves)
    bool                       m_hasNote = false;
    GifGrid*                   m_grid    = nullptr;
    GifConfirmBar*             m_bar     = nullptr;
    // The animation (5.7): one at a time.
    QTimer*          m_dwell      = nullptr;
    QTimer*          m_frameTimer = nullptr;
    QString          m_target; // the tile resting under the pointer or the focus
    QString          m_anim;   // the tile animating (or about to)
    QVector<QImage>  m_frames;
    QVector<int>     m_delays;
    int              m_frame          = 0;
    mf::VideoPlayer* m_player         = nullptr; // one per opening, created at the first loop hover
    int              m_framePaints    = 0;
    int              m_playersCreated = 0;
};
