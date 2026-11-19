#include "darkpalette.h"

#include <QFont>
#include <QFontInfo>
#include <QRegularExpression>

// 2.7 Dark mode (design_dark 2). Every string that reaches Qt is built here at run time (CRASH RULE, see darkpalette.h).

namespace darkpalette {

namespace {

// The application style sheet (design_dark 2.6, the prototype's dark.qss.in): @token@ a colour, {N} N scaled pixels,
// @g:name@ a glyph picture, @m:name@ the skin's chat metric. The slider handle's hover colour is a token here.
const char kAppSheet[] =
R"qss(/* TS Media chat: Dark mode (application style sheet, built at run time) */

/* ---- windows and chrome ---- */
QMainWindow { background: @panel@; }
QMainWindow::separator { background: @panel@; width: {6}px; height: {6}px; }
QDialog, QWizard, QMessageBox { background: @window@; }
QDockWidget { color: @text@; }
QDockWidget::title { background: @panel@; padding: {4}px {8}px; }
QWidget#MainWindowChatWidget, QWidget#MainWindowServerTabsWidget { background: @panel@; }
QWidget#HeadlineLabel { background: @panel@; border: none; border-bottom: 1px solid @border@; }
QLabel#OptionsHeadlineTitleLabel { color: @text@; }
QLabel#OptionsHeadlineDescriptionLabel { color: @secondary@; }
QLabel#appNameLabel { color: @text@; }
QLabel#bookmarksBanner, QLabel#identitiesBanner, QLabel#accountLoginBanner, QLabel#accountCreateBanner {
    background: @raised@; color: @text@; padding-left: {8}px; border-bottom: 1px solid @border@; qproperty-minimumSize: 0px 32px; }
QLabel#accountLoginBanner, QLabel#accountCreateBanner { qproperty-minimumSize: 420px 32px; }
QStatusBar { background: @panel@; color: @secondary@; border-top: 1px solid @border@; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @secondary@; padding: 0px {3}px; border: 0px; }
QSplitter::handle { background: @panel@; }
QSplitter::handle:horizontal { width: {5}px; }
QSplitter::handle:vertical { height: {5}px; }
QSplitter::handle:hover { background: @border@; }

/* ---- menu bar and menus ---- */
QMenuBar { background: @panel@; color: @text@; border: none; padding: {2}px {4}px; }
QMenuBar::item { background: transparent; color: @text@; padding: {4}px {10}px; border-radius: {6}px; }
QMenuBar::item:selected { background: @hover@; }
QMenuBar::item:pressed { background: @menuHover@; }
QMenu { background: @raised@; color: @text@; border: 1px solid @border@; padding: {4}px; }
QMenu::item { background: transparent; padding: {5}px {28}px {5}px {12}px; border-radius: {6}px; }
QMenu::item:selected { background: @menuHover@; color: @text@; }
QMenu::item:disabled { color: @disabled@; background: transparent; }
QMenu::separator { height: 1px; background: @border@; margin: {4}px {8}px; }
QMenu::icon { padding-left: {8}px; }
QMenu::indicator { width: {14}px; height: {14}px; padding-left: {6}px; }
QMenu::indicator:non-exclusive:checked { image: url("@g:mcheck@"); }
QMenu::indicator:exclusive:checked { image: url("@g:mdot@"); }
QMenu::right-arrow { image: url("@g:chev-right@"); width: {10}px; height: {10}px; padding-right: {8}px; }
QToolTip { background: @raised@; color: @text@; border: 1px solid @border@; padding: {4}px {6}px; }

/* ---- tool bars and tool buttons ---- */
QToolBar { background: @panel@; border: none; spacing: {2}px; padding: {4}px {6}px; }
QToolBar::separator { background: @border@; width: 1px; height: 1px; margin: {6}px {5}px; }
QToolBar::handle { width: 0px; height: 0px; image: none; }
QToolButton { background: transparent; color: @text@; border: 1px solid transparent; border-radius: {6}px; padding: {3}px; }
QToolButton:hover { background: @hover@; }
QToolButton:pressed, QToolButton:checked { background: @menuHover@; }
QToolButton:checked:hover { background: @hoverStrong@; }
QToolButton:focus { border-color: @accent@; }
QToolButton:disabled { color: @disabled@; }
QToolButton[popupMode="1"] { padding-right: {16}px; }
QToolButton::menu-button { border: none; width: {14}px; }
QToolButton::menu-indicator, QToolButton::menu-arrow { image: url("@g:chev-down-sm@"); width: {8}px; height: {8}px; }
QToolButton::menu-indicator { subcontrol-origin: padding; subcontrol-position: bottom right; }

/* ---- tab bars ---- */
QTabWidget::pane { background: @window@; border: none; border-top: 1px solid @border@; top: -1px; }
QTabWidget::tab-bar { left: {6}px; }
QTabBar { background: transparent; }
QTabBar::tab { background: transparent; color: @secondary@; border: none; border-bottom: {2}px solid transparent;
    padding: {6}px {12}px; margin-right: {2}px; }
QTabBar::tab:hover { color: @text@; background: @hover@; border-top-left-radius: {6}px; border-top-right-radius: {6}px; }
QTabBar::tab:selected { color: @text@; border-bottom-color: @accent@; }
QTabBar::tab:disabled { color: @disabled@; }
QTabBar::tab:bottom { border-bottom: none; border-top: {2}px solid transparent; }
QTabBar::tab:bottom:selected { border-top-color: @accent@; }
QTabBar::close-button { image: url("@g:close@"); subcontrol-position: right; border-radius: {4}px; padding: {1}px; }
QTabBar::close-button:hover { background: @hoverStrong@; }
QTabBar::scroller { width: {22}px; }
QTabBar QToolButton { background: @panel@; border: none; }
QTabBar QToolButton::left-arrow { image: url("@g:chev-left@"); }
QTabBar QToolButton::right-arrow { image: url("@g:chev-right@"); }
ImprovedTabBarCloseButton { background: transparent; border: none; border-radius: {4}px; }
ImprovedTabBarCloseButton:hover { background: @hoverStrong@; }

/* ---- text views: the chat, the info panel, the chat input ---- */
QTextEdit, QPlainTextEdit { background: @window@; color: @text@; border: 1px solid @control@;
    selection-background-color: @selection@; selection-color: @text@; }
QTextEdit:hover, QPlainTextEdit:hover { border-color: @controlHover@; }
QTextEdit:focus, QPlainTextEdit:focus { border-color: @accent@; }
QTextEdit[readOnly="true"], QPlainTextEdit[readOnly="true"] { border-color: @border@; }
QTextEdit:disabled, QPlainTextEdit:disabled { background: @disabledFill@; color: @disabled@; border-color: @border@; }
QTextEdit#ChatTab { background: @window@; color: @text@; border: @m:chatFrame@px solid @window@; }
QTextEdit#InfoFrame { background: @window@; color: @text@; border: none; }
QTextEdit#ChatLineEdit { background: @raised@; color: @text@; border: 1px solid @control@; border-radius: {8}px; padding: 0px {6}px; }
QTextEdit#ChatLineEdit:focus { border-color: @accent@; }
QTextEdit#ChatLineEdit[helpTextVisible="true"] { color: @muted@; }
QTextEdit#ChatLineEdit[helpTextVisible="false"] { color: @text@; }

/* ---- fields ---- */
QLineEdit, QAbstractSpinBox, QComboBox { background: @field@; color: @text@; border: 1px solid @control@; border-radius: {6}px;
    padding: {3}px {7}px; selection-background-color: @selection@; selection-color: @text@; }
QLineEdit:hover, QAbstractSpinBox:hover, QComboBox:hover { border-color: @controlHover@; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QComboBox:on { border-color: @accent@; }
QLineEdit:read-only { background: @window@; }
QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled { background: @disabledFill@; color: @disabled@; border-color: @border@; }
QComboBox { padding-right: {26}px; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: {22}px; border: none; background: transparent; }
QComboBox::down-arrow { image: url("@g:chev-down@"); width: {10}px; height: {10}px; }
QComboBox::down-arrow:disabled { image: url("@g:chev-down-dis@"); }
QComboBox QAbstractItemView { background: @raised@; color: @text@; border: 1px solid @border@; padding: {4}px; outline: none;
    selection-background-color: @menuHover@; selection-color: @text@; }
QComboBox QAbstractItemView::item { min-height: {24}px; padding: 0px {6}px; border-radius: {4}px; }
QComboBox QAbstractItemView::item:selected, QComboBox QAbstractItemView::item:hover { background: @menuHover@; color: @text@; }
QAbstractSpinBox { padding-right: {22}px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin: border; width: {20}px; border: none;
    border-left: 1px solid @border@; background: transparent; }
QAbstractSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: {6}px; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: {6}px; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: @hover@; }
QAbstractSpinBox::up-arrow { image: url("@g:chev-up-sm@"); width: {8}px; height: {8}px; }
QAbstractSpinBox::down-arrow { image: url("@g:chev-down-sm@"); width: {8}px; height: {8}px; }
QAbstractSpinBox::up-arrow:disabled, QAbstractSpinBox::up-arrow:off { image: url("@g:chev-up-sm-dis@"); }
QAbstractSpinBox::down-arrow:disabled, QAbstractSpinBox::down-arrow:off { image: url("@g:chev-down-sm-dis@"); }

/* ---- buttons ---- */
QPushButton { background: @raised@; color: @text@; border: 1px solid @control@; border-radius: {6}px; padding: {4}px {14}px; min-width: {52}px; outline: none; }
QPushButton:hover { background: @raisedHover@; border-color: @controlHover@; }
QPushButton:pressed { background: @raisedPressed@; }
QPushButton:checked { background: @accentTint@; border-color: @accent@; }
QPushButton:focus { border: {2}px solid @accent@; padding: {4}px {13}px; }
QPushButton:default { background: @accent@; color: @onAccent@; border: 1px solid @accent@; }
QPushButton:default:hover { background: @accentHover@; border-color: @accentHover@; }
QPushButton:default:pressed { background: @accentPressed@; border-color: @accentPressed@; }
QPushButton:default:focus { border: {2}px solid @text@; padding: {4}px {13}px; }
QPushButton:disabled { background: @window@; color: @disabled@; border-color: @border@; }
QPushButton:default:disabled { background: @accentOff@; color: @onAccentOff@; border-color: @accentOff@; }
QPushButton:checked:default { background: @accentTint@; color: @text@; border: 1px solid @accent@; }
QPushButton:checked:default:focus { border: {2}px solid @accent@; padding: {4}px {13}px; }
QPushButton:flat { background: transparent; border-color: transparent; }
QPushButton:flat:hover { background: @hover@; }
QPushButton::menu-indicator { image: url("@g:chev-down-sm@"); subcontrol-origin: padding; subcontrol-position: center right; width: {8}px; height: {8}px; right: {-6}px; }

/* ---- check boxes, radio buttons, group boxes ---- */
QCheckBox, QRadioButton { color: @text@; spacing: {8}px; background: transparent; border: 1px solid transparent; border-radius: {4}px; padding: {1}px {2}px; }
QCheckBox:focus, QRadioButton:focus { border-color: @accent@; }
QCheckBox:disabled, QRadioButton:disabled { color: @disabled@; }
QCheckBox::indicator, QRadioButton::indicator, QGroupBox::indicator { width: {16}px; height: {16}px; }
QCheckBox::indicator:unchecked, QGroupBox::indicator:unchecked { image: url("@g:cb-off@"); }
QCheckBox::indicator:unchecked:hover, QGroupBox::indicator:unchecked:hover { image: url("@g:cb-off-hover@"); }
QCheckBox::indicator:checked, QGroupBox::indicator:checked { image: url("@g:cb-on@"); }
QCheckBox::indicator:checked:hover, QGroupBox::indicator:checked:hover { image: url("@g:cb-on-hover@"); }
QCheckBox::indicator:indeterminate { image: url("@g:cb-mixed@"); }
QCheckBox::indicator:unchecked:disabled, QGroupBox::indicator:unchecked:disabled { image: url("@g:cb-off-dis@"); }
QCheckBox::indicator:checked:disabled, QGroupBox::indicator:checked:disabled { image: url("@g:cb-on-dis@"); }
QCheckBox::indicator:indeterminate:disabled { image: url("@g:cb-mixed-dis@"); }
QRadioButton::indicator:unchecked { image: url("@g:rb-off@"); }
QRadioButton::indicator:unchecked:hover { image: url("@g:rb-off-hover@"); }
QRadioButton::indicator:checked { image: url("@g:rb-on@"); }
QRadioButton::indicator:checked:hover { image: url("@g:rb-on-hover@"); }
QRadioButton::indicator:unchecked:disabled { image: url("@g:rb-off-dis@"); }
QRadioButton::indicator:checked:disabled { image: url("@g:rb-on-dis@"); }
QGroupBox { background: transparent; color: @text@; border: 1px solid @border@; border-radius: {8}px; margin-top: 1.5em; padding: {2}px {8}px {6}px {8}px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: {2}px; top: 0px; padding: 0px {2}px; color: @secondary@; background: transparent; }
QGroupBox:disabled { color: @disabled@; }

)qss"
R"qss(/* ---- lists, tables, trees, headers ---- */
QAbstractItemView { background: @window@; alternate-background-color: @alt@; color: @text@; border: 1px solid @border@;
    selection-background-color: @selection@; selection-color: @text@; outline: none; }
