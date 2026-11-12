#pragma once

// 2.5 link previews: what a card shows (a rich site card or a generic web card). Plain data, shared by the
// Fetcher's Result, the LPX announcement, the chat's state and the card painter (linkcard). Every text in it is
// plain text cleaned by textclean (never rich text, never HTML).

#include <QRgb>
#include <QString>

namespace linkpreview {

enum class CardLayout { None, Large, Thumb }; // None: no picture

struct CardData {
    QString    site;        // shown above the title; Site class: the provider's brand (never from a page)
    QString    author;      // oEmbed author_name / provider line ("r/gifs · u/name", "12,345 members · 678 online")
    QString    title;       // required for a card
    QString    description;
    QString    pictureFile; // local file (the fetch, or Core's download of the sender's preview); empty: none
    int        pictureWidth = 0, pictureHeight = 0; // known before the picture is (LPX w/ht), for a stable size
    CardLayout layout       = CardLayout::None;
    QRgb       color        = 0;
    bool       hasColor     = false; // Web: the page's theme colour
    bool       play         = false; // a play glyph on the picture (a video page)
    qint64     durationMs   = 0;     // with play
    bool       sensitive    = false;

    bool isEmpty() const { return title.isEmpty(); }
};

// Shown text limits in characters (after cleaning) and on the wire in UTF-8 bytes (LPX).
constexpr int kCardSiteChars        = 64;
constexpr int kCardAuthorChars      = 96;
constexpr int kCardTitleChars       = 200;
constexpr int kCardDescriptionChars = 350;
constexpr int kCardSiteBytes        = 64;
constexpr int kCardAuthorBytes      = 96;
constexpr int kCardTitleBytes       = 256;
constexpr int kCardDescriptionBytes = 384;

inline const char* layoutName(CardLayout layout)
{
    switch (layout) {
    case CardLayout::Large:
        return "large";
    case CardLayout::Thumb:
        return "thumb";
    case CardLayout::None:
        break;
    }
    return "none";
}

} // namespace linkpreview
