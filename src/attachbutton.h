#pragma once

// 2.3 attach: the paperclip in TeamSpeak's chat input (ChatLineEdit, a QTextEdit), in the input's left
// slot: the margin EmojiInput reserves at the input's left edge (the viewport is narrowed, so text and
// TeamSpeak's "Enter Chat Message..." placeholder start right of it). EmojiInput::layout() calls placeIn()
// with that strip, and removeFrom() when it gives the input back.
//
// Grey like the microphone and the emoji face; darker under the pointer, the accent colour while pressed
// or while its menu is open. A click opens the attach menu (attachpopup.h) with the entries registered in
// attachmenu.h; a click while it is open closes it. The button never takes the focus (typing stays in the
// input). Tool tip "Attach", accessible name "Attach a file, a poll or live music". objectName "tsmediaAttachButton",
// so plugin shutdown's sweep finds it among TeamSpeak's widgets.

#include <QAbstractButton>
#include <QPointer>

#include "attachpopup.h"
class QTextEdit;

class AttachButton : public QAbstractButton
{
    Q_OBJECT

  public:
    static constexpr int kIcon = 24; // logical px: the glyph's box inside the strip

    explicit AttachButton(QWidget* input);
    ~AttachButton() override;

    // ---- EmojiInput's slot ---------------------------------------------------------------------------
    static AttachButton* placeIn(QTextEdit* input, const QRect& strip); // made on first use, shown, raised
    static void          removeFrom(QTextEdit* input);                  // deletes input's button, if any
    static AttachButton* of(const QWidget* input);

    void         place(const QRect& strip);
    QRect        iconRect() const { return QRect(m_icon, QSize(kIcon, kIcon)); }
    bool         menuOpen() const { return !m_popup.isNull(); }
    AttachPopup* popup() const { return m_popup.data(); }
    void         openMenu(); // what a click does (tests, the live-test driver)

    struct Look {
        bool hover   = false;
        bool pressed = false;
        bool open    = false;
        bool dark    = false;
    };
    // The button as drawn in a box of kIcon x kIcon logical px (render tools).
    static void paintIcon(QPainter& p, const QRectF& box, const Look& look);

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& pos) const override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    bool dark() const;

    QPoint                m_icon;
    QPointer<AttachPopup> m_popup;
};