QAbstractItemView:disabled { color: @disabled@; }
QAbstractItemView::item:hover { background: @hover@; }
QAbstractItemView::item:selected { background: @selection@; color: @text@; }
QAbstractItemView::item:selected:!active { background: @selectionInactive@; color: @text@; }
QAbstractItemView::indicator { width: {16}px; height: {16}px; }
QAbstractItemView::indicator:unchecked { image: url("@g:cb-off@"); }
QAbstractItemView::indicator:checked { image: url("@g:cb-on@"); }
QAbstractItemView::indicator:indeterminate { image: url("@g:cb-mixed@"); }
QTreeView { show-decoration-selected: 1; }
QTreeView#ServerTreeView { background: @panel@; border: none; alternate-background-color: transparent; show-decoration-selected: 0; qproperty-rootIsDecorated: false; }
QTreeView#ServerTreeView::item:selected { border-radius: {6}px; }
QTreeView::branch { background: transparent; }
QTreeView::branch:has-children:!has-siblings:closed, QTreeView::branch:closed:has-children:has-siblings { image: url("@g:chev-right@"); }
QTreeView::branch:open:has-children:!has-siblings, QTreeView::branch:open:has-children:has-siblings { image: url("@g:chev-down@"); }
QTableView { gridline-color: @border@; }
QTableView#clientsTableView, QTableView#banTableView { qproperty-alternatingRowColors: true; }
QTableCornerButton::section { background: @window@; border: none; border-right: 1px solid @border@; border-bottom: 1px solid @border@; }
QListWidget#localIconsListWidget::item:selected, QListWidget#remoteIconsListWidget::item:selected { border: 1px solid @control@; }
QHeaderView { background: @window@; border: none; }
QHeaderView::section { background: @window@; color: @secondary@; border: none; border-right: 1px solid @border@; border-bottom: 1px solid @border@; padding: {4}px {8}px; }
QHeaderView::section:hover { background: @hover@; color: @text@; }
QHeaderView::section:checked { color: @text@; }
QHeaderView::up-arrow { image: url("@g:chev-up-sm@"); width: {8}px; height: {8}px; padding-right: {6}px; }
QHeaderView::down-arrow { image: url("@g:chev-down-sm@"); width: {8}px; height: {8}px; padding-right: {6}px; }
QToolBox::tab { background: @raised@; color: @secondary@; border: 1px solid @border@; border-radius: {6}px; padding: {4}px {8}px; }
QToolBox::tab:selected { color: @text@; }

