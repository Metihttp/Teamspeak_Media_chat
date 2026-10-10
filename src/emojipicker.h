#pragma once

// 2.2 emoji: the emoji picker (Discord-like). A popup with a search field, a skin-tone selector,
// category tabs with HD emoji icons, a grid of 32 px HD emoji in sections ("Recently used" first), and a
// footer with the hovered emoji's picture and name. Pictures come from the colour renderer's worker
// (emojirender.h) and fill in as they are drawn; placeholders keep the grid still meanwhile.
//
// Keyboard: typing searches (the field has the focus), Down or Tab moves into the grid, the arrows move
// there, Page Up / Page Down jump a section, Enter picks (Shift+Enter, Shift+click: pick and stay open),
// Esc clears the search or closes. A top-level popup named "tsmedia..." so plugin shutdown deletes it.

#include <QColor>
#include <QSet>
#include <QWidget>

class EmojiPickerPrivate;

class EmojiPicker : public QWidget
{
    Q_OBJECT

  public:
    enum class Mode {
        Insert, // into a text field
        React,  // as a reaction: your current ones are marked
    };

    // dark / base: the theme of what it opens from (base: its background colour, may be invalid).
    EmojiPicker(Mode mode, bool dark, const QColor& base, QWidget* parent = nullptr);
    ~EmojiPicker() override;

    void setMarked(const QSet<int>& ids); // React: your reactions on the media

    // Opens next to anchor (global coordinates): above it, right-aligned (where chat inputs are), or below
    // when there is no room; below first when preferBelow.
    void openAt(const QRect& anchor, bool preferBelow = false);
    // The button that opened it (global coordinates): a press there while it is open closes it, and the
    // press isn't handed on to the button (which would open it again at once).
    void setOpener(const QRect& opener);

    static QSize preferredSize();

    // Tools and tests: the state to draw (search text, the item with the keyboard focus).
    void setSearchText(const QString& text);
    void focusItem(int index);
    void scrollToGroup(int section);

  signals:
    void picked(int id, bool keepOpen); // the id with the chosen skin tone applied
    void closed();

  protected:
    bool event(QEvent* event) override; // tool tips of the tabs
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override; // a press on the opener closes it (no reopening)
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    friend class EmojiPickerPrivate;
    EmojiPickerPrivate* d;
};
