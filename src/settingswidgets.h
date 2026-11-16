#pragma once

// 2.7 F3: the painted parts of the new Settings window (design_settings 4, design H1/M9). Everything here is QPainter vector
// work in logical pixels with antialiasing (sharp at any scale), sized with the dialog's font-based factor k
// (settingstheme::Tokens::px), coloured from the dialog's tokens (settingstheme::tokensFor). No bitmaps from files, no
// QIconEngine, no metatypes; style sheets and property names are built at run time (the CRASH RULE).
//   SwitchBox    a QCheckBox painted as a switch on the right of its row (it IS a QCheckBox: findChildren, click(),
//                accessibility and the live-test driver keep working)
//   LayoutTile   a QRadioButton painted as a picture tile (the chat layouts); TileLayout puts 3 in a row, or 1 per row
//                when 3 don't fit at their minimum width
//   IconButton   a square push button painting a minus or plus (the chat text size)
//   LinePreview  two "TS Media chat" lines on the chat's background (the line colour choice)
//   SettingRow   one row of a card: title and helper text on the left, the control on the right; a field row turns
//                stacked (title, hint, control) when its control is wider than 40 % of the row (M9)
//   NavDelegate  the sidebar's items (icon + name, the current page tinted with an accent bar)

#include <QCheckBox>
#include <QLayout>
#include <QPushButton>
#include <QRadioButton>
#include <QStyledItemDelegate>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QLabel;
class QPainter;

namespace settingstheme {
struct Tokens;
}

namespace settingsicons {
enum class Icon { Chat, Emoji, Send, Receive, Link, Shield, Server, Info, Minus, Plus };
// One stroke icon (24-unit grid, 1.9 stroke, round caps) in r.
void draw(QPainter& p, Icon icon, const QRectF& r, const QColor& color);
} // namespace settingsicons

namespace settingswidgets {

// The font painted widgets use: their window's (a skin may give radio buttons and combos a smaller font).
QFont windowFont(const QWidget* w);
// The 2 px focus ring.
void focusRing(QPainter& p, const QRectF& r, qreal radius, const settingstheme::Tokens& t);
// Whether the style shows access-key underlines now (Windows: while Alt is held).
bool showMnemonics(const QWidget* w);

} // namespace settingswidgets

class SwitchBox : public QCheckBox
{
    Q_OBJECT

  public:
    explicit SwitchBox(const QString& text, QWidget* parent = nullptr);

    // The switch's track for a font and size factor: h = max(18 k, 1.25 em), w = 2 h.
    static QSize trackSize(const QFont& font, qreal k);
    // Keeps px free between the title and the switch for a trailing control (the row places it there).
    void setTrailingReserve(int px);
    int  trailingReserve() const { return m_reserve; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool  hasHeightForWidth() const override { return true; }
    int   heightForWidth(int width) const override;

  protected:
    bool hitButton(const QPoint& pos) const override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    int textWidth(int width) const;

    int m_reserve = 0;
};

class LayoutTile : public QRadioButton
{
    Q_OBJECT

  public:
    enum class Kind { Cozy, Compact, Classic };
    LayoutTile(Kind kind, const QString& text, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    bool hitButton(const QPoint& pos) const override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    Kind m_kind;
};

// The layout tiles: three in a row while each gets its minimum width, else one per row (height for width, no timer).
class TileLayout : public QLayout
{
  public:
    explicit TileLayout(QWidget* parent = nullptr);
    ~TileLayout() override;

    void           addItem(QLayoutItem* item) override;
    int            count() const override { return m_items.size(); }
    QLayoutItem*   itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem*   takeAt(int index) override;
    QSize          sizeHint() const override;
    QSize          minimumSize() const override;
    bool           hasHeightForWidth() const override { return true; }
    int            heightForWidth(int width) const override;
    void           setGeometry(const QRect& rect) override;
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

    int perRow(int width) const; // 3 or 1 (tests)

  private:
    int gap() const;
    int tileHeight() const;

    QVector<QLayoutItem*> m_items;
};

class IconButton : public QPushButton
{
    Q_OBJECT

  public:
    IconButton(settingsicons::Icon icon, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    settingsicons::Icon m_icon;
};

class LinePreview : public QWidget
{
    Q_OBJECT

  public:
    explicit LinePreview(QWidget* parent = nullptr);

    // The chat's background and text, and the two colours of TS Media's lines on it.
    void   setColors(const QColor& base, const QColor& text, const QColor& accent, const QColor& warning);
    QColor base() const { return m_base; }
    QColor accent() const { return m_accent; }
    QColor warning() const { return m_warning; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QColor m_base, m_text, m_accent, m_warning;
};

class SettingRow : public QWidget
{
    Q_OBJECT

  public:
    enum class Kind { Switch, Field, Stacked, Custom };
    SettingRow(Kind kind, int indent, QWidget* parent);

    // title: the SwitchBox (Switch) or the QLabel (Field, Stacked); hint may be null; control: the field (Field,
    // Stacked), the trailing control (Switch) or the content (Custom). extra: a widget between the field and the hint
    // (Stacked: the link's error line).
    void setParts(QWidget* title, QLabel* hint, QWidget* control, QWidget* extra = nullptr);

    Kind kind() const { return m_kind; }
    int  indent() const { return m_indent; }
    bool stacked() const { return m_stacked; }        // a field row shown title, hint, control (M9)
    void setAlwaysStacked(bool on);                   // the zoom and colour rows
    bool isFirstInCard() const;                       // no divider above it

  protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void applyMetrics();
    void arrange();
    bool wantsStacked(int width) const;

    Kind         m_kind;
    int          m_indent;
    QGridLayout* m_grid    = nullptr;
    QWidget*     m_title   = nullptr;
    QLabel*      m_hint    = nullptr;
    QWidget*     m_control = nullptr;
    QWidget*     m_extra   = nullptr;
    bool         m_stacked = false;
    bool         m_always  = false;
    bool         m_arranged = false;
};

class NavDelegate : public QStyledItemDelegate
{
    Q_OBJECT

  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void  paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