/* ---- sliders, progress bars ---- */
QSlider { background: transparent; }
QSlider::groove:horizontal { height: {4}px; background: @border@; border-radius: {2}px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: {2}px; }
QSlider::groove:vertical { width: {4}px; background: @border@; border-radius: {2}px; }
QSlider::add-page:vertical { background: @accent@; border-radius: {2}px; }
QSlider::handle:horizontal, QSlider::handle:vertical { background: @text@; border: {3}px solid @window@; width: {12}px; height: {12}px; border-radius: {9}px; }
QSlider::handle:horizontal { margin: {-7}px 0px; }
QSlider::handle:vertical { margin: 0px {-7}px; }
QSlider::handle:hover { background: @handleHover@; }
QSlider::handle:focus { border-color: @accent@; }
QSlider::sub-page:horizontal:disabled, QSlider::add-page:vertical:disabled { background: @disabled@; }
QSlider::handle:disabled { background: @disabled@; }
QProgressBar { background: @raised@; color: @text@; border: 1px solid @border@; border-radius: {6}px; text-align: center; }
QProgressBar::chunk { background: @accentDeep@; border: 1px solid @accent@; border-radius: {5}px; }

/* ---- scroll bars: thin, rounded, hover ---- */
QScrollBar:vertical { background: transparent; width: {12}px; margin: 0px; border: none; }
QScrollBar:horizontal { background: transparent; height: {12}px; margin: 0px; border: none; }
QScrollBar::handle:vertical { background: @thumb@; min-height: {32}px; border-radius: {3}px; margin: {2}px {3}px; }
QScrollBar::handle:horizontal { background: @thumb@; min-width: {32}px; border-radius: {3}px; margin: {3}px {2}px; }
QScrollBar::handle:hover { background: @thumbHover@; }
QScrollBar::handle:pressed { background: @thumbPressed@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QTextEdit#ChatTab QScrollBar:vertical { width: @m:chatBar@px; }
QTextEdit#ChatTab QScrollBar::handle:vertical { margin: {2}px @m:chatBarMargin@px; }
QAbstractScrollArea::corner { background: transparent; border: none; }
QSizeGrip { width: {12}px; height: {12}px; }

/* ---- TeamSpeak's server integrations ---- */
com--teamspeak--ui--ServerIntegrationsWidget QWidget#ExistingServerIntegration,
com--teamspeak--ui--ServerIntegrationsWidget QWidget#AvailableServerIntegration { background: @raised@; }
com--teamspeak--ui--ServerIntegrationsWidget com--teamspeak--ui--IntegrationResponseWidget { background: @window@; border: 1px solid @border@; border-radius: {4}px; }
com--teamspeak--ui--ServerIntegrationsWidget com--teamspeak--ui--IntegrationBubble { background: @raised@; border: 1px solid @border@; border-radius: {4}px; }
com--teamspeak--ui--IntegrationBubbleEditable QComboBox#IntegrationBubbleEditableComboBox { border: 1px solid @accent@; background: @raised@; padding: 0px {9}px {1}px {9}px; }
com--teamspeak--ui--ServerIntegrationsWidget com--teamspeak--ui--ErrorIndicator { background: @error@; border: none; border-radius: {4}px; }
com--teamspeak--ui--ErrorIndicator QLabel#IntegrationErrorIndicatorLabel { color: @onAccent@; }

/* ---- Options -> Addons: each add-on's box (the style's own light-blue frame otherwise) ---- */
com--teamspeak--ui--addons--Addon_Frame { border: 1px solid @border@; border-radius: {6}px; }
/* My Addons and Browse online: TeamSpeak sizes them for its own look, too narrow for their text at 150 % and more */
#Addons_Setup > QPushButton, #Addons_Setup > QPushButton:focus { padding: {3}px {8}px; min-width: {108}px; }

/* ---- the channel's file browser: the download folder and the count (the style's light frame otherwise) ---- */
#FileBrowser QLabel#directoryLabel, #FileBrowser QLabel#counterLabel { border: 1px solid @border@; border-radius: {4}px; padding: 0px {4}px; }
)qss";

