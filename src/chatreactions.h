#pragma once

// 2.2 reactions in the chat. Owned by ChatIntegration, which calls it at five places (marked
// "2.2 reactions" in chatintegration.cpp): composing a preview, narrowing a hit to the picture, the
// viewport's events, the context menu, and nothing else. It:
//  * draws the reaction row under the media into the same preview object (only once the media has a
//    reaction; hovering never changes the size) and the round add button over a hovered picture;
//  * handles hover, clicks and tooltips on the pills, the add pill and the add button (a drag never
//    starts there; the picture keeps all of its own behaviour);
//  * adds "Add reaction" to the preview's context menu (the way without hover);
//  * opens the picker, shows why a reaction couldn't be sent, and tells the hub which media keys
//    are in the chat (SYNC).
// No reactions in the server chat. Audio and voice cards have no add button on the card: they use
// the row's add pill and the menu. An album has one row, under its grid, keyed by its first item
// (ai=1): its preview object is the grid, the reactions are that item's.

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QTextBrowser>

#include "previewrenderer.h"
#include "reactionart.h"

class ChatIntegration;
class Core;
class QEvent;
class QMenu;
class ReactionPicker;

class ChatReactions : public QObject
{
    Q_OBJECT

  public:
    ChatReactions(ChatIntegration* chat, Core* core);
    ~ChatReactions() override;

    // The preview object for key: picture (its logical size in *size) plus the row below and the add
    // button on it. *size becomes the object's size. Returns picture itself when there is nothing to add.
    QImage compose(QTextBrowser* browser, const QString& key, const QImage& picture, const PreviewStyle& style, QSize* size);
    // The picture's part of a preview object's rect (video zones, clicks and drags use only that).
    QRectF pictureRect(QTextBrowser* browser, const QString& key, const QRectF& objectRect) const;
    // Viewport events; true when one was for the reactions (ChatIntegration then stops).
    bool filterEvent(QObject* watched, QEvent* event);
    // "Add reaction" in the preview's context menu.
    void addMenu(QMenu* menu, QTextBrowser* browser, const QString& key);

    // Which chat a browser shows (decided while it is visible: its tab is the current one).
    enum class Kind { Unknown, Server, Channel, Private };

  private:
#ifdef TSMEDIA_TESTHOOKS
    friend class SelfTest; // test builds: the live-test driver clicks the row's pills (selftest.cpp)
#endif
    struct Geometry {
        QSize         picture; // logical
        rx::RowLayout row;
        bool          button = false; // the add button is drawn on the picture
    };
    struct Spot {
        QPointer<QTextBrowser> browser;
        QString                key;
        rx::Zone               zone  = rx::Zone::None;
        int                    index = -1;

        bool same(const Spot& o) const { return browser == o.browser && key == o.key && zone == o.zone && index == o.index; }
    };
    struct Hit {
        QString     key;
        QRectF      object;
        rx::ZoneHit zone;
    };

    Hit     hitAt(QTextBrowser* browser, const QPoint& pos) const;
    Kind    kindOf(QTextBrowser* browser, bool reclassify = false);
    void    track(QTextBrowser* browser);
    bool    eligible(QTextBrowser* browser, const QString& key, Kind* kind);
    bool    canAdd(Kind kind) const;
    void    setSpot(const Spot& spot);
    void    refresh(QTextBrowser* browser, const QString& key);
    void    refreshLater(QTextBrowser* browser, const QString& key);
    void    refreshKey(const QString& key);
    void    collapseKept(const QString& stillOn);
    void    toggle(QTextBrowser* browser, const QString& key, int reaction); // 2.2 emoji: an emoji id
    void    openPicker(QTextBrowser* browser, const QString& key, const QRect& anchor);
    void    openFullPicker(QTextBrowser* browser, const QString& key, const QRect& anchor); // 2.2 emoji: any emoji
    QString pillToolTip(const QString& key, int reaction) const;
    QString addToolTip() const;
    QString blockedText(QTextBrowser* browser) const; // why you can't react in this chat now; empty: you can
    QStringList presentKeys(const QString& serverUid) const;
    QColor  baseOf(QTextBrowser* browser) const;

    ChatIntegration*                               m_chat;
    Core*                                          m_core;
    QHash<QTextBrowser*, QHash<QString, Geometry>> m_geometry;
    QSet<QTextBrowser*>                            m_tracked; // destroyed() connected
    QHash<QTextBrowser*, Kind>                     m_kinds;
    QHash<QTextBrowser*, qint64>                   m_kindCheckedMs;
    Spot                                           m_hover;
    Spot                                           m_press;
    QString                                        m_objectKey; // preview object under the pointer (picture or row)
    QSet<QString>                                  m_keep;      // rows kept under the pointer after the last reaction went
    QPointer<ReactionPicker>                       m_picker;
    QPointer<QWidget>                              m_fullPicker; // 2.2 emoji
};
