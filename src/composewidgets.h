#pragma once

// 2.3 compose-v2: the send window's own controls, painted by the plugin so they look the same in every
// TeamSpeak skin (TeamSpeak's dark skin styles every QPushButton and QCheckBox alike): the primary,
// secondary and ghost buttons, the compact chips (Edit, Revert, Quality), the icon buttons on a picture
// (spoiler, edit, remove), the "Send as an album" switch, the vector glyphs and the colours, all taken
// from TeamSpeak's palette (light or dark skin). Hover and state changes fade (ui::animationsEnabled()).
// Every control draws its own focus ring after keyboard focus (the dark skins turn Qt's off).
//
// GUI thread only. The animations are owned by the controls (no timers outlive them).

#include <QAbstractButton>
#include <QColor>
#include <QPushButton>
#include <QString>

#include <functional>

class QPainter;
class QVariantAnimation;

namespace composeui {

// The window's colours, from TeamSpeak's palette (Window, WindowText). Text colours keep 4.5:1 on body
// (muted, error), the accent and the focus ring 3:1.
struct Theme {
    bool   dark = false;
    QColor window;         // TeamSpeak's window colour
    QColor body;           // the window's background
    QColor footer;         // the band with Add files, Send and Cancel
    QColor surface;        // the large preview's stage, a tile's placeholder, the file card
    QColor field;          // the caption field
    QColor fieldBorder;
    QColor border;         // hairlines
    QColor text;
    QColor muted;          // secondary text
    QColor accent;         // focus rings, checked states, links (3:1 on body)
    QColor primary;        // Send
    QColor primaryHover;
    QColor primaryPressed;
    QColor onPrimary;      // text on primary (4.5:1)
    QColor error;
    QColor errorTint;      // a callout's background
    QColor chip;           // chips and the secondary button
    QColor chipHover;
    QColor chipPressed;
    QColor scrim;          // behind white glyphs and badges on a picture
};

Theme  themeFor(const QPalette& palette);
QColor withAlpha(QColor color, qreal alpha);
QColor mixColors(const QColor& from, const QColor& to, qreal amount); // amount 0: from, 1: to

enum class Glyph {
    None,
    Spoiler,  // an eye with a slash
    Remove,   // x
    Edit,     // a pencil
    Revert,   // an arrow turning back
    Plus,
    Upload,   // an arrow up out of a tray
    Channel,  // #
    Person,
    Server,   // a globe
    Play,     // a filled triangle
    Quality,  // sliders
    Chevron,  // down
    Album,    // a 2 x 2 grid
    Check,
};

// Stroked like the plugin's other glyphs; box is the glyph's square.
void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color);
// A filled disc with "!" (never colour alone: the mark says it).
void drawAlert(QPainter& p, const QRectF& box, const QColor& fill, const QColor& mark);
// A page with a folded corner and the file's extension on a coloured band (files without a picture).
void drawFileArt(QPainter& p, const QRectF& box, const QString& extension, const QColor& tint, const Theme& theme);
// The colour of a file type's band (archives amber, audio pink, documents blue, ...).
QColor fileTint(const QString& extension, bool dark);
// 2 px ring just inside shape (rounded by radius).
void drawFocusRing(QPainter& p, const QRectF& shape, qreal radius, const QColor& color);

// Animates a value towards 0 or 1 and repaints its widget; instant when Windows' animations are off
// or the widget isn't visible.
class Fader : public QObject
{
  public:
    explicit Fader(QWidget* target, int durationMs = 140);
    qreal value() const { return m_value; }
    qreal target() const { return m_goal; }
    void  setTarget(qreal goal);
    void  jumpTo(qreal value);

  private:
    QWidget*           m_target;
    QVariantAnimation* m_anim;
    qreal              m_value = 0.0;
    qreal              m_goal  = 0.0;
};

// Records whether focus arrived by keyboard (Tab, a shortcut; or moved by the window after the keyboard
// was used to move around it) rather than a click or the window opening: only then the ring, as Windows
// does.
class FocusTracker
{
  public:
    void        onFocusIn(QFocusEvent* event, const QWidget* widget);
    static bool byKeyboard(QFocusEvent* event, const QWidget* widget);
    bool        keyboard() const { return m_keyboard; }

  private:
    bool m_keyboard = false;
};

// Send (Primary), Cancel (Secondary) and Add files (Ghost, with a glyph). QPushButtons, so the dialog's
// Enter key finds the default one and QDialogButtonBox lays them out.
class Button : public QPushButton
{
  public:
    enum class Kind { Primary, Secondary, Ghost };
    Button(Kind kind, const QString& text, QWidget* parent);

    void  setGlyph(Glyph glyph);
    void  setTheme(const Theme& theme);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    QFont paintFont() const;

    Kind         m_kind;
    Glyph        m_glyph = Glyph::None;
    Theme        m_theme;
    Fader*       m_hover;
    FocusTracker m_focus;
};

// A compact pill: a glyph, a short text and, for a menu, a chevron. Enter and Space click it; with
// stepping on, Up and Down ask for the previous or next entry (stepped(-1 / +1)).
class Chip : public QAbstractButton
{
  public:
    Chip(Glyph glyph, const QString& text, QWidget* parent);

    void  setTheme(const Theme& theme);
    void  setMenuIndicator(bool on);
    void  setStepHandler(std::function<void(int)> step) { m_step = std::move(step); }
    void  setMaximumTextWidth(int width);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    Glyph                    m_glyph;
    bool                     m_menu = false;
    int                      m_maxText = 0;
    Theme                    m_theme;
    Fader*                   m_hover;
    FocusTracker             m_focus;
    std::function<void(int)> m_step;
};

// A square glyph button. Plain: on the window (the file card's remove). OnMedia: a white glyph on a
// dark pill over a picture; its opacity follows reveal (the tile's hover / focus). Checkable for the
// spoiler toggle (checked: accent).
class IconButton : public QAbstractButton
{
  public:
    enum class Look { Plain, OnMedia };
    IconButton(Glyph glyph, Look look, int side, QWidget* parent);

    void  setTheme(const Theme& theme);
    void  setReveal(const Fader* reveal) { m_reveal = reveal; }
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    Glyph        m_glyph;
    Look         m_look;
    int          m_side;
    Theme        m_theme;
    const Fader* m_reveal = nullptr;
    Fader*       m_hover;
    FocusTracker m_focus;
};

// A settings-style row: a title, a muted subtitle under it, and a switch at the right. The whole row
// toggles (click, Space); Enter is left to the dialog (Send), like a check box.
class SwitchRow : public QAbstractButton
{
  public:
    SwitchRow(Glyph glyph, const QString& title, QWidget* parent);

    void    setTheme(const Theme& theme);
    void    setSubtitle(const QString& subtitle);
    QString subtitle() const { return m_subtitle; }
    QSize   sizeHint() const override;
    QSize   minimumSizeHint() const override;
    bool    hasHeightForWidth() const override { return true; }
    int     heightForWidth(int width) const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

  private:
    QRectF switchRect() const;
    int    textWidth(int width) const;

    Glyph        m_glyph;
    QString      m_subtitle;
    Theme        m_theme;
    Fader*       m_hover;
    Fader*       m_knob;
    FocusTracker m_focus;
};

// Windows 10 / 11: the title bar in the window's colours (dark in dark skins; Windows 11 also takes the
// exact background colour). Does nothing elsewhere or when Windows can't.
void applyWindowCaption(QWidget* window, const Theme& theme);

} // namespace composeui