// The chat document style sheet for new lines (design_dark 2.7, the prototype's dark_chat.css.in).
const char kChatSheet[] =
R"css(/* TS Media chat: Dark mode (chat document style sheet for new lines) */
p { padding: 0px; margin: 0px; }
a { color: @link@; }
.TextMessage_UserLink { color: @text@; font-weight: bold; text-decoration: none; }
.TextMessage_OfflineLink, .TextMessage_ChannelLink, .TextMessage_ServerLink { color: @link@; font-weight: bold; text-decoration: none; }
.TextMessage_HostName { color: @secondary@; font-weight: bold; }
.TextMessage_Welcome, .TextMessage_HostMessage, .TextMessage_Poke { color: @success@; }
.TextMessage_Normal, .TextMessage_Connected, .TextMessage_ClientConnected, .TextMessage_ClientJoined,
.TextMessage_Disconnected, .TextMessage_ClientDisconnected, .TextMessage_ClientSwitched, .TextMessage_ClientLeft,
.TextMessage_ChannelCreated, .TextMessage_ChannelDeleted, .TextMessage_ChannelEdited, .TextMessage_ChannelMoved, .TextMessage_ServerEdited,
.TextMessage_GroupAssigned, .TextMessage_GroupRevoked, .TextMessage_TalkPowerAssigned, .TextMessage_TalkPowerRevoked,
.TextMessage_TalkPowerRequested { color: @muted@; }
.TextMessage_ClientDropped, .TextMessage_ClientKicked, .TextMessage_ClientBanned { color: @error@; }
.TextMessage_Time { color: @muted@; font-weight: normal; }
.TextMessage_BeginHistory, .TextMessage_EndHistory { color: @muted@; font-weight: bold; font-style: italic; }
.TextMessage_ServerError { color: @error@; font-weight: bold; }
.InfoFrame_Title { font-size: x-large; }
.InfoFrame_Title .TextMessage_UserLink, .InfoFrame_Title .TextMessage_ChannelLink, .InfoFrame_Title .TextMessage_ServerLink { color: @text@; font-weight: normal; }
.InfoFrame_Title .InfoFrame_CustomNick { color: @link@; }
.InfoFrame_Table { border-collapse: collapse; border-spacing: 0px; }
.InfoFrame_Table td { padding: 0px 4px 0px 1px; }
.Highlight, .InfoFrame_Table td.Highlight { color: @link@; }
.Important, .InfoFrame_Table td.Important { color: @error@; }
.Active, .InfoFrame_Table td.Active { color: @success@; }
.Inactive, .InfoFrame_Table td.Inactive { color: @muted@; }
.InfoFrame_Table td.Label { font-weight: bold; color: @secondary@; }
.InfoFrame_Table td.Bottom { vertical-align: bottom; }
.InfoFrame_Table td.List { padding: 0px 1px 0px 8px; }
.InfoFrame_Table td.Badge { padding: 5px 5px 0px 5px; }
.InfoFrame_Table td.Space-Top { padding: 10px 0px 0px 0px; }
.InfoFrame_Table td.Space-Bot { padding: 0px 0px 10px 0px; }
.InfoFrame_Table.Space-Top { margin: 10px 0px 0px 0px; }
.InfoFrame_Table.Space-Bot { margin: 0px 0px 10px 0px; }
)css";

