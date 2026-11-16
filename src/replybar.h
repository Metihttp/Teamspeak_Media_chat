#pragma once

// 2.2 reply: the slim "Replying to Alice: …" bar attached above TeamSpeak's chat input while a reply is
// being written, and the same line in the send window. Neither takes the keyboard focus: the chat input
// keeps it (Enter sends the reply, Esc cancels it). Named "tsmedia..." so plugin shutdown finds them.

#include <QColor>
#include <QFont>
#include <QWidget>

#include <functional>

class QTextBrowser;

// 2.7 F4 (issue #1: the reply text was tiny on 4K): widgets above the chat input live at device pixel ratio 1 in
// TeamSpeak, so a size in pixels doesn't grow with Windows scaling. Their text and sizes follow a scale instead: the chat's
// chrome scale (ReplyBar, RepliesPopup: Windows scaling x the chat's text size) or the input's text size (HoldStrip, the
// music bar). replyart::scaledFont stays for canvases drawn at a ratio (its tests pin it).
namespace replybar {
// base's family and style at qMax(10, round(11 x factor x scale)) px.
QFont widgetFont(const QFont& base, qreal factor, qreal scale);
// The input's scale: its text size against TeamSpeak's 11 px at 100% (at least 1; 2 at 200%).
qreal inputScale(const QFont& inputFont);
// A bar above the input changes its look (change(), e.g. setTheme with a new scale). When that changed its height while it
// shows in the input's layout (not floating), the layout is applied at once, so the chat above has its new height now
// (a scale pass restores the chat's place right after it); with keepEnd, a chat that showed its end before the change
// shows it again, now and after Qt's own posted relayout. True: the height changed (review fix: a zoom left the chat
// short of its end by the bar's growth).
bool restyleInLayout(QWidget* bar, bool floating, QTextBrowser* chat, bool keepEnd, const std::function<void()>& change);
} // namespace replybar

class ReplyBar : public QWidget
{
    Q_OBJECT

  public:
    explicit ReplyBar(QWidget* parent = nullptr);

    void setReply(const QString& nick, const QColor& nickColor, const QString& snippet, bool media);
    // TeamSpeak's theme: the chat input's background and the chat font. 2.7 F4: scale is the chat's chrome scale
    // (PreviewScale::chromeRatio(), kept between 1 and 4): the text, the glyphs and every size follow it.
    void  setTheme(bool dark, const QColor& base, const QFont& font, qreal scale = 1.0);
    qreal scale() const { return m_scale; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void canceled();      // the x
    void jumpRequested(); // a click on the text: show the message being replied to

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

  private:
    enum class Part { None, Text, Close };
    Part   partAt(const QPoint& pos) const;
    QRectF closeRect() const;
    QRectF textRect() const;
    void   setHover(Part part);

    QString m_nick;
    QColor  m_nickColor;
    QString m_snippet;
    bool    m_media = false;
    bool    m_dark  = false;
    QColor  m_base;
    qreal   m_scale = 1.0; // 2.7 F4
    Part    m_hover   = Part::None;
    Part    m_pressed = Part::None;
};

// "Replying to Alice: “…”" in the send window, with an x that sends the files without the reply.
class ComposeReplyLine : public QWidget
{
    Q_OBJECT

  public:
    ComposeReplyLine(const QString& nick, const QString& snippet, bool media, QWidget* parent);

    // Another message is being replied to now (the reply hotkey while the window is open).
    void  setReply(const QString& nick, const QString& snippet, bool media);
    QSize sizeHint() const override;

  signals:
    void dropped(); // the x: send without replying (the line deletes itself)

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

  private:
    QRectF closeRect() const;

    QString m_nick;
    QString m_snippet;
    bool    m_media   = false;
    bool    m_hover   = false;
    bool    m_pressed = false;
};
