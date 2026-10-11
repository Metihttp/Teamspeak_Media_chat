#pragma once

// 2.3 live music: the slim bar above TeamSpeak's chat input while music plays to the channel. No window:
// it sits in TeamSpeak's layout at the top of the input's stack (inputbars.h), as wide as the input.
//
//   [music note] Title · Artist        [play/pause] 1:23 ━━━━●──── 3:45  [volume ━━●] [stop]
//   (! a problem line when the channel can't hear it: muted microphone, no talk power, ...)
//
// On another server tab (2.3: TeamSpeak's one chat input is shared by every tab) the bar is compact, so the
// music can always be played or stopped from where the user is:
//   [music note] Title · on Weekend Squad › Lobby                         [play/pause] [stop]
//   (i Paused while you're on another server tab. Play takes you back there.)
// (2.3 live test: TeamSpeak sends only from the tab in front, so the music waits meanwhile; the controller
// says so in the problem line, and Play's tool tip says it goes back to that tab.)
//
// The badge is a music note in the paperclip menu's hue (grey while the channel doesn't hear it, a ring
// while the file opens). Drag the seek slider (or click on it) to move forward or back; the arrow keys
// move 5 s, Page Up / Down 30 s, Home / End (the wheel doesn't seek). The volume slider (0..100) goes first when the bar gets narrow,
// then the total time, the seek slider and the elapsed time; the speaker button mutes (its wheel and
// arrow keys change the volume). Stop ends the music and the bar goes. A Notice is one line (an error,
// "Music stopped…") with a close button.
//
// Controls never take the focus on a click (the chat input keeps it) but are reachable with Tab, have
// visible focus rings, tool tips and accessible names. Painted with QPainter in the input's theme (light
// and dark); texts through i18n::t. objectName "tsmediaMusicBar": the stack and plugin shutdown find it.

#include <QColor>
#include <QFont>
#include <QRect>
#include <QString>
#include <QWidget>

namespace music {
class BarButton;
class BarSlider;
} // namespace music

class MusicBar : public QWidget
{
    Q_OBJECT

  public:
    enum class Mode { Loading, Playing, Paused, Notice };
    enum class Problem { None, Info, Warning };
    enum class NoticeIcon { Info, Error };

    struct View {
        Mode       mode = Mode::Loading;
        QString    title;  // shown as is (the controller sanitizes tags and names)
        QString    artist; // may be empty
        qint64     positionMs = 0;
        qint64     durationMs = 0; // 0: unknown (no seeking, no total)
        int        volume     = 50; // 0..100
        bool       muted      = false;
        bool       live       = true; // the channel hears it now (the badge is grey otherwise)
        QString    problem;           // a second line, empty: none
        Problem    problemKind = Problem::None;
        QString    notice;            // Notice: the line
        NoticeIcon noticeIcon  = NoticeIcon::Info;
        bool       animate     = true; // Windows animations (nothing in the bar moves by itself any more)
        // 2.3: the music plays on another server tab than the one on screen: where ("Weekend Squad › Lobby",
        // sanitized by the controller). The bar is then compact: the note, the title, where, play/pause, stop,
        // and the problem line if there is one (2.3 live test: the music waits for its tab).
        QString    away;
    };

    // Where each part sits in a bar of a size (tests and the layout).
    struct Geometry {
        QRect badge, title, play, elapsed, seek, total, volumeButton, volumeSlider, stop, close, problem;
        bool  showElapsed = true, showTotal = true, showSeek = true, showVolumeSlider = true, showVolumeButton = true;
    };

    explicit MusicBar(QWidget* parent = nullptr);
    ~MusicBar() override;

    void        setView(const View& view);
    const View& view() const { return m_view; }
    // TeamSpeak's theme: the chat input's background and font.
    void setTheme(bool dark, const QColor& base, const QFont& font);
    bool dark() const { return m_dark; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool  hasHeightForWidth() const override; // a notice wraps (up to three lines)
    int   heightForWidth(int width) const override;
    static int      heightFor(const QFont& font, bool withProblem);
    static Geometry geometryFor(const QSize& size, const QFont& font, const View& view);
    const Geometry& parts() const { return m_geometry; } // (not geometry(): that is QWidget's)
    bool            seeking() const; // the seek slider is held
    qint64          shownPositionMs() const; // the time shown (the slider's while it is dragged)

    // The controls (tests, the harness).
    QWidget* playButton() const;
    QWidget* stopButton() const;
    QWidget* closeButton() const;
    QWidget* seekSlider() const;
    QWidget* volumeSlider() const;
    QWidget* volumeButton() const;

    // The attach menu's entry: a music note in its own hue (menuHue: 3:1 on light and dark menus) on a
    // transparent square, 24 logical px (attachmenu.h), as plain pixmaps.
    static QIcon  menuIcon();
    static QColor menuHue();

    // Colours of the bar in a theme (the controls use them too).
    struct Colors {
        QColor surface, line, text, muted, accent, onAccent, hover, pressed, quiet, error, focus;
        QColor note;     // 2.3: the badge's music note (menuHue, 3:1 on its tile)
        QColor noteTile; // 2.3: the badge's tile behind it (the note's hue, soft)
    };
    static Colors colorsFor(bool dark, const QColor& base);
    const Colors& colors() const { return m_colors; }

  signals:
    void playPauseClicked();
    void stopClicked();
    void seekRequested(qint64 ms);
    void volumeChanged(int percent);
    void muteToggled();
    void noticeClosed();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override; // tool tips over the title and the problem line
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void relayout();
    void applyTexts();
    QString titleTip() const;

    View              m_view;
    Geometry          m_geometry;
    bool              m_dark = false;
    QColor            m_base;
    Colors            m_colors;
    music::BarButton* m_play         = nullptr;
    music::BarButton* m_stop         = nullptr;
    music::BarButton* m_close        = nullptr;
    music::BarButton* m_volumeButton = nullptr;
    music::BarSlider* m_seek         = nullptr;
    music::BarSlider* m_volume       = nullptr;
    QString           m_announced;
};
