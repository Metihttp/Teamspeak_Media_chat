#pragma once

// 2.7 F6: the chat's effective colours, the one source every chat module and TS Media's own chat lines use.
//  * The background is what the chat really shows: the ChatTab's palette Base brush (QStyleSheetStyle copies the
//    `QTextEdit#ChatTab` rule's background into it): a solid colour, the average of a gradient's stops, or, for a
//    picture the style sheet paints (it never reaches the palette), the dominant colour of the ChatTab's style-sheet
//    background drawn into a small picture (sampleBackground, owned timer, never in a paint). Dark or light follows
//    that colour (isDark), never the skin's name or one known colour.
//  * TS Media's own lines (ts3::printInfo, printWarning, printInfoLink, the body of every printed line): a preset tone
//    (Settings::chatLineColor, "" = Automatic), made 4.5:1 on that background with the hue kept. Automatic on
//    TeamSpeak's white chat and on its dark skin (#2f3136) gives exactly the 2.6 colours.
// The pure parts need QtGui only (the tests, the render tools); sampleBackground is QtWidgets.

#include <QColor>
#include <QString>

class QGradient;
class QImage;
class QPalette;
class QWidget;

namespace chatcolors {

struct Colors {
    QColor base;          // the effective background (opaque)
    QColor text;          // palette Text (the skin's `color`)
    QColor link;          // palette Link
    bool   dark    = false;
    bool   sampled = false; // base comes from painted pixels (a picture background)
    // 2.7 dark mode: the theme's accent while TS Media's dark mode is on (darkmode.h), else invalid. Automatic then gives
    // TS Media's lines this accent (made 4.5:1 on base), and the chat layout's accents follow it.
    QColor themeAccent;
};

// Names "#rrggbb", built at run time (QColor::name()): they go into BBCode lines TeamSpeak keeps.
struct LineColors {
    QString accent;          // the "TS Media chat" prefix and the links of our lines
    QString warning;         // the "TS Media chat ⚠" prefix
    QString body;            // the line's own text when wrapBody
    bool    wrapBody = false; // TeamSpeak's plugin-line blue (#00008b) reads under 4.5:1 on the background
};

// TeamSpeak's light chat (#ffffff, black text, link #1c82cc): what is used while no chat is known.
Colors teamSpeakLight();
// The colours of a ChatTab's palette (ts_facts 5.3 steps 1-3, 5): solid Base -> it; a gradient -> averageOfStops; anything
// else -> palette Window until sampled. Pure.
Colors fromPalette(const QPalette& chatTabPalette);
// A sampled background over the palette's answer: taken when the palette's Base is a texture (texture: no colour of its
// own) or when the painted pixels disagree with it about dark and light (a picture only the style sheet paints). Pure.
Colors withSample(const Colors& fromPalette, const QColor& sampled, bool texture = false);
// The palette's Base brush is neither a plain colour nor a gradient (fromPalette used the Window colour for it).
bool   isTexture(const QPalette& chatTabPalette);
QColor averageOfStops(const QGradient& gradient);
// The most common colour of image (4 bits per channel, every other pixel), averaged over its bucket. Invalid: empty.
QColor dominant(const QImage& image);
// Light text reads better on it than dark text (WCAG contrast with white >= with black).
bool   isDark(const QColor& base);
// colour made minimum:1 on base (ui::readableOn, the hue kept; the other direction or black / white when that can't).
QColor readable(const QColor& colour, const QColor& base, double minimum = 4.5);

// Settings::chatLineColor's presets (Settings::lineColorChoices): the tone on a light / a dark background.
// "" (Automatic) and unknown names: #4752c4 / #949cf7. "text": invalid (the chat's text colour is used).
QColor presetTone(const QString& choice, bool dark);
// The colours of TS Media's lines on chat for choice (Settings::chatLineColor). Pure.
LineColors lineColors(const Colors& chat, const QString& choice);

// One chat's colours between samples (ChatIntegration keeps one per chat; pure, review fix). A new palette or style is
// read at once (read()), but a sample taken before stays in use over it until the new sample arrives, so a picture
// background never flips to the palette's light or dark guess in between; and a new sample counts as a change only when
// the effective colours differ from the ones handed out (takeSample), so the chat modules restyle once per real change.
// A font change can't change the background and never invalidates.
struct Cache {
    bool   valid      = false; // colors is the answer for the palette read last
    bool   sampleDone = false; // the sample belongs to the palette and style read last
    bool   haveSample = false; // a sample was taken (kept over invalidate())
    bool   texture    = false; // the palette's Base is a texture (fromPalette used Window)
    Colors fromPalette;
    QColor sampled;
    Colors colors; // the answer: the palette's, with the latest sample over it

    void read(const QPalette& chatTabPalette); // valid again; sampleDone unchanged
    void invalidate();                         // a new palette or style: read again, sample again
    bool takeSample(const QColor& sample);     // true: the effective colours changed (dark or base)
};

// The ChatTab's own style-sheet background drawn into a 24 x 24 picture (QStyle::PE_Widget with the widget's style:
// only the background, no paint event, no text) and its dominant colour. Invalid when nothing opaque was drawn.
// Never call it inside a paint.
QColor sampleBackground(QWidget* chatTab);

} // namespace chatcolors
