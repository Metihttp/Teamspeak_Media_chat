#pragma once

// 2.2 emoji: HD emoji in TeamSpeak's chat documents (for people with this plugin; everyone else keeps
// TeamSpeak's own rendering). Owned by ChatIntegration, which hands over every chat view it finds.
//
//  * Emoji in messages (incoming and your own, channel / server / private chats, history) become inline
//    pictures from the colour renderer, about 1.375 x the text size and bottom-aligned like Discord's,
//    and 48 px "jumbo" ones when a message is nothing but 1-27 emoji. TeamSpeak's own emoticons (":)"
//    shown as "emoticons:smile.svg") are swapped for the HD emoji they stand for.
//  * Left alone: links (also nicknames and file names in them), everything from the first ts3file link
//    of a message on (the TS Media link and the "plugin required" note), and anything in a document
//    that isn't text or one of TeamSpeak's emoticons.
//  * Every change keeps what was there in the character format (the emoji's text, the emoticon's
//    image name and size), so restore() puts it all back exactly: when the setting is turned off and
//    when the plugin unloads. Pictures are document resources named "tsmemoji:<code>/<px>".
//  * Work is bounded: changed lines are found through the document's change signal, a few
//    milliseconds of them per step, newest first; the history behind them is worked through backwards
//    in the same steps. Pictures a step needs that aren't drawn yet are asked of the renderer's worker;
//    the line is done when they arrive. A view at the bottom stays there; otherwise the line at the top
//    of the view stays where it was.
//  * Copying: Ctrl+C on a selection with HD emoji copies the text with the emoji (and TeamSpeak's
//    emoticon codes) back in place; a copy made through TeamSpeak's own menu is corrected the same way.
//    A right click on an HD emoji offers "Copy emoji", "Copy text" and "Use in the chat input".
//  * The pointer over an emoji shows its name.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTextCursor>
#include <QVector>

class QMenu;
class QTextBlock;
class QTextBrowser;
class QTextDocument;
class QTextEdit;
class QTimer;
class EmojiInput;

class ChatEmoji : public QObject
{
    Q_OBJECT

  public:
    explicit ChatEmoji(QObject* parent = nullptr);
    ~ChatEmoji() override; // restores every document

    void attach(QTextBrowser* browser);         // a chat view
    void documentSwapped(QTextBrowser* browser); // TeamSpeak gave it a new document
    void attachInput(QTextEdit* input);         // a chat input line (the emoji button, Ctrl+E)
    void settingsChanged();                     // Settings::hdEmoji / jumboEmoji / emojiButton

    // True while this edits a document (ChatIntegration doesn't rescan for those changes).
    static bool isMutating();

    // ---- also for tests and tools ------------------------------------------------------------------
    // Everything waiting in browser's document, now (pictures drawn synchronously).
    void processNow(QTextBrowser* browser);
    // Gives the document back as it was.
    static void restore(QTextDocument* document);
    // The text of [from, to) with HD emoji and replaced emoticons as what they were (other objects left out).
    static QString originalText(QTextDocument* document, int from, int to);
    // How many HD emoji the document holds.
    static int countEmoji(QTextDocument* document);
    // Replaces everything in a document without a view (tests): pictures at dpr, drawn now.
    static void processDocument(QTextDocument* document, qreal dpr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Range {
        QTextCursor from; // stays at the start when text is inserted there
        QTextCursor to;
    };
    struct View {
        QPointer<QTextBrowser>  browser;
        QPointer<QTextDocument> document;
        QVector<Range>          dirty;      // changed since the last step
        QTextCursor             history;    // the part before it is still to be worked through (null: done)
        QSet<QString>           resources;  // names added to this document
        QSet<QString>           redraw;     // ... to be drawn again (another device pixel ratio)
        qreal                   dpr = 0;    // the pictures' device pixel ratio
        bool                    waiting = false; // a step waits for pictures from the worker
        bool                    urgent  = false; // the pictures asked for now are for new messages
    };

    View* viewFor(QTextBrowser* browser);
    void  watch(View& view);
    void  restart(View& view); // after a document swap or turning it on: the whole document
    void  schedule(int delayMs = 15);
    void  step();
    bool  stepView(View& view, qint64 deadlineMs, bool synchronous);
    // Replaces what the block has to replace; false when it needs pictures that aren't there yet.
    static bool processBlock(View& view, const QTextBlock& block, bool synchronous, bool* changed);
    void  refreshResources(View& view);
    bool  enabled() const;
    void  restoreAll();
    void  copySelection(QTextBrowser* browser);
    bool  fixClipboard();
    int   emojiAt(QTextBrowser* browser, const QPoint& viewportPos, int* position, QRect* area = nullptr) const;
    void  showMenu(QTextBrowser* browser, int position, int id, const QPoint& globalPos);
    void  insertIntoInput(int id);

    QHash<QTextBrowser*, View> m_views;
    QTimer*                    m_timer = nullptr;
    EmojiInput*                m_input = nullptr;
    QPointer<QTextBrowser>     m_copyBrowser;  // TeamSpeak's own menu (or its copy shortcut) was used in it ...
    qint64                     m_copyMs = 0;   // ... at this time: the copy that follows is corrected
    bool                       m_settingClipboard = false;
    bool                       m_lastEnabled      = false;
};