struct Named {
    const char*   name;
    QColor Tokens::*field;
};

// The template names of the tokens.
const Named kNamed[] = {
    {"panel", &Tokens::panel},
    {"window", &Tokens::window},
    {"raised", &Tokens::raised},
    {"field", &Tokens::field},
    {"raisedHover", &Tokens::raisedHover},
    {"raisedPressed", &Tokens::raisedPressed},
    {"hover", &Tokens::hover},
    {"menuHover", &Tokens::menuHover},
    {"hoverStrong", &Tokens::hoverStrong},
    {"border", &Tokens::border},
    {"control", &Tokens::control},
    {"controlHover", &Tokens::controlHover},
    {"text", &Tokens::text},
    {"secondary", &Tokens::secondary},
    {"muted", &Tokens::muted},
    {"disabled", &Tokens::disabled},
    {"disabledFill", &Tokens::disabledFill},
    {"accent", &Tokens::accent},
    {"accentHover", &Tokens::accentHover},
    {"accentPressed", &Tokens::accentPressed},
    {"onAccent", &Tokens::onAccent},
    {"accentDeep", &Tokens::accentDeep},
    {"accentOff", &Tokens::accentOff},
    {"onAccentOff", &Tokens::onAccentOff},
    {"accentTint", &Tokens::accentTint},
    {"link", &Tokens::link},
    {"selection", &Tokens::selection},
    {"selectionInactive", &Tokens::selectionInactive},
    {"thumb", &Tokens::thumb},
    {"thumbHover", &Tokens::thumbHover},
    {"thumbPressed", &Tokens::thumbPressed},
    {"alt", &Tokens::alt},
    {"success", &Tokens::success},
    {"warning", &Tokens::warning},
    {"error", &Tokens::error},
    {"handleHover", &Tokens::handleHover},
};

QColor rgb(QRgb value)
{
    return QColor::fromRgb(value);
}

Tokens makeTokens()
{
    Tokens t;
    t.panel             = rgb(0x1f1e1d);
    t.window            = rgb(0x262624);
    t.raised            = rgb(0x30302e);
    t.field             = rgb(0x2b2b29);
    t.raisedHover       = rgb(0x383835);
    t.raisedPressed     = rgb(0x2c2c2a);
    t.hover             = rgb(0x30302d);
    t.menuHover         = rgb(0x41403c);
    t.hoverStrong       = rgb(0x3e3e3a);
    t.border            = rgb(0x3e3e3a);
    t.control           = rgb(0x7d7b75);
    t.controlHover      = rgb(0x96948e);
    t.text              = rgb(0xf5f4ee);
    t.secondary         = rgb(0xc2c0b6);
    t.muted             = rgb(0xa3a198);
    t.disabled          = rgb(0x7f7d77);
    t.disabledFill      = rgb(0x2a2a28);
    t.accent            = rgb(0xd97757);
    t.accentHover       = rgb(0xe08a6c);
    t.accentPressed     = rgb(0xcc6b4b);
    t.onAccent          = rgb(0x1a1918);
    t.accentDeep        = rgb(0xa9512f);
    t.accentOff         = rgb(0x4e3a32);
    t.onAccentOff       = rgb(0xa3a198);
    t.accentTint        = rgb(0x4a362e);
    t.link              = rgb(0xe8a383);
    t.selection         = QColor(217, 119, 87, 77);
    t.selectionInactive = QColor(217, 119, 87, 41);
    t.thumb             = rgb(0x7c7a74);
    t.thumbHover        = rgb(0x96948e);
    t.thumbPressed      = rgb(0xa3a198);
    t.alt               = rgb(0x2a2a28);
    t.success           = rgb(0x7fb685);
    t.warning           = rgb(0xe5b567);
    t.error             = rgb(0xe5806e);
    t.handleHover       = rgb(0xffffff);
    return t;
}

// {N} -> round(N * k) (negative and fractional N too).
QString scaleSizes(const QString& text, qreal k)
{
    const QRegularExpression size(QString::fromLatin1("\\{(-?[0-9.]+)\\}"));
    QString                  out;
    out.reserve(text.size());
    int  last = 0;
    auto it   = size.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.midRef(last, m.capturedStart() - last);
        out += QString::number(px(m.captured(1).toDouble(), k));
        last = m.capturedEnd();
    }
    out += text.midRef(last);
    return out;
}

