#pragma once

// 2.2 reactions: the small popup with the six reactions (opened by the add button, the add pill or
// "Add reaction" without a hover). Keyboard: Left/Right/Home/End move, Enter or Space toggles and
// closes, 1-6 toggle directly, Esc closes. Your current reactions are shown as checked. A top-level
// popup named "tsmedia..." so plugin shutdown deletes it if it is still open.

#include <QColor>
#include <QVector>
#include <QWidget>

class QAbstractButton;

class ReactionPicker : public QWidget
{
    Q_OBJECT

  public:
    // ownMask: your reactions on this media (checked). base: the chat's background colour.
    ReactionPicker(bool dark, const QColor& base, int ownMask, QWidget* parent = nullptr);

    // Opens below anchor (global coordinates), right-aligned with it; above it when there is no room.
    void openAt(const QRect& anchor);

    static QSize fixedSize();

  signals:
    void picked(int reaction); // then it closes

  protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void choose(int reaction);
    void moveFocus(int index);

    bool                      m_dark;
    QColor                    m_base;
    QVector<QAbstractButton*> m_buttons;
};
