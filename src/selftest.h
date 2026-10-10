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
    qint64                    m_jobsLoggedAt   = 0;
    bool                      runPart2(const QString& cmd, const QJsonObject& c, bool* handled);
    void                      reactStep(const QString& key, int reaction, int left, int gapMs);
};

#endif