// One rule (with the comments and blank lines before it) with its @...@ placeholders replaced; false: leave it out (a
// glyph without a glyph folder, or a name that isn't a token).
bool expandRule(const QString& rule, const Tokens& t, const QString& glyphDir, const Metrics* m, qreal k, QString* out, QStringList* unknown)
{
    const QRegularExpression at(QString::fromLatin1("@([a-zA-Z]+(?::[a-zA-Z0-9-]+)?)@"));
    out->clear();
    int  last = 0;
    auto it   = at.globalMatch(rule);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString                 name  = match.captured(1);
        QString                       value;
        if (name.startsWith(QLatin1String("g:"))) {
            if (glyphDir.isEmpty())
                return false;
            value = QString::fromLatin1("%1/%2.png").arg(glyphDir, name.mid(2));
        } else if (name.startsWith(QLatin1String("m:")) && m) {
            const QString metric = name.mid(2);
            if (metric == QLatin1String("chatFrame"))
                value = QString::number(qMax(0, m->chatFrame));
            else if (metric == QLatin1String("chatBar"))
                value = QString::number(qMax(1, m->chatBar));
            else if (metric == QLatin1String("chatBarMargin"))
                value = QString::number(qMax(1, (m->chatBar - px(6, k)) / 2));
        } else {
            const QColor c = tokenByName(t, name);
            if (c.isValid())
                value = cssColor(c);
        }
        if (value.isEmpty()) {
            if (unknown)
                unknown->append(name);
            return false;
        }
        *out += rule.midRef(last, match.capturedStart() - last);
        *out += value;
        last = match.capturedEnd();
    }
    *out += rule.midRef(last);
    return true;
}

// Every rule of text expanded (rules end at '}'; the {N} sizes are replaced before, so every brace left is a rule's).
QString expand(const QString& text, const Tokens& t, const QString& glyphDir, const Metrics* m, qreal k, QStringList* unknown)
{
    QString out;
    out.reserve(text.size() + text.size() / 4);
    QString rule;
    int     start = 0;
    while (start < text.size()) {
        int end = text.indexOf(QLatin1Char('}'), start);
        end     = end < 0 ? text.size() : end + 1;
        if (expandRule(text.mid(start, end - start), t, glyphDir, m, k, &rule, unknown))
            out += rule;
        start = end;
    }
    return out;
}

} // namespace

const Tokens& tokens()
{
    static const Tokens t = makeTokens();
    return t;
}

settingstheme::Tokens settingsTokens(qreal k)
{
    settingstheme::Tokens t;
    t.dark     = true;
    t.k        = k > 0 ? k : 1.0;
    t.window   = rgb(0x262624);
    t.text     = rgb(0xf5f4ee);
    t.card     = rgb(0x30302e);
    t.border   = rgb(0x3e3e3a);
    t.divider  = rgb(0x3a3a36);
    t.sidebar  = rgb(0x1f1e1d);
    t.muted    = rgb(0xa3a198);
    t.disabled = rgb(0x7f7d77);
    t.control  = rgb(0x7d7b75);
    t.accent   = rgb(0xd97757);
    t.onAccent = rgb(0x1a1918);
    t.focus    = rgb(0xf5f4ee);
    t.navSel   = rgb(0x4a362e);
    t.navHover = rgb(0x2f2e2c);
    return t;
}

QPalette palette(const QPalette& base)
{
    const Tokens& t = tokens();
    QPalette      p = base;
    const auto    set = [&p](QPalette::ColorRole role, const QColor& color) { p.setColor(QPalette::All, role, color); };
    set(QPalette::Window, t.window);
    set(QPalette::WindowText, t.text);
    set(QPalette::Base, t.window);
    set(QPalette::AlternateBase, t.alt);
    set(QPalette::ToolTipBase, t.raised);
    set(QPalette::ToolTipText, t.text);
    set(QPalette::PlaceholderText, t.muted);
    set(QPalette::Text, t.text);
    set(QPalette::Button, t.raised);
    set(QPalette::ButtonText, t.text);
    set(QPalette::BrightText, rgb(0xffffff));
    set(QPalette::Light, rgb(0x4a4945));
    set(QPalette::Midlight, rgb(0x3e3e3a));
    set(QPalette::Mid, rgb(0x33332f));
    set(QPalette::Dark, rgb(0x1a1918));
    set(QPalette::Shadow, rgb(0x0d0d0c));
    set(QPalette::Highlight, rgb(0x5d3f33)); // the selection over the window, opaque
    set(QPalette::HighlightedText, t.text);
    set(QPalette::Link, t.link);
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, t.disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, rgb(0x3a3532));
    return p;
}

qreal scaleFactor(const QFont& font)
{
    return qMax<qreal>(1.0, QFontInfo(font).pixelSize() / 11.0);
}

int px(qreal v, qreal k)
{
    return qRound(v * k);
}

