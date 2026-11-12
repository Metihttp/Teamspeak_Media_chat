#pragma once

// Test builds only (-DTSMEDIA_TESTHOOKS=ON; never compiled into a release): a scripted driver for live
// tests inside TeamSpeak without a mouse. <data dir>/selftest_script.txt is picked up (and deleted)
// while the current server tab is connected to a localhost server; it holds one JSON object per line,
// run one after another ({"cmd":"send","files":[...],"caption":"..."}, {"cmd":"wait","ms":2000}, ...;
// see selftest.cpp for the commands). Every command logs a "[test] script" line to TeamSpeak's log;
// grabs go to <data dir>/debug. Nothing runs, and a running script stops, as soon as the current tab
// is not a localhost server.

#ifdef TSMEDIA_TESTHOOKS

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

class ChatIntegration;
class Core;
class QTimer;
class QUrl;
class QWidget;

class SelfTest : public QObject
{
    Q_OBJECT

  public:
    struct Hooks {
        std::function<void()>     openSettings;   // plugin.cpp showSettings(nullptr)
        std::function<QWidget*()> settingsDialog; // the open settings dialog, or nullptr
    };

    SelfTest(Core* core, ChatIntegration* chat, Hooks hooks, QObject* parent);

  public slots:
    // 2.4 live pass B: QDesktopServices' http/https handler while "browser.capture" is on (no browser opens).
    void capturedUrl(const QUrl& url);

  private:
    void poll();
    void next(int delayMs = 0);
    void runOne();
    bool run(const QJsonObject& command); // false: the command continues by itself (it calls next())
    void finish(const QString& why);

    QString findKey(const QString& namePart) const; // the newest entry whose file name contains namePart
    QString saveGrab(QWidget* widget, const QString& name) const;
    void    logJobs() const;
    void    logEntries(const QString& match) const;
    void    waitIdle(qint64 deadline);
    void    waitCompose(qint64 deadline);
    void    settingsStep(const QString& prefix, int tab);

    Core*                     m_core;
    QPointer<ChatIntegration> m_chat;
    Hooks                     m_hooks;
    QTimer*                   m_poll  = nullptr;
    QTimer*                   m_step  = nullptr;
    QStringList               m_lines;
    int                       m_pos     = 0;
    bool                      m_running = false;
    qint64                    m_notLocalSince = 0; // the current tab stopped being a localhost server then
    int                       m_logJobsEveryMs = 0; // waitidle: progress lines this often (0: none)
    bool                      m_micHeld        = false; // part 5: micbtn press .. release / up (the recorder reads it while held)
    qint64                    m_jobsLoggedAt   = 0;
    bool                      runPart2(const QString& cmd, const QJsonObject& c, bool* handled);
    void                      reactStep(const QString& key, int reaction, int left, int gapMs);
    // Part 3 (replies and HD emoji): context menus, keys into the chat input, clicks on reply lines.
    bool                      runPart3(const QString& cmd, const QJsonObject& c, bool* handled);
    void                      later(int ms, std::function<void()> fn); // a child timer: gone with the driver
    // Part 4 (chat redesign): the chat layout (layout, layoutinfo, hover) and TeamSpeak's emoji button (emojibtn).
    bool                      runPart4(const QString& cmd, const QJsonObject& c, bool* handled);
    // 2.3 live pass A (chat and protocol): avatars on the localhost test server, text reaction rows with the mouse.
    bool                      runPart6(const QString& cmd, const QJsonObject& c, bool* handled);
    // 2.4 live pass A (link previews): the Settings window's check boxes, the hub's counters, hand-made LPs.
    bool                      runPart7(const QString& cmd, const QJsonObject& c, bool* handled);
    // 2.4 live pass B (click-to-load, network safety): browser calls captured, the event loop's longest gaps.
    bool                      runPart8(const QString& cmd, const QJsonObject& c, bool* handled);
    // 2.5 GIFs: the emoji picker's GIFs tab (gifs.list, gifs.open, gifs.tab, gifs.hover, gifs.focus, gifs.key, gifs.send, ...).
    QString                   runGifs(const QString& cmd, const QJsonObject& c);
    QTimer*                   m_stall      = nullptr; // stall.start .. stall.stop
    qint64                    m_stallStart = 0;
    qint64                    m_stallLast  = 0;
    qint64                    m_stallMax   = 0;
    qint64                    m_stallMaxAt = 0;
    int                       m_stallTicks = 0;
    int                       m_stallOver[3] = {0, 0, 0}; // gaps over 100, 250 and 1000 ms
};

#endif
