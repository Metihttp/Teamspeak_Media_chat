#pragma once

// 2.7 F3: Settings → Chat → "Text size and colors" (design 4.3, 4.4; design_settings 4.6):
//   Chat te&xt size   [-] 112% [+]  Re&set   acts at once, like Ctrl + mouse wheel in the chat (one zoom for every chat,
//                                            Settings::chatZoom in tenths of a point); not a form value: OK, Cancel,
//                                            Apply and Restore defaults never touch it
//   &Color of messages from TS Media          Settings::chatLineColor (a preset name, "" = Automatic): a form value
//   a preview of two TS Media lines on the chat's background, in the colours the chat will really print
// The section knows nothing of the chat: plugin.cpp fills the hooks (the Join connects them to ChatIntegration). Without
// hooks the zoom buttons are disabled, the size reads 100%, and the preview shows the Automatic colours on TeamSpeak's
// chat background (white, or the dark skin's #2f3136 in a dark dialog).

#include <functional>

#include "settingssection.h"

class IconButton;
class LinePreview;
class QComboBox;
class QLabel;
class QPushButton;

struct LinePreviewColors {
    QColor base, text, accent, warning;
};

struct ChatTextHooks {
    std::function<int()>                                     zoomTenths;    // Settings::chatZoom as the chat has it now
    std::function<int()>                                     zoomPercent;   // the value shown ("112%")
    std::function<void(int)>                                 setZoomTenths; // acts at once (the chat clamps, saves, notifies)
    std::function<LinePreviewColors(const QString& choice)> linePreview;   // what the chat will really print
};

class ChatTextSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit ChatTextSection(QWidget* parent = nullptr, ChatTextHooks hooks = ChatTextHooks());

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

    // The choice shown: "" (Automatic) or one of Settings::lineColorChoices.
    QString choice() const;

    // Tests and the live-test driver.
    IconButton*  smallerButton() const { return m_smaller; }
    IconButton*  largerButton() const { return m_larger; }
    QPushButton* resetButton() const { return m_reset; }
    QLabel*      sizeLabel() const { return m_size; }
    QComboBox*   colorBox() const { return m_color; }
    LinePreview* preview() const { return m_preview; }

    QWidget*     zoomRow() const { return m_zoomRow; } // the "Chat te&xt size" label's buddy (focus only, never a click)

  public slots:
    void refreshZoom(); // re-reads zoomTenths / zoomPercent (the chat's zoom signal, window activation, the page shown)
    // 2.7 dark mode (review fix): the swatches and the line preview read the visible chat's colours again (dark mode
    // turned on or off, the chat's colours settled after it).
    void refreshColors();

  protected:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void stepZoom(int direction); // -1, +1
    void refreshPreview();
    void refreshSwatches();

    ChatTextHooks m_hooks;
    QWidget*      m_zoomRow = nullptr;
    IconButton*   m_smaller = nullptr;
    IconButton*   m_larger  = nullptr;
    QPushButton*  m_reset   = nullptr;
    QLabel*       m_size    = nullptr;
    QComboBox*    m_color   = nullptr;
    LinePreview*  m_preview = nullptr;
    bool          m_loading = false;
    bool          m_swatchesDark = false;
    qreal         m_swatchesDpr  = 0;
};
