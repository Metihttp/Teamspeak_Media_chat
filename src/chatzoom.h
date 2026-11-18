#pragma once

// 2.7 F5: the saved chat zoom (design_features 4, design 6.3). One value for every chat: Settings::chatZoom, tenths of a
// point added to TeamSpeak's chat font (Tools > Options > Chat, which stays the base). Ctrl + mouse wheel in any chat
// sets it (Qt's own zoom, +-1 pt per notch, recorded and saved 500 ms later) and every chat follows (the visible ones
// 300 ms after the last notch, hidden ones when they are shown, before their first paint); the Settings row sets it at
// once (ChatIntegration::setChatZoom). TeamSpeak's chat font applied to a chat (Options OK / Apply) becomes that chat's
// new base and the zoom goes back on top.
//  * The base of a chat is a dynamic property on the chat widget ("tsmediaZoomBase", a QFont: Qt's own type), set when
//    the chat is first seen; it stays across plugin reloads, so a load finds a zoomed chat as it is and sets no font
//    (idempotent: no FontChange, no scale pass, the reader's place untouched).
//  * The zoom stays when the plugin unloads (no font is set at unload, R-DEF-1).
//  * New chat tabs: a ChatTabWidget (and its page stack) gaining a child runs the host's discover 0 ms later (owned
//    timer), so a new tab is zoomed before its first paint (design M4).
// CRASH RULE: owned timers (children), stopped in shutdown(); filters removed there; the property name built at run time;
// no metatype of ours (int signal only).

#include <QByteArray>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

class QTextBrowser;
class QTimer;
class QWidget;

namespace chatzoom {

constexpr qreal kDefaultBasePoints = 8.25; // TeamSpeak's chat font without Chat/Font (MS Shell Dlg 2, 11 px at 96 DPI)
constexpr qreal kMinPoints         = 4.0;  // a smaller target is raised to this

int     clampTenths(int tenths);                      // Settings::chatZoomRange
qreal   targetPoints(qreal basePoints, int tenths);   // base + tenths / 10, at least kMinPoints
int     percentOf(int tenths, qreal basePoints);      // the size as a percentage of the base: 8.25 pt, +10 -> 112
QString deltaText(int tenths);                        // "+2.0 pt", "-0.5 pt", "+0.0 pt"
QByteArray baseProperty();                            // "tsmediaZoomBase" (a run-time copy)
QByteArray appliedProperty();                             // "tsmediaZoomSet": the font ChatZoom set last (a run-time copy)

} // namespace chatzoom

class ChatZoom : public QObject
{
    Q_OBJECT

  public:
    struct Host {
        std::function<void()>          endLoadHolds; // before a font change from outside the chat (R-DEF-1, design M3.2)
        std::function<void()>          discover;     // a chat tab widget gained a child: find new chats (0 ms later)
        std::function<QTextBrowser*()> visible;      // the chat the user looks at (the base shown in Settings)
        // 2.7.2 (D4): the zoom was set at this chat's Show and its place put back at once: a scale pass that the new font
        // started takes that place again (ChatScale::retake, as after ShowPlace's put-back). Optional.
        std::function<void(QTextBrowser*)> placed;
        // 2.7.2 (D4): the chat's look for the new font on screen now (ChatLayout::styleNow), inside the Show's place
        // bracket: a restyle after the first paint would move the reader's line by the new size of the rows above it in
        // its message (a few px). Optional.
        std::function<void(QTextBrowser*)> restyle;
    };

    explicit ChatZoom(Host host, QObject* parent = nullptr);
    ~ChatZoom() override;

    // A chat ChatIntegration attached: its filters (FontChange and Show on the chat, Wheel on its viewport), its base,
    // and the zoom now (nothing when it has it already).
    void attach(QTextBrowser* browser);
    // A ChatTabWidget: its new pages are found at once (design M4).
    void watchTabs(QWidget* tabWidget);

    int   zoom() const;                        // Settings::chatZoom
    qreal basePoints(QTextBrowser* browser) const; // that chat's base point size (kDefaultBasePoints when unknown)
    int   percent() const;                     // on the visible chat's base
    // A new value for every chat (the Settings row, a test hook): clamped, the load holds end, every visible chat now
    // (hidden ones when shown), saved at once, chatZoomChanged.
    void  setChatZoom(int tenths);
    // Unload: timers stopped (a pending save is made now), filters removed. No font is changed.
    void  shutdown();

    // ---- tests ----
    bool ensure(QTextBrowser* browser); // the chat's font is base + zoom (true: it was set now)
    int  fontsSet() const { return m_fontsSet; }

  signals:
    void chatZoomChanged(int tenths); // Ctrl + wheel or setChatZoom changed Settings::chatZoom

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QTextBrowser* browserOf(QObject* watched) const; // the chat of a viewport (or the chat itself)
    bool          hasBase(QTextBrowser* browser) const;
    QFont         baseOf(QTextBrowser* browser) const;
    void          setBase(QTextBrowser* browser, const QFont& font);
    bool          isOurs(QTextBrowser* browser, const QFont& font) const; // the font we set last (an older zoom)
    QFont         targetFor(const QFont& base, int tenths) const;
    bool          sameSize(const QFont& a, const QFont& b) const;
    void          fontChanged(QTextBrowser* browser);
    void          userZoomed(QTextBrowser* browser);
    void          queueApply(QTextBrowser* browser);
    void          applyQueued();
    void          syncVisible();
    bool          due(QTextBrowser* browser) const; // ensure() would set a font now
    void          shownZoomed(QTextBrowser* browser); // 2.7.2 (D4): the zoom at the chat's Show, its place kept at once

    Host                            m_host;
    QVector<QPointer<QTextBrowser>> m_browsers;
    QVector<QPointer<QObject>>      m_tabObjects; // ChatTabWidgets and their page stacks (filtered for ChildAdded)
    QVector<QPointer<QTextBrowser>> m_applyQueue;
    QPointer<QTextBrowser>          m_wheel; // a Ctrl + wheel is being handled on this chat (until the next turn)
    QTimer*                         m_wheelEnd     = nullptr; // children: 0 ms, single shot
    QTimer*                         m_sync         = nullptr; // 300 ms after the last notch: the visible chats follow
    QTimer*                         m_apply        = nullptr; // 0 ms: a new base or a bound put back (never in a font event)
    QTimer*                         m_save         = nullptr; // 500 ms after the last notch
    QTimer*                         m_discoverSoon = nullptr; // 0 ms: a tab widget gained a page
    bool                            m_applying = false;       // our own setFont (never taken for TeamSpeak's or a user's)
    bool                            m_down     = false;
    int                             m_fontsSet = 0;
};
