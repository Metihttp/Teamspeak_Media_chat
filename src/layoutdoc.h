#pragma once

// Chat redesign: TS Media's chat layout in TeamSpeak's chat document. QtGui only (no widgets): the tests and
// render_gallery run it on documents built like TeamSpeak's.
//
// Three looks: Cozy (Discord: names and pictures above the messages), Compact (one line per message, the
// time in front) and TeamSpeak classic (TeamSpeak's own). One mechanism serves both modern ones:
//  * TeamSpeak's header runs ("[icon]<16:00:17> "Nick": ") move into the format of one picture of ours
//    (layoutformat.h), which stands where they were;
//  * block formats give the hanging indent (left margin = the content column, text indent = minus it);
//  * hover, gutter times and link underlines are painted by ChatLayout, never edited in.
// Everything is given back exactly (unstyle, restoreAll): the text and every fragment's and block's format
// as TeamSpeak made them. Blocks are never added or removed. It is the outermost layer: applied after
// previews, albums and reply lines, removed before them; ChatReplies unstyles a block before it collapses
// a quote line in it. Everything read from the chat is untrusted: nothing in it is run or fetched.

#include <QColor>
#include <QDate>
#include <QHash>
#include <QMap>
#include <QPixmap>
#include <QSet>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTextCharFormat>
#include <QVector>

#include <functional>

#include "layoutart.h"
#include "layoutformat.h"
#include "replyart.h"

class QTextBlock;
class QTextDocument;

