#pragma once

// 2.2 reply: "View 3 replies" — a small popup listing the replies to a message in the loaded chat
// (author, time, text or file name); a click or Enter shows that reply in the chat. Keyboard: Up/Down,
// Home/End, Page Up/Down move, Enter or Space opens, Esc closes. A top-level popup named "tsmedia..." so
// plugin shutdown deletes it if it is still open.

#include <QColor>
#include <QFont>
#include <QString>
#include <QVector>
#include <QWidget>

struct ReplyRow {
    QString nick;
    QColor  nickColor;
    QString time;  // "21:14"; empty without one
    QString text;  // the reply's text; for a file its name
    bool    media = false;
};

class RepliesPopup : public QWidget
{
    Q_OBJECT

  public:
    static constexpr int kMaxRows = 200; // listed at most (the newest)

    RepliesPopup(bool dark, const QColor& base, const QFont& chatFont, const QString& title, const QVector<ReplyRow>& rows, QWidget* parent = nullptr);

    // Opens with its top-left at pos (global), moved inside the screen.
    void openAt(const QPoint& pos);

  signals:
    void activated(int row); // an index into the rows it was given; then it closes

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    int    rowAt(const QPoint& pos) const;
    QRectF rowRect(int row) const; // in widget coordinates (scrolled)
    void   setCurrent(int row);
    void   scrollTo(int row);
    int    visibleRows() const;

    bool              m_dark;
    QColor            m_base;
    QString           m_title;
    QVector<ReplyRow> m_rows;
    int               m_offset = 0; // rows left out at the start (more than kMaxRows)
    QFont             m_font;
    int               m_rowHeight = 44;
    int               m_header    = 30;
    int               m_first     = 0;  // first row shown
    int               m_current   = 0;  // keyboard selection
    int               m_hover     = -1;
    int               m_pressed   = -1;
    bool              m_keyboard  = false;
};
