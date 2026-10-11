#pragma once

// 2.3 attach: the menu the paperclip opens (attachbutton.h). A vertical list of the registered entries
// (attachmenu.h) with their icons on soft tiles, drawn by us in TeamSpeak's light or dark look: rounded,
// with a shadow, above the chat input. Keyboard: Up / Down (wrapping), Home / End, Enter or Space picks,
// Esc closes; the first key shows a focus ring. A press on the paperclip while it is open closes it
// (the press isn't handed on, which would open it again). Picking an entry closes the menu first, then
// runs the entry's trigger. A top-level popup named "tsmediaAttachMenu": plugin shutdown deletes it.

#include <QColor>
#include <QList>
#include <QPointer>
#include <QWidget>

#include "attachmenu.h"

class QAbstractButton;

class AttachPopup : public QWidget
{
    Q_OBJECT

  public:
    // anchor: the button that opens it (the entries' trigger gets it); base: the chat input's background.
    AttachPopup(const QList<attachmenu::Entry>& entries, bool dark, const QColor& base, QWidget* anchor);
    ~AttachPopup() override;

    // Above opener (global coordinates), its left edge at the opener's; below it when there is no room.
    void openAt(const QRect& opener);

    int              count() const { return m_items.size(); }
    QAbstractButton* item(int index) const { return m_items.value(index); }
    QString          entryId(int index) const { return index >= 0 && index < m_entries.size() ? m_entries.at(index).id : QString(); }
    bool             keyboardUsed() const { return m_keyboard; }
    int              currentIndex() const { return m_current; } // the item the keys act on (-1: none)
    void             setCurrent(int index);

    static constexpr int kShadow = 10; // the transparent margin the shadow is drawn in

    // The open menu, if any (one at a time).
    static AttachPopup* current();

  signals:
    void picked(const QString& id); // before the trigger runs

  protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    friend class AttachItem;
    void choose(int index);
    void moveFocus(int from, int step);
    void markKeyboard();

    QList<attachmenu::Entry>  m_entries;
    QList<QAbstractButton*>   m_items;
    QPointer<QWidget>         m_anchor;
    QRect                     m_opener;
    bool                      m_dark     = false;
    QColor                    m_base;
    bool                      m_keyboard = false;
    int                       m_current  = -1;
};
