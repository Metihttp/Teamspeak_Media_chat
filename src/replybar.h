#pragma once

// 2.2 reply: the slim "Replying to Alice: …" bar attached above TeamSpeak's chat input while a reply is
// being written, and the same line in the send window. Neither takes the keyboard focus: the chat input
// keeps it (Enter sends the reply, Esc cancels it). Named "tsmedia..." so plugin shutdown finds them.

#include <QColor>
#include <QFont>
#include <QWidget>

class ReplyBar : public QWidget
{
    Q_OBJECT

  public:
    explicit ReplyBar(QWidget* parent = nullptr);

    void setReply(const QString& nick, const QColor& nickColor, const QString& snippet, bool media);
    // TeamSpeak's theme: the chat input's background and the chat font.
    void setTheme(bool dark, const QColor& base, const QFont& font);

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
    Part    m_hover   = Part::None;
    Part    m_pressed = Part::None;
};

// "Replying to Alice: “…”" in the send window, with an x that sends the files without the reply.
class ComposeReplyLine : public QWidget
{
    Q_OBJECT

  public:
    ComposeReplyLine(const QString& nick, const QString& snippet, bool media, QWidget* parent);

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
