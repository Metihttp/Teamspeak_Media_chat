#pragma once

// 2.2 presence: the line in the send window that says who will see the media in the chat
// ("3 of 5 people here will see it in the chat. The others get a download link."), with a "Who?"
// button that lists the names. Registered as compose::presenceLineFactory (composehooks.h); the send
// window only places it. It updates itself while open (people join, answers arrive), always as one
// complete sentence, and keeps the height of two text lines so the buttons below never move.

#include <QWidget>

#include "core.h"
#include "peers.h"

class QLabel;
class QToolButton;

class PresenceLine : public QWidget
{
    Q_OBJECT

  public:
    explicit PresenceLine(const ChatTarget& target, QWidget* parent = nullptr);

    void    setTarget(const ChatTarget& target);
    QString text() const; // what the line says now (empty: hidden)
    // Shows summary (refresh() asks the presence directory; tools show made-up states).
    void    showSummary(const peers::PresenceSummary& summary);

  protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void refresh();
    void updateIcon();
    void reserveTwoLines();

    ChatTarget   m_target;
    QLabel*      m_icon;
    QLabel*      m_text;
    QToolButton* m_who;
    QLabel*      m_details;
    bool         m_shown = false;
};
