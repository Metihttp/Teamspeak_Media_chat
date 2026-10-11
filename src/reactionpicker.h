#pragma once

// 2.2 reactions: the small popup with the quick reactions (opened by the add button, the add pill or
// "Add reaction" without a hover). 2.2 emoji: eight HD emoji (the six every 2.2 client knows, then your
// two most recent others) and "+", which opens the full emoji picker so any emoji can be a reaction.
// Keyboard: Left/Right/Home/End move, Enter or Space picks and closes, 1-8 pick directly, + or = opens
// the full picker, Esc closes. Your current reactions are shown as checked. A top-level popup named
// "tsmedia..." so plugin shutdown deletes it if it is still open.

#include <QColor>
#include <QSet>
#include <QVector>
#include <QWidget>

class QAbstractButton;

class ReactionPicker : public QWidget
{
    Q_OBJECT

  public:
    // quick: the emoji ids to offer; own: your reactions on this media (checked). base: the chat's background.
    ReactionPicker(bool dark, const QColor& base, const QVector<int>& quick, const QSet<int>& own, QWidget* parent = nullptr);

    // Opens below anchor (global coordinates), right-aligned with it; above it when there is no room.
    void openAt(const QRect& anchor);
    // Chat redesign: the button that opened it (global coordinates): a press there while it is open closes
    // it, and the press isn't handed on to the button (which would open it again at once).
    void setOpener(const QRect& opener) { m_opener = opener; }

    QSize        sizeForCount() const;
    static QSize fixedSize(int quickCount = 8);

  signals:
    void picked(int reaction); // an emoji id; then it closes
    void more();               // "+": the full picker; then it closes

  protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    void choose(int index);
    void moveFocus(int index);

    bool                      m_dark;
    QColor                    m_base;
    QVector<int>              m_quick;
    QVector<QAbstractButton*> m_buttons; // the quick ones, then "+"
    QRect                     m_opener;
};