namespace layoutdoc {

using layoutart::Mode;
using layoutart::SystemKind;

enum class RowType { Unknown, Message, System, Day, History, Other };

// A block as TeamSpeak shows it (read through our styling when it is styled).
struct Row {
    RowType            type   = RowType::Unknown;
    bool               styled = false;              // our picture leads it
    layoutformat::Kind lead   = layoutformat::None; // that picture's kind
    // a message
    QString nick;
    QString uid;  // from the nickname's client:// link
    QString href; // that link
    int     clientId = 0;
    QColor  nickColor;
    QString time;          // "16:00:17" / "4:00:17 PM"; empty without timestamps
    bool    reply    = false; // ChatReplies' reply line in front
    bool    outgoing = false;
    // a system row
    SystemKind kind           = SystemKind::Info;
    bool       keepFormatting = false; // welcome and host messages
    bool       ownPrint       = false; // TS Media's own line
    // a day or history line
    QDate   date;
    QString dayText; // after "*** "
    qreal   timeRow = 0; // 2.3.1: a styled own message on the right: the height of its time's own row (0: none)
    // what is taken into our picture (document positions; for a block that isn't styled)
    int headerStart = -1;
    int headerEnd   = -1;
    int bodyStart   = -1; // TeamSpeak's message text
};
// A block's row. System rows are TeamSpeak's "[icon]<time> *** text" lines and its status lines without
// "*** " ([MESSAGE_INFO icon]<time> text: the server tab, private chats); "*** <date>" is a day line and
// "*** Chat begins <date> <time>" (or an icon-less "*** " line) a history marker.
Row read(const QTextBlock& block);
// The system row's kind from its text ("Alice connected to channel ...").
SystemKind systemKindOf(const QString& text, bool* keepFormatting);
// A status line's kind (one without "*** ") from TeamSpeak's own English words, names and channels given as
// "" ("\"\" connected to channel \"\""); quoted words count as names too. Only the line's start is read
// (what follows, a reason or a poke's text, is the users'). Any other line (a server's welcome or host
// message, another language) is Info and keeps TeamSpeak's formatting (keepFormatting), so it never folds.
SystemKind statusKindOf(const QString& words, bool* keepFormatting);

// What one block becomes.
struct Plan {
    Mode       mode         = Mode::Cozy;
    RowType    type         = RowType::Unknown;
    bool       continuation = false;
    int        top          = 0;
    int        bottom       = 0;
    QString    name;    // the picture's resource name
    QSizeF     size;    // its size
    QString    time;    // kTime
    QString    trailer; // system rows: "  22:05", the spaces no-break ones (empty: none)
    SystemKind kind = SystemKind::Info;
    int        tag  = 0; // kBlockTag
    qreal      nameRow = 0;   // 2.3 chat-visuals: a Cozy head's name row width (kNameRow)
    bool       own     = false; // 2.3 chat-visuals: your message (kOwn)
    bool       ownRight = false; // 2.3.1: your message on the right (kOwnRight; settle() fits it)
    qreal      topExtra = 0;     // 2.3.1: top's part for the time row of the visible block above (kTopExtra)
};

struct Env {
    Mode              mode = Mode::Cozy;
    layoutart::Tokens tokens;
    layoutart::Colors colors;
    qreal             dpr          = 1.0;
    int               contentWidth = 400; // the document's text width minus its margins
    QDate             today;
    bool              group = true;
    QString           tag;           // the instance's part of resource names
    QColor            usualNickColor; // the document's most common nickname colour
    // ChatReplies: what the reply row of block shows (false: none).
    std::function<bool(int block, replyart::Header* header)> replyHeader;
    // Mentions (P2): your nickname and uid on this chat's server (empty: unknown, no mentions).
    bool    mentions = false;
    QString ownNick;
    QString ownUid;
    // Collapsed runs of events (P2): the runs the reader opened (their first row's tag).
    bool       collapse = false;
    QSet<int>  expandedRuns;
    // 2.3 chat-visuals: tokens.bubbles is message bubbles (Cozy): wider margins, links and muted text that
    // read on the bubbles, system rows on pills (layoutbubbles.h). System rows are centred in every look.
    // 2.3.1: your messages on the right, like Telegram (only with tokens.bubbles in Cozy): no avatar and no name,
    // each bubble as wide as what it holds and flush right, its time on it (settle() fits them).
    bool ownRight = false;
};

// Mentions (P2): where text mentions nick: "@Nick" (any case), or the nickname as a whole word of three or
// more characters. [start, end) pairs, in order, never overlapping.
QVector<QPair<int, int>> mentionRanges(const QString& text, const QString& nick);
// Whether block n is a message that mentions you (its row is highlighted).
bool isMention(const QTextBlock& block);

// The plan for block n: its row type, whether it joins the message before, its margins and its picture.
// date: the block's day (invalid: unknown). arrivedMs / previousArrivedMs: when the block and the previous
// visible one were appended while watched (0: unknown; used for grouping without timestamps).
struct Neighbour {
    Row    row;
    bool   valid    = false;
    qint64 arrived  = 0;
};
Plan plan(const Env& env, const QTextBlock& block, const Row& row, const Neighbour& previous, const QDate& date, qint64 arrivedMs, int serial);

// ---- edits (the caller keeps ChatLayout's mutating flag set and may hold one edit block around many) ----
// Styles block n by plan (the block must not be styled; sanitize() it first). False if nothing was done.
bool apply(QTextDocument* doc, int n, const Plan& plan, const Env& env);
// Gives block n back as TeamSpeak made it. False if nothing of ours was there.
bool unstyle(QTextDocument* doc, int n);
// A block that took our formats over when TeamSpeak appended it (no picture of ours leads it): our
// properties go, its format becomes TeamSpeak's again. True if it changed.
bool sanitize(QTextDocument* doc, int n);
// 2.3: a character format as TeamSpeak made it, the layout's recolouring (and its bookkeeping) taken back,
// also inside an HD emoji's kept format. For modules that keep text of a styled block for later (a poll
// card): what they give back after the layout went must be TeamSpeak's.
QTextCharFormat originalCharFormat(QTextCharFormat format);
// Whether block n carries anything of ours.
bool hasOurs(const QTextBlock& block);
// Every block back; returns how many. The names of our pictures go to names (may be null).
int restoreAll(QTextDocument* doc, QStringList* names = nullptr);
// restoreAll(), and the pictures' memory too: for a document no chat shows any more.
int giveBack(QTextDocument* doc);
// New sizes for the pictures of block n (a narrower chat, a new day label); positions never move.
bool resizeObject(QTextDocument* doc, int n, const QSizeF& size);

// ---- 2.3.1: your messages on the right ---------------------------------------------------------------------
// apply() lays such a message out over the whole column first; settle() then reads its laid-out lines and gives it
// its place: as wide as its widest line (and its time), flush right at ownRightEdge(env), its text left-aligned
// (right-aligned when it is right-to-left), its time after the last line when there is room, else on a row of its
// own (the next visible block's top margin grows by that row: kTopExtra). Call it outside an edit block (it reads
// the layout). blocks: the ones just styled (the visible blocks after them get their top margins checked too);
// refit: blocks another module changed since (measured again; laid out again only when their place changed).
// Returns how many blocks changed.
int settle(QTextDocument* doc, const QVector<int>& blocks, const Env& env, const QSet<int>& refit = QSet<int>());
// The right edge of your messages' text (relative to the root frame's content; the bubble's padding is outside it),
// the widest their text may be (the content column, at most 36 f in a wide chat, like Telegram's bubbles), and
// the room block gets for it (wider for a wider picture: pictures are never broken).
qreal ownRightEdge(const Env& env);
qreal ownMaxContent(const Env& env);
qreal ownRoom(const QTextBlock& block, const Env& env);
// The right margin of such a message in doc as it is laid out now (wider than env.contentWidth while the chat's
// scroll bar is hidden): its lines end at ownRightEdge(env) (rtl: right-to-left text, right-aligned), or ownSlack
// past it (left-to-right text: a fitted line never wraps again), so its left margin alone gives its lines their
// width. alignOwnRight() gives every such message of doc its margin when the chat's width changed under the same
// env (its lines keep their width: nothing wraps again); returns how many changed.
qreal ownRightMargin(QTextDocument* doc, const Env& env, bool rtl);
int   alignOwnRight(QTextDocument* doc, const Env& env);
// What a laid-out own message on the right shows (document coordinates; valid: it is one and is laid out): its
// content's left and right edge (its lines on the bubble and its time), those lines, its time.
struct OwnPlace {
    bool    valid  = false;
    int     first  = 0; // the first line on the bubble (1 under a reply row)
    int     lines  = 0; // lines on the bubble: first .. lines - 1
    qreal   left   = 0;
    qreal   right  = 0;
    qreal   top    = 0; // the first line's top
    qreal   bottom = 0; // the last line's bottom, the time's row included
    int     time   = layoutformat::TimeNone;
    QString timeText;
    QPointF timeBaseline;
    QRectF  timeRect;
};
OwnPlace ownPlace(const QTextBlock& block, const Env& env);

// ---- 2.3.1: the chat's room at its ends ----------------------------------------------------------------------
// The root frame's top and bottom margins get tokens.chatTop and chatBottom on top of what they are (the bottom
// also the time row of your message when it is the last one: Qt leaves the last block's bottom margin out); true if
// they changed. restoreAll() gives them back (only what still has our values).
bool applyEndGap(QTextDocument* doc, const Env& env);
bool removeEndGap(QTextDocument* doc);
bool hasEndGap(QTextDocument* doc);
// How much room under the last line is ours (0: none): a chat scrolled that close to its end is at its end.
qreal endGapExtra(QTextDocument* doc);

// ---- pictures ---------------------------------------------------------------------------------------------
// What block n's picture shows (the head, compact head, system prefix or divider), drawn at env's ratio.
struct Look {
    bool    replyHover   = false;
    bool    replyPressed = false;
    QImage  avatar;      // null: initials
    QImage  replyAvatar;
};
QImage render(const Env& env, const QTextBlock& block, const QDate& date, const Look& look, QString* signature);
// What render() would draw, as a string (empty: nothing to draw): cheap, to know whether to draw again.
QString signature(const Env& env, const QTextBlock& block, const QDate& date, const Look& look);
// The head's geometry of block n's picture (tooltips, the reply row's click target); false if it has none.
bool headGeometry(const Env& env, const QTextBlock& block, const QDate& date, layoutart::HeadGeometry* geometry, QSizeF* size);
// The blank picture every name gets until it is drawn (and when it is far off screen).
QPixmap blankPicture();

// Whether a nick is a plausible one (untrusted text: length, no line breaks or objects).
QString nickFromQuoted(const QString& quoted);

// ---- collapsed runs of events (P2) ---------------------------------------------------------------------------
// A run is consecutive system rows that may collapse (pokes, kicks, bans, errors, welcome and host messages and
// TS Media's own lines never do). Identical consecutive rows show once with "xN"; a run of four or more
// (different) events shows its first row with "+N more events" ("Show fewer" once the reader opened it).
bool    collapsible(const Row& row);
QString eventText(const QTextBlock& block); // the event after "*** " (our trailer and chips left out)
struct RunPlan {
    QVector<int>            rows;   // block numbers, in order
    int                     tag = 0; // the first row's kBlockTag (the run's identity)
    QHash<int, bool>        shown;  // per row
    QHash<int, QStringList> chips;  // per row: its chips' texts
    QHash<int, bool>        toggle; // per row: its last chip opens / closes the run
};
// The runs touching blocks [from, to] (whole runs, blocks below floor left out). expanded: the first rows'
// tags of the runs the reader opened.
QVector<RunPlan> planRuns(QTextDocument* doc, int from, int to, const QSet<int>& expanded, int floor = 0);
// Gives the run its visibility (kCollapsed) and chips; true if anything changed. serial: for chip names.
bool applyRun(QTextDocument* doc, const RunPlan& run, const Env& env, int* serial);
// A chip's picture: a gap, then the chip. size: its logical size.
QImage chipPicture(const QString& text, bool hovered, const Env& env, QSize* size);
QSize  chipPictureSize(const QString& text, const Env& env);
// The chips of block (their positions and texts).
struct ChipRef {
    int     position = -1;
    QString text;
    QString name;
    bool    toggle = false;
};
QVector<ChipRef> chipsOf(const QTextBlock& block);

// ---- the reader's place (ChatLayout keeps it across its edits) ----------------------------------------------
// The block at document y (the top of the chat) and how far into it y is.
struct Anchor {
    int   block  = -1;
    qreal offset = 0;
    qreal height = 0;
};
Anchor anchorAt(QTextDocument* doc, qreal y);
// Where that place is now (document y; -1: unknown): as far into the block as before (relative to its new
// height when that changed). A block hidden since (folded into a run of events, taken by an album) gives
// the nearest shown block above it, where what it said went.
qreal anchorY(QTextDocument* doc, const Anchor& anchor);

// ---- lines that just arrived ---------------------------------------------------------------------------------
// The lines TeamSpeak appended in one turn of the event loop (in order; an append touches the line before it
// too, so a block may come twice): the block numbers of those that arrived now ("today", grouping without
// timestamps). More than three different lines at once are history being loaded (at the start, or the
// cleared chat refilled after a reconnect); a line timed more than three minutes away from nowSeconds (the
// time of day) is from TeamSpeak's log too.
QVector<int> arrivedNow(const QVector<QTextBlock>& lines, int nowSeconds);

// ---- whole documents (tests, render_gallery; ChatLayout works in steps with the same pieces) ---------------
// The accepted date of a day line: at most tomorrow, not before the accepted one above (invalid: shown raw).
QDate acceptDate(const QDate& date, const QDate& previous, const QDate& today);
// Per block: its day (a divider: its own accepted date; anything else: the nearest divider's above, a day
// line or a history marker with its date).
QVector<QDate> datesOf(QTextDocument* doc, const QDate& today);
// The same from block from on (what is before it is kept): lines TeamSpeak appended cost only themselves.
void extendDates(QTextDocument* doc, const QDate& today, QVector<QDate>* dates, int from);
// The previous visible block's row (blocks hidden by albums or collapsed runs are skipped).
Neighbour previousOf(const QTextBlock& block);
// Styles every block, top to bottom, in one edit; serials from firstSerial on. Returns how many.
int styleAll(QTextDocument* doc, const Env& env, int firstSerial = 1);
// Draws every picture of ours into the document's resources.
void renderAll(QTextDocument* doc, const Env& env, const QDate& today);
// The most common nickname colour of the document's messages (the skin's usual one).
QColor usualNickColor(QTextDocument* doc, int maxBlocks = 400);
// The most common colour of links in messages (the skin's link colour); invalid without links.
QColor usualLinkColor(QTextDocument* doc, int maxBlocks = 400);
// Whether the headers' times have AM/PM (the newest message with a time decides).
bool usesAmPm(QTextDocument* doc, int maxBlocks = 200);

} // namespace layoutdoc
