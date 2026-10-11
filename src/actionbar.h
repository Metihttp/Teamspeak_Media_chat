#pragma once

// Chat redesign: the action bar over the hovered chat message (Discord's): Add reaction (rows with media
// that can get reactions), Reply, Copy text, More (TeamSpeak's own menu with TS Media's items). A child of
// the chat's viewport named "tsmediaActionBar"; it never takes the focus (every action is in the chat menu
// too). It fades in (80 ms) and out (60 ms), at once without Windows' animations. Copy text shows a check
// and "Copied" for a moment.

#include <QWidget>

#include "layoutart.h"

class QAbstractButton;
class QGraphicsOpacityEffect;
class QTimer;
class QVariantAnimation;

class ActionBar : public QWidget
{
    Q_OBJECT

  public:
    enum Action { React = 0, Reply = 1, Copy = 2, More = 3 };

    explicit ActionBar(QWidget* viewport);
    ~ActionBar() override;

    void setLook(const layoutart::Colors& colors, bool compact);
    void setReactVisible(bool visible); // hidden (never disabled) on rows without media
    void showBar(bool animate);
    void hideBar(bool animate);
    bool showing() const { return isVisible() && !m_hiding; }
    void flashCopied();
    // The bar itself inside the widget (around it: room for its shadow).
    QRect barRect() const;
    int   shadow() const { return kShadow; }
    QAbstractButton* button(int action) const;

  signals:
    void triggered(int action);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    static constexpr int kShadow = 4;
    void relayout();
    void fadeTo(qreal target, int ms);

    layoutart::Colors        m_colors;
    bool                     m_compact = false;
    bool                     m_hiding  = false;
    QVector<QAbstractButton*> m_buttons;
    QVariantAnimation*       m_fade   = nullptr;
    QGraphicsOpacityEffect*  m_effect = nullptr;
    QTimer*                  m_copied = nullptr;
};