QString cssColor(const QColor& color)
{
    if (color.alpha() == 255)
        return color.name();
    return QString::fromLatin1("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

QColor tokenByName(const Tokens& t, const QString& name)
{
    for (const Named& n : kNamed) {
        if (name == QLatin1String(n.name))
            return t.*(n.field);
    }
    return QColor();
}

QString appSheetTemplate()
{
    return QString::fromLatin1(kAppSheet);
}

QString chatSheetTemplate()
{
    return QString::fromLatin1(kChatSheet);
}

QString paneRule(const Tokens& t, const Metrics& m)
{
    if (m.paneSelector.isEmpty())
        return QString();
    QString vertical;
    if (m.paneTop >= 0 && m.paneBottom >= 0) {
        // 2.7.3 (M6): the pages as far from the top and the bottom as in the skin (top: 0px; the tab bars are as tall as the
        // skin's: barRules). The design's line along the pane's top stays when the skin leaves room for it (its rest is
        // padding); the bottom edge in the window colour.
        if (m.paneTop >= 1)
            vertical = QString::fromLatin1(" border-top: 1px solid %1; padding-top: %2px;").arg(cssColor(t.border)).arg(m.paneTop - 1);
        else
            vertical = QString::fromLatin1(" border-top: 0px; padding-top: 0px;");
        vertical += QString::fromLatin1(" border-bottom: %1px solid %2; padding-bottom: 0px; top: 0px;").arg(m.paneBottom).arg(cssColor(t.window));
    }
    return QString::fromLatin1("%1::pane { border-left: %2px solid %4; border-right: %3px solid %4;%5 }\n")
        .arg(m.paneSelector)
        .arg(qMax(0, m.paneLeft))
        .arg(qMax(0, m.paneRight))
        .arg(cssColor(t.window), vertical);
}

QString barRules(qreal k, const Metrics& m)
{
    const qreal scale = k > 0 ? k : 1.0;
    QString     out;
    // Tabs: no vertical padding, the content as tall as the skin's tab without the design's accent line ({2}).
    const auto tabs = [&](const char* area, int height) {
        if (height > 0)
            out += QString::fromLatin1("QWidget#%1 QTabBar::tab { padding-top: 0px; padding-bottom: 0px; min-height: %2px; max-height: %2px; }\n")
                       .arg(QString::fromLatin1(area))
                       .arg(qMax(1, height - px(2, scale)));
    };
    tabs("MainWindowServerTabsWidget", m.serverTab);
    tabs("MainWindowChatWidget", m.chatTab);
    // The menu bar: the skin's frame and margin around its items as its padding (the design's has no border), its items as
    // tall as the skin's (the text's line plus their paddings).
    if (m.menuItem > 0 && m.menuText > 0) {
        const int pad = qMax(0, m.menuItem - m.menuText);
        out += QString::fromLatin1("QMainWindow > QMenuBar { padding: %1px %2px; }\nQMainWindow > QMenuBar::item { padding: %3px %4px %5px %4px; }\n")
                   .arg(qMax(0, m.menuMargin))
                   .arg(px(4, scale))
                   .arg(pad / 2)
                   .arg(px(10, scale))
                   .arg(pad - pad / 2);
    }
    // Tool buttons: the vertical padding that gives the skin's height around their contents (QStyleSheetStyle adds 3 px to
    // a tool button's contents, the design's borders are 1 px). A tool bar's padding is one margin on every side: an odd
    // rest goes to its buttons (1 px taller), so the bar is the skin's height.
    const auto toolPad = [](int button, int content) { return qMax(0, button - content - 5); };
    if (m.toolBar > 0 && m.toolButton > 0 && m.toolContent > 0) {
        const int margin = qMax(0, m.toolBar - m.toolButton);
        const int pad    = toolPad(m.toolButton, m.toolContent) + margin % 2;
        out += QString::fromLatin1("QMainWindow > QToolBar { padding: %1px; }\nQMainWindow > QToolBar > QToolButton { padding: %2px %3px %4px %3px; }\n")
                   .arg(margin / 2)
                   .arg(pad / 2)
                   .arg(px(3, scale))
                   .arg(pad - pad / 2);
        // A separator taller than the buttons would make the bar taller (its margins are the design's otherwise).
        if (1 + 2 * px(6, scale) > m.toolButton)
            out += QString::fromLatin1("QMainWindow > QToolBar::separator { margin: %1px %2px; }\n").arg(qMax(0, (m.toolButton - 1) / 2)).arg(px(5, scale));
    }
    if (m.inputButton > 0 && m.inputContent > 0) {
        const int pad = toolPad(m.inputButton, m.inputContent);
        out += QString::fromLatin1("QWidget#MainWindowChatWidget > QToolButton { padding: %1px %2px %3px %2px; }\n").arg(pad / 2).arg(px(3, scale)).arg(pad - pad / 2);
    }
    if (m.splitterV > 0)
        out += QString::fromLatin1("QMainWindow QSplitter::handle:vertical { height: %1px; }\n").arg(m.splitterV);
    if (m.splitterH > 0)
        out += QString::fromLatin1("QMainWindow QSplitter::handle:horizontal { width: %1px; }\n").arg(m.splitterH);
    // The status bar keeps the design's 1 px line at its top.
    if (m.status > 1)
        out += QString::fromLatin1("QMainWindow > QStatusBar { min-height: %1px; max-height: %1px; }\n").arg(m.status - 1);
    // Review fix: the main splitter's areas as small as in the skin (a minimum of their own: QSplitter takes it over the
    // smallest size their contents ask for, which dark mode's scroll bars and frames made another).
    for (const QPair<QString, int>& area : m.areas) {
        if (!area.first.isEmpty() && area.second > 0)
            out += QString::fromLatin1("QMainWindow QSplitter > QWidget#%1 { min-height: %2px; }\n").arg(area.first).arg(area.second);
    }
    return out;
}

QString appSheet(const Tokens& t, qreal k, const Metrics& m, const QString& glyphDir, QStringList* unknown)
{
    const qreal scale = k > 0 ? k : 1.0;
    QString     sheet = expand(scaleSizes(appSheetTemplate(), scale), t, glyphDir, &m, scale, unknown);
    // 2.7.2 (D3): after the generic pane rule (as specific, later: its side borders win), the chat column's skin width.
    const QString pane = paneRule(t, m);
    if (!pane.isEmpty())
        sheet += QString::fromLatin1("\n/* the chat column keeps the skin's width */\n") + pane;
    // 2.7.3 (M6): and its height: TeamSpeak's main window bars as tall as in the skin.
    const QString bars = barRules(scale, m);
    if (!bars.isEmpty())
        sheet += QString::fromLatin1("\n/* the main window's bars keep the skin's heights */\n") + bars;
    return sheet;
}

QString chatSheet(const Tokens& t, QStringList* unknown)
{
    return expand(chatSheetTemplate(), t, QString(), nullptr, 1.0, unknown);
}

namespace {

bool isFontProperty(const QString& name)
{
    return name == QLatin1String("font") || name.startsWith(QLatin1String("font-"));
}

// 2.7.3 (M9): what places text in a chat or info panel document: margins, paddings, table spacing, alignment, line height,
// indent, white space and the font's size and family (also the font shorthand, which carries them). Review fix: borders too
// (their width takes room; each with the style's own colour and line, so a table or cell made while dark mode is on is the
// style's after off), and the rest of the layout Qt's text documents read: block and list indents, empty paragraphs, floats,
// widths and heights, letter and word spacing, list styles, page breaks. Never a text or background colour, a weight, an
// italic, a decoration or capitals (dark mode's own: the looks and their way back).
bool isSpacingProperty(const QString& name)
{
    static const char* const kExact[] = {"vertical-align", "line-height", "-qt-line-height-type", "text-indent", "text-align", "white-space",
                                         "font-size", "font-family", "font", "letter-spacing", "word-spacing", "float", "width", "height",
                                         "min-width", "min-height", "max-width", "max-height", "-qt-block-indent", "-qt-list-indent",
                                         "-qt-paragraph-type", "-qt-table-type", "list-style", "list-style-type", "-qt-list-number-prefix",
                                         "-qt-list-number-suffix", "page-break-before", "page-break-after"};
    if (name.startsWith(QLatin1String("margin")) || name.startsWith(QLatin1String("padding")) || name.startsWith(QLatin1String("border")))
        return true;
    for (const char* exact : kExact) {
        if (name == QLatin1String(exact))
            return true;
    }
    return false;
}

// Every rule of sheet with only the declarations keep() takes (by their lower-case property name), each rule with its
// selector as written ("selector { declaration; declaration; }"); comments dropped, a rule left without a declaration too.
QString rulesWith(const QString& sheet, bool (*keep)(const QString& name), bool keepIt = true)
{
    // A small reader of Qt's style sheet syntax: comments, quoted strings (selectors' [name="value"], font families,
    // url("...")) and parentheses never end a selector, a rule or a declaration.
    QString out;
    QString selector;
    int     i = 0;
    const int n = sheet.size();
    const auto skipComment = [&sheet, n](int at) {
        const int end = sheet.indexOf(QLatin1String("*/"), at + 2);
        return end < 0 ? n : end + 2;
    };
    const auto skipString = [&sheet, n](int at) {
        const QChar quote = sheet.at(at);
        for (int k = at + 1; k < n; ++k) {
            if (sheet.at(k) == QLatin1Char('\\'))
                ++k;
            else if (sheet.at(k) == quote)
                return k + 1;
        }
        return n;
    };
    while (i < n) {
        const QChar c = sheet.at(i);
        if (c == QLatin1Char('/') && i + 1 < n && sheet.at(i + 1) == QLatin1Char('*')) {
            i = skipComment(i);
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const int end = skipString(i);
            selector += sheet.midRef(i, end - i);
            i = end;
            continue;
        }
        if (c == QLatin1Char('}')) { // a stray one: nothing to take
            selector.clear();
            ++i;
            continue;
        }
        if (c != QLatin1Char('{')) {
            selector += c;
            ++i;
            continue;
        }
        // The rule's body: its declarations, split at ';' outside strings and parentheses.
        QStringList kept;
        QString     declaration;
        int         depth = 0;
        ++i;
        while (i < n) {
            const QChar d = sheet.at(i);
            if (d == QLatin1Char('/') && i + 1 < n && sheet.at(i + 1) == QLatin1Char('*')) {
                i = skipComment(i);
                continue;
            }
            if (d == QLatin1Char('"') || d == QLatin1Char('\'')) {
                const int end = skipString(i);
                declaration += sheet.midRef(i, end - i);
                i = end;
                continue;
            }
            if (d == QLatin1Char('('))
                ++depth;
            else if (d == QLatin1Char(')') && depth > 0)
                --depth;
            if (depth == 0 && (d == QLatin1Char(';') || d == QLatin1Char('}'))) {
                const QString decl  = declaration.trimmed();
                const int     colon = decl.indexOf(QLatin1Char(':'));
                if (colon > 0 && keep(decl.left(colon).trimmed().toLower()) == keepIt)
                    kept << decl;
                declaration.clear();
                ++i;
                if (d == QLatin1Char('}'))
                    break;
                continue;
            }
            declaration += d;
            ++i;
        }
        const QString which = selector.simplified();
        if (!kept.isEmpty() && !which.isEmpty())
            out += which + QString::fromLatin1(" { ") + kept.join(QString::fromLatin1("; ")) + QString::fromLatin1("; }\n");
        selector.clear();
    }
    return out;
}

} // namespace

QString fontRules(const QString& sheet)
{
    return rulesWith(sheet, isFontProperty);
}

QString chatSheet(const Tokens& t, const QString& skinChat, QStringList* unknown)
{
    // 2.7.3 (M9): the template's colours and text styles (weight, italic, decoration), none of its spacing, then the style's
    // own spacing and layout rules with their selectors (isSpacingProperty): lines TeamSpeak prints while dark mode is on get
    // the style's margins, paddings, borders, indents, text sizes and fonts, so their block, table and cell formats are the
    // style's and none of that changes with a toggle (an info panel or a chat that was empty when dark mode saw it kept dark
    // mode's paragraph and table spacing after off before). The text styles are dark mode's: the way back gives the style's
    // (darkchat's fonts).
    QString sheet = QString::fromLatin1("/* TS Media chat: Dark mode (chat document style sheet for new lines) */\n")
                    + rulesWith(expand(chatSheetTemplate(), t, QString(), nullptr, 1.0, unknown), isSpacingProperty, false);
    const QString spacing = rulesWith(skinChat, isSpacingProperty);
    if (!spacing.isEmpty())
        sheet += QString::fromLatin1("/* the style's spacing */\n") + spacing;
    return sheet;
}

} // namespace darkpalette
