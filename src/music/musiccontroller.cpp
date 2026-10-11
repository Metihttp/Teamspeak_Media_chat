#include "music/musiccontroller.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLayout>
#include <QPalette>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>

#include <chrono>

#include "attachmenu.h"
#include "i18n.h"
#include "inputbars.h"
#include "medialink.h"
#include "music/musicmix.h"
#include "servertabs.h"
#include "ts3api.h"
#include "uiutil.h"
#include "video/mfvideo.h"

namespace {

constexpr int kTagMaxChars = 120;

const char* const kEntryId = "livemusic";

MusicController* g_current = nullptr; // the newest controller (the live-test driver reads it)

QString cleanTag(const QString& text)
{
    return sanitizeCaption(text).left(kTagMaxChars); // no controls, bidi marks or line breaks
}

bool readSelf(quint64 sch, size_t flag, int* value)
{
    return ts3::funcs.getClientSelfVariableAsInt && ts3::funcs.getClientSelfVariableAsInt(sch, flag, value) == ERROR_ok;
}

bool setSelf(quint64 sch, size_t flag, int value)
{
    if (!ts3::funcs.setClientSelfVariableAsInt || !ts3::funcs.flushClientSelfUpdates)
        return false;
    if (ts3::funcs.setClientSelfVariableAsInt(sch, flag, value) != ERROR_ok)
        return false;
    // The set took effect; the flush only tells the server. CLIENT_INPUT_DEACTIVATED is never the server's:
    // nothing to send there is ERROR_ok_no_update, a success.
    const unsigned int flushed = ts3::funcs.flushClientSelfUpdates(sch, nullptr);
    return flushed == ERROR_ok || flushed == ERROR_ok_no_update;
}

// 2.3 review: TeamSpeak's capture uses voice activation ("vad" in its preprocessor). Its input is then never
// push-to-talk, whatever CLIENT_INPUT_DEACTIVATED reads for a moment (TeamSpeak deactivates the input of a
// server tab in the background and activates it again when the tab comes back): the plugin never keeps it
// open, so the voice is never left out. False if it can't be read (push-to-talk handling as before).
bool voiceActivation(quint64 sch)
{
    char* value = nullptr;
    if (!sch || !ts3::funcs.getPreProcessorConfigValue || ts3::funcs.getPreProcessorConfigValue(sch, "vad", &value) != ERROR_ok || !value)
        return false;
    const bool on = qstrcmp(value, "true") == 0;
    if (ts3::funcs.freeMemory)
        ts3::funcs.freeMemory(value);
    return on;
}

// 2.3 live test: whether TeamSpeak sends our voice now (its talk flag for the own client); -1: can't be read.
int selfTalking(quint64 sch)
{
    int value = 0;
    if (ts3::funcs.getClientSelfVariableAsInt && ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_FLAG_TALKING, &value) == ERROR_ok)
        return value ? 1 : 0;
    const anyID me = ts3::ownClientId(sch);
    if (me && ts3::funcs.getClientVariableAsInt && ts3::funcs.getClientVariableAsInt(sch, me, CLIENT_FLAG_TALKING, &value) == ERROR_ok)
        return value ? 1 : 0;
    return -1;
}

// TeamSpeak's voice activation for sch (runtime only: the user's capture profile isn't changed).
bool setVoiceActivation(quint64 sch, bool on)
{
    return sch && ts3::funcs.setPreProcessorConfigValue && ts3::funcs.setPreProcessorConfigValue(sch, "vad", on ? "true" : "false") == ERROR_ok;
}

constexpr int kFadeOutWaitMs = 40; // a stop (or a push-to-talk pause) waits this long at most for the fade out

// The chat browser on screen right above input (not the server info pane beside it), kept at its end
// when the bar takes or gives back height.
QTextBrowser* chatAbove(QWidget* input)
{
    if (!input)
        return nullptr;
    QWidget*      window = input->window();
    const QRect   in(input->mapTo(window, QPoint(0, 0)), input->size());
    QTextBrowser* best   = nullptr;
    int           bottom = -1;
    for (QTextBrowser* b : window->findChildren<QTextBrowser*>()) {
        if (!b->isVisible() || b->objectName().startsWith(QLatin1String("tsmedia")))
            continue;
        const QRect r(b->mapTo(window, QPoint(0, 0)), b->size());
        if (r.bottom() <= in.top() && r.right() >= in.left() && r.left() <= in.right() && r.bottom() > bottom) {
            best   = b;
            bottom = r.bottom();
        }
    }
    return best;
}

bool atEnd(QTextBrowser* chat)
{
    return chat && chat->verticalScrollBar()->value() >= chat->verticalScrollBar()->maximum() - 2;
}

QWidget* findChatInput()
{
    for (QWidget* w : QApplication::allWidgets()) {
        if (w->inherits("ChatLineEdit") && w->isVisible())
            return w;
    }
    return nullptr;
}

} // namespace

MusicController::Host MusicController::pluginHost()
{
    Host h;
    h.input             = [] { return findChatInput(); };
    h.visibleConnection = [] { return static_cast<quint64>(ts3::currentConnection()); };
    h.source            = [](const QString& path) { return music::fileSource(path); };
    h.pickFile          = [](QWidget* parent, const QString& dir) {
        return QFileDialog::getOpenFileName(parent, i18n::t("Play music to the channel"), dir,
                                            i18n::t("Music (*.mp3 *.m4a *.aac *.flac *.wav *.ogg *.oga *.opus *.wma);;All files (*.*)"));
    };
    h.placeName    = [](quint64 sch, quint64 channel) {
        const QString server = ts3::serverName(sch);
        const QString room   = channel ? ts3::channelName(sch, channel) : QString();
        return room.isEmpty() ? server : server.isEmpty() ? room : server + QString::fromUtf8(" \xE2\x80\xBA ") + room;
    };
    h.showConnection = [](quint64 sch) { return servertabs::show(sch); };
    h.settingsFile   = ts3::dataDir() + QLatin1String("/music.ini");
    return h;
}

MusicController::MusicController(Host host, QObject* parent)
    : QObject(parent)
    , m_host(std::move(host))
{
    // Child timers only (never a functor single shot): they die with this object, before the DLL goes.
    m_tick = new QTimer(this);
    m_tick->setInterval(kTickMs);
    connect(m_tick, &QTimer::timeout, this, &MusicController::tick);
    m_noticeTimer = new QTimer(this);
    m_noticeTimer->setSingleShot(true);
    connect(m_noticeTimer, &QTimer::timeout, this, &MusicController::endNotice);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(800);
    connect(m_saveTimer, &QTimer::timeout, this, &MusicController::saveSettings);
    m_lastDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    loadSettings();

    // The paperclip menu's entry (attachmenu.h: order 40, after Photo or video, Document and Poll); removed
    // again in shutdown().
    attachmenu::Entry entry;
    entry.id    = QString::fromLatin1(kEntryId);
    entry.order = kEntryOrder;
    entry.text  = [] { return i18n::t("Live music"); };
    entry.icon  = [] { return MusicBar::menuIcon(); }; // its own hue, like the menu's other entries
    const QPointer<MusicController> self(this);
    entry.enabled = [self] {
        // Connected and in a channel (like the menu's other entries). What else stands in the way (no talk
        // power, no Media Foundation) is said in the bar once it is chosen, not hidden behind a grey item.
        const quint64 sch = self && self->m_host.visibleConnection ? self->m_host.visibleConnection() : 0;
        return sch && ts3::isConnected(sch) && ts3::ownChannel(sch) != 0;
    };
    entry.trigger = [self](QWidget* anchor) {
        if (self)
            self->pick(anchor);
    };
    attachmenu::addEntry(entry);
    g_current = this;
}

MusicController::~MusicController()
{
    shutdown();
    if (g_current == this)
        g_current = nullptr;
}

MusicController* MusicController::current()
{
    return g_current;
}

QString MusicController::problem() const
{
    MusicBar::Problem kind = MusicBar::Problem::None;
    return m_engine ? problemText(&kind) : QString();
}

void MusicController::shutdown()
{
    if (m_shutDown)
        return;
    m_shutDown = true;
    attachmenu::removeEntry(QString::fromLatin1(kEntryId));
    m_tick->stop();
    m_noticeTimer->stop();
    if (m_saveTimer->isActive()) {
        m_saveTimer->stop();
        saveSettings(); // the volume chosen just now
    }
    stopMusic();
    if (m_bar)
        delete m_bar.data(); // out of TeamSpeak's layout; it must not outlive the DLL
    m_bar = nullptr;
}

qint64 MusicController::nowMs() const
{
    if (m_host.clock)
        return m_host.clock();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void MusicController::loadSettings()
{
    if (m_host.settingsFile.isEmpty())
        return;
    QFile file(m_host.settingsFile);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        return;
    const QStringList lines = QString::fromLatin1(file.readAll()).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        bool      ok    = false;
        const int value = line.section(QLatin1Char('='), 1).trimmed().toInt(&ok);
        if (!ok)
            continue;
        if (line.startsWith(QLatin1String("volume=")))
            m_volume = qBound(0, value, 100);
        else if (line.startsWith(QLatin1String("muted=")))
            m_muted = value != 0;
    }
}

void MusicController::saveSettingsSoon()
{
    m_saveTimer->start();
}

void MusicController::saveSettings()
{
    if (m_host.settingsFile.isEmpty())
        return;
    QFile file(m_host.settingsFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QStringLiteral("volume=%1\nmuted=%2\n").arg(m_volume).arg(m_muted ? 1 : 0).toLatin1());
}

float MusicController::gain() const
{
    return m_muted ? 0.0f : music::volumeGain(m_volume);
}

// ---- TeamSpeak ---------------------------------------------------------------------------------------------

MusicController::VoiceState MusicController::readVoiceState(quint64 sch)
{
    VoiceState v;
    v.connected = sch && ts3::isConnected(sch);
    if (!v.connected)
        return v;
    v.channel = ts3::ownChannel(sch);
    int value = 0;
    if (readSelf(sch, CLIENT_INPUT_MUTED, &value))
        v.inputMuted = value != MUTEINPUT_NONE;
    if (readSelf(sch, CLIENT_OUTPUT_MUTED, &value))
        v.outputMuted = value != MUTEOUTPUT_NONE;
    if (readSelf(sch, CLIENT_INPUT_HARDWARE, &value))
        v.hardware = value != 0;
    // Talk power: a talker, or at least what the channel needs (unknown counts as allowed).
    const anyID me     = ts3::ownClientId(sch);
    int         talker = 0, power = 0, needed = 0;
    if (me && v.channel && ts3::funcs.getClientVariableAsInt && ts3::funcs.getChannelVariableAsInt) {
        const bool haveTalker = ts3::funcs.getClientVariableAsInt(sch, me, CLIENT_IS_TALKER, &talker) == ERROR_ok;
        const bool havePower  = ts3::funcs.getClientVariableAsInt(sch, me, CLIENT_TALK_POWER, &power) == ERROR_ok;
        const bool haveNeeded = ts3::funcs.getChannelVariableAsInt(sch, v.channel, CHANNEL_NEEDED_TALK_POWER, &needed) == ERROR_ok;
        if (havePower && haveNeeded && !(haveTalker && talker))
            v.canTalk = power >= needed;
        int codec = -1;
        if (ts3::funcs.getChannelVariableAsInt(sch, v.channel, CHANNEL_CODEC, &codec) == ERROR_ok)
            v.codec = codec;
    }
    return v;
}

bool MusicController::canStart(quint64 sch, QString* why) const
{
    if (!sch || !ts3::isConnected(sch)) {
        *why = i18n::t("Connect to a server to play music to your channel.");
        return false;
    }
    if (!ts3::ownChannel(sch)) {
        *why = i18n::t("Join a channel to play music to it.");
        return false;
    }
    if (!readVoiceState(sch).canTalk) {
        *why = i18n::t("You can't talk in this channel (no talk power), so you can't play music to it either.");
        return false;
    }
    if (!mf::available()) {
        *why = i18n::t("Live music needs Windows Media Foundation. On Windows N editions, install the Media Feature Pack.");
        return false;
    }
    return true;
}

// ---- starting and stopping ---------------------------------------------------------------------------------

void MusicController::pick(QWidget* anchor)
{
    if (m_shutDown)
        return;
    const quint64 sch = m_host.visibleConnection ? m_host.visibleConnection() : 0;
    QString       why;
    if (!canStart(sch, &why)) {
        showNotice(MusicBar::NoticeIcon::Error, why, sch);
        return;
    }
    if (!m_host.pickFile)
        return;
    const QPointer<MusicController> guard(this); // the dialog runs its own event loop
    const QString path = m_host.pickFile(anchor ? anchor->window() : nullptr, m_lastDir);
    if (!guard || m_shutDown || path.isEmpty())
        return;
    m_lastDir = QFileInfo(path).absolutePath();
    start(path);
}

bool MusicController::start(const QString& path)
{
    if (m_shutDown)
        return false;
    const quint64 sch = m_host.visibleConnection ? m_host.visibleConnection() : 0;
    QString       why;
    if (!canStart(sch, &why)) {
        showNotice(MusicBar::NoticeIcon::Error, why, sch);
        return false;
    }
    const QFileInfo file(path);
    if (!file.isFile() || !m_host.source) {
        showNotice(MusicBar::NoticeIcon::Error, i18n::t("That file isn't there any more."), sch);
        return false;
    }
    stopMusic(); // the track before goes
    endNotice();
    m_sch         = sch;
    m_channel     = ts3::ownChannel(sch);
    m_voice       = readVoiceState(sch);
    m_title       = cleanTag(displayFileName(file.completeBaseName()));
    m_artist.clear();
    m_tagsRead    = false;
    m_startedMs   = nowMs();
    m_ticks       = 0;
    m_pttGaveUp   = false;
    m_inputForced = false;
    m_reopens.clear();
    m_vadGaveUp = false;
    m_vadKicks.clear();
    m_home           = true; // (started on the tab in front)
    m_resumeOnReturn = false;
    m_switchFailed   = false;
    m_engine = std::make_unique<music::Engine>(sch, m_host.source(path), m_host.clock);
    m_engine->setGain(gain());
    m_engine->setSendAllowed(!m_voice.inputMuted && !m_voice.outputMuted); // before the callbacks reach it
    m_engine->setMonitorAllowed(!m_voice.outputMuted);
    m_engine->start();
    music::setActive(m_engine.get());
    ++m_diagStarted;
    ts3::log("Live music: started playing a file to the channel", LogLevel_INFO, sch);
    ensureBar();
    updateBar(m_engine->snapshot());
    m_tick->start();
    return true;
}

void MusicController::stopMusic()
{
    if (!m_engine)
        return;
    // 2.3 review: the sides TeamSpeak calls play a 5 ms fade out first (at most kFadeOutWaitMs), while a
    // push-to-talk input is still open: the music never stops mid-wave, a click for everyone.
    m_engine->fadeOut(kFadeOutWaitMs);
    restoreInput();
    music::setActive(nullptr); // no audio callback is inside the engine from here on
    m_engine.reset();          // joins its worker
    m_sch            = 0;
    m_channel        = 0;
    m_home           = true;
    m_resumeOnReturn = false; // stopped: nothing plays on anywhere
    m_switchFailed   = false;
    m_tick->stop();
}

void MusicController::stop()
{
    if (!m_engine)
        return;
    ts3::log("Live music: stopped", LogLevel_INFO, m_sch);
    stopMusic();
    placeBar(); // the bar goes
}

void MusicController::pauseMusic()
{
    // 2.3 review: a push-to-talk input the plugin keeps open closes right after this, so the channel gets
    // the pause's fade out first (otherwise the music stops mid-wave there).
    if (m_inputForced)
        m_engine->fadeOut(kFadeOutWaitMs);
    else
        m_engine->pause();
    restoreInput(); // TeamSpeak's push-to-talk (or its voice activation) is its own again meanwhile
}

void MusicController::togglePause()
{
    if (!m_engine)
        return;
    if (m_engine->snapshot().state == music::Engine::State::Playing)
        pause();
    else
        play();
}

void MusicController::pause()
{
    if (!m_engine)
        return;
    const music::Engine::State state = m_engine->snapshot().state;
    if (state == music::Engine::State::Playing || state == music::Engine::State::Opening)
        pauseMusic(); // (while opening: it opens paused)
    // The user's pause: it stays paused, also back on its tab (2.3 polish review: "pause" on another server tab
    // used to be a toggle there, so it played on and took the user back to the music's tab).
    m_resumeOnReturn = false;
    m_switchFailed   = false;
    refreshNow();
}

void MusicController::play()
{
    if (!m_engine)
        return;
    const music::Engine::State state = m_engine->snapshot().state;
    if (state == music::Engine::State::Paused || state == music::Engine::State::Opening) {
        const quint64 visible = m_host.visibleConnection ? m_host.visibleConnection() : 0;
        if (m_host.visibleConnection && !visible) {
            // 2.3 polish review: no tab in front for a moment (TeamSpeak between tabs): it plays on once its tab is
            // in front (followTab), never on a tab in the background, where it would go on unheard.
            m_home           = false;
            m_resumeOnReturn = true;
            m_switchFailed   = false;
        } else if (visible && visible != m_sch) {
            // 2.3 live test: TeamSpeak sends only from the server tab in front: its tab first, and it plays on
            // as soon as that tab is in front (followTab; TeamSpeak may say so a moment later).
            m_home           = false; // (also before followTab saw that tab go)
            m_resumeOnReturn = true;
            m_switchFailed   = !(m_host.showConnection && m_host.showConnection(m_sch));
            ts3::log(m_switchFailed ? "Live music: its server tab can't be brought to the front; it plays on once that tab is in front"
                                    : "Live music: its server tab brought to the front to play on",
                     LogLevel_INFO, m_sch);
        } else {
            m_resumeOnReturn = false;
            m_switchFailed   = false;
            m_engine->resume();
        }
    }
    refreshNow();
}

void MusicController::seek(qint64 ms)
{
    if (m_engine)
        m_engine->seek(ms);
    refreshNow();
}

void MusicController::setVolume(int percent)
{
    percent = qBound(0, percent, 100);
    if (percent == m_volume && !(m_muted && percent > 0))
        return;
    m_volume = percent;
    if (percent > 0)
        m_muted = false;
    if (m_engine)
        m_engine->setGain(gain());
    saveSettingsSoon();
    refreshNow();
}

void MusicController::toggleMute()
{
    m_muted = !m_muted;
    if (!m_muted && m_volume == 0)
        m_volume = kDefaultVolume;
    if (m_engine)
        m_engine->setGain(gain());
    saveSettingsSoon();
    refreshNow();
}

// ---- TeamSpeak's events ----------------------------------------------------------------------------------

void MusicController::serverTabChanged(quint64 sch)
{
    Q_UNUSED(sch); // the host says which tab is on screen
    refreshNow();  // paused or played on (followTab), the bar full or compact, the input given back elsewhere
}

void MusicController::clientMoved(quint64 sch, quint16 client, quint64 newChannel)
{
    if (!m_engine || sch != m_sch || newChannel == m_channel)
        return;
    const anyID me = ts3::ownClientId(sch);
    if (me && client != me)
        return;
    if (!me && newChannel != 0)
        return;
    ts3::log("Live music: stopped, the own client left the channel", LogLevel_INFO, sch);
    stopMusic();
    showNotice(MusicBar::NoticeIcon::Info,
               newChannel ? i18n::t("The music stopped because you left the channel.") : i18n::t("The music stopped: you left the server."), sch);
}

void MusicController::connectionLost(quint64 sch)
{
    if (!m_engine || sch != m_sch)
        return;
    ts3::log("Live music: stopped, disconnected", LogLevel_INFO, sch);
    stopMusic();
    showNotice(MusicBar::NoticeIcon::Info, i18n::t("The music stopped: you were disconnected from the server."), sch);
}

// ---- the tick ----------------------------------------------------------------------------------------------

void MusicController::refreshNow()
{
    if (m_engine)
        tick();
    else
        placeBar();
}

void MusicController::tick()
{
    if (!m_engine) {
        m_tick->stop();
        placeBar();
        return;
    }
    music::Engine::Snapshot s = m_engine->snapshot();
    if (s.state == music::Engine::State::Failed) {
        const QString title = m_title;
        const quint64 sch   = m_sch;
        ++m_diagFailed;
        ts3::log(LogLevel_WARNING, sch, "Live music: the file can't be played (%1)", {ts3::pub(QStringLiteral("0x%1").arg(static_cast<quint32>(s.code), 8, 16, QLatin1Char('0')))});
        stopMusic();
        showNotice(MusicBar::NoticeIcon::Error, i18n::t("Couldn't play “%1”: %2").arg(title, s.error), sch);
        return;
    }
    if (s.state == music::Engine::State::Ended) {
        ts3::log("Live music: the track ended", LogLevel_INFO, m_sch);
        stopMusic();
        placeBar(); // the bar goes with the music
        return;
    }
    if (!m_tagsRead && s.state != music::Engine::State::Opening) {
        m_tagsRead = true;
        const QString title = cleanTag(s.info.title);
        if (!title.isEmpty())
            m_title = title;
        m_artist = cleanTag(s.info.artist);
    }
    if (m_ticks++ % kCheckTicks == 0 && !checkTeamSpeak())
        return;
    if (followTab(s))
        s = m_engine->snapshot(); // paused or playing on now: the input and the bar follow in this tick
    readMutes(); // every tick: muting in TeamSpeak keeps the music out of the voice within a tick
    keepInputOpen(s);
    updateBar(s);
}

bool MusicController::followTab(const music::Engine::Snapshot& s)
{
    const quint64 visible = m_host.visibleConnection ? m_host.visibleConnection() : 0;
    if (!visible)
        return false; // (between tabs: nothing to tell)
    const bool home = visible == m_sch;
    if (home == m_home)
        return false;
    m_home         = home;
    m_switchFailed = false;
    if (!home) {
        // 2.3 live test: TeamSpeak sends nothing from a server tab in the background, so the track went on
        // unheard there. It waits instead (paused by the user already: it stays so, also back home).
        if (s.state != music::Engine::State::Playing && s.state != music::Engine::State::Opening)
            return false;
        pauseMusic(); // a push-to-talk input given back, voice activation turned on again if it was off
        m_resumeOnReturn = true;
        ++m_diagTabPauses;
        ts3::log("Live music: paused while another server tab is in front", LogLevel_INFO, m_sch);
        return true;
    }
    if (!m_resumeOnReturn)
        return false;
    m_resumeOnReturn = false;
    // A fresh start on its tab: TeamSpeak opened its input again, which says nothing about the give-up rules.
    m_reopens.clear();
    m_vadKicks.clear();
    m_engine->resume();
    ts3::log("Live music: its server tab is in front again; playing on", LogLevel_INFO, m_sch);
    return true;
}

bool MusicController::checkTeamSpeak()
{
    const VoiceState v = readVoiceState(m_sch);
    if (!v.connected) {
        connectionLost(m_sch);
        return false;
    }
    if (v.channel != m_channel) {
        clientMoved(m_sch, ts3::ownClientId(m_sch), v.channel);
        return false;
    }
    m_voice = v;
    return true;
}

void MusicController::keepInputOpen(const music::Engine::Snapshot& s)
{
    const bool visible = m_host.visibleConnection && m_host.visibleConnection() == m_sch;
    const bool want    = visible && s.state == music::Engine::State::Playing && !m_pttGaveUp && !m_voice.inputMuted && !m_voice.outputMuted && m_voice.hardware
                      && m_voice.canTalk;
    if (!want) {
        restoreInput();
        return;
    }
    if (m_vadKicked || voiceActivation(m_sch)) { // 2.3 review: not push-to-talk; the voice goes along with the music
        if (m_inputForced) {      // (switched to voice activation while it played: TeamSpeak's input now)
            m_inputForced = false;
            ts3::log("Live music: voice activation: TeamSpeak's input is its own", LogLevel_INFO, m_sch);
        }
        const int talking = selfTalking(m_sch);
        if (m_vadKicked) { // TeamSpeak sends now, and goes on sending on the music's word: its voice activation back
            if (talking != 0 || nowMs() - m_vadKickAt >= kVadKickMs)
                endVadKick();
            return;
        }
        if (m_engine)
            m_engine->setVoiceAllowed(true);
        // 2.3 live test: music for the voice, but TeamSpeak doesn't send (its voice activation hears silence,
        // and it never starts a transmission on the plugin's word alone).
        if (s.mixing && talking == 0 && !m_vadGaveUp)
            startVadKick();
        return;
    }
    int deactivated = INPUT_ACTIVE;
    if (!readSelf(m_sch, CLIENT_INPUT_DEACTIVATED, &deactivated) || deactivated != INPUT_DEACTIVATED)
        return;
    // TeamSpeak captures nothing while push-to-talk is up: open it for the music (only the music goes out).
    const qint64 now = nowMs();
    m_reopens.append(now);
    while (!m_reopens.isEmpty() && now - m_reopens.first() > kReopenWindowMs)
        m_reopens.removeFirst();
    if (m_reopens.size() > kReopenLimit) { // TeamSpeak keeps closing it: leave it to TeamSpeak
        m_pttGaveUp = true;
        ts3::log("Live music: TeamSpeak keeps deactivating the input; it is no longer kept open", LogLevel_INFO, m_sch);
        restoreInput();
        return;
    }
    // The key can't be seen: never the microphone meanwhile. Said to the engine before the input opens, so
    // no block of TeamSpeak's capture thread goes out with the voice in between.
    if (m_engine)
        m_engine->setVoiceAllowed(false);
    // Opened is what TeamSpeak's variable says now, whatever the flush answered: an input the plugin opened
    // is always given back (restoreInput), and the voice stays out meanwhile.
    int opened = INPUT_DEACTIVATED;
    if (setSelf(m_sch, CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE) || (readSelf(m_sch, CLIENT_INPUT_DEACTIVATED, &opened) && opened == INPUT_ACTIVE)) {
        if (!m_inputForced)
            ts3::log("Live music: TeamSpeak's input kept open for the music (push-to-talk)", LogLevel_INFO, m_sch);
        m_inputForced = true;
        ++m_diagReopened;
    } else if (!m_inputForced && m_engine) {
        m_engine->setVoiceAllowed(true); // not opened: TeamSpeak's push-to-talk as it is
    }
}

void MusicController::startVadKick()
{
    const qint64 now = nowMs();
    if (!m_vadKicks.isEmpty() && now - m_vadKicks.last() < kVadKickGapMs)
        return;
    while (!m_vadKicks.isEmpty() && now - m_vadKicks.first() > kReopenWindowMs)
        m_vadKicks.removeFirst();
    if (m_vadKicks.size() >= kVadKickLimit) { // TeamSpeak never sends: leave it to TeamSpeak
        m_vadGaveUp = true;
        ts3::log("Live music: TeamSpeak doesn't send the music under voice activation; not started for it any more", LogLevel_INFO, m_sch);
        return;
    }
    // The microphone stays out first: with voice activation off TeamSpeak sends whatever it captures.
    if (m_engine)
        m_engine->setVoiceAllowed(false);
    if (!setVoiceActivation(m_sch, false)) {
        if (m_engine)
            m_engine->setVoiceAllowed(true);
        return;
    }
    if (m_vadKicks.isEmpty())
        ts3::log("Live music: voice activation paused for a moment so TeamSpeak sends the music", LogLevel_INFO, m_sch);
    m_vadKicked = true;
    m_vadKickAt = now;
    m_vadKicks.append(now);
    ++m_diagVadKicks;
}

void MusicController::endVadKick()
{
    if (!m_vadKicked)
        return;
    m_vadKicked = false;
    setVoiceActivation(m_sch, true); // the user's voice activation again: TeamSpeak decides for the voice
    if (m_engine)
        m_engine->setVoiceAllowed(true);
}

void MusicController::restoreInput()
{
    endVadKick(); // 2.3 live test: TeamSpeak's voice activation back first
    if (m_inputForced) {
        m_inputForced = false;
        int deactivated = INPUT_DEACTIVATED;
        // (never under voice activation: that input is TeamSpeak's, closing it would silence the user)
        if (m_sch && !voiceActivation(m_sch) && readSelf(m_sch, CLIENT_INPUT_DEACTIVATED, &deactivated) && deactivated == INPUT_ACTIVE)
            setSelf(m_sch, CLIENT_INPUT_DEACTIVATED, INPUT_DEACTIVATED); // push-to-talk is up again
        ts3::log("Live music: TeamSpeak's input given back", LogLevel_INFO, m_sch);
    }
    // Only now the voice may go along again: the input is TeamSpeak's own first (closed while the key is up).
    if (m_engine)
        m_engine->setVoiceAllowed(true);
}

void MusicController::readMutes()
{
    int value = 0;
    if (readSelf(m_sch, CLIENT_INPUT_MUTED, &value))
        m_voice.inputMuted = value != MUTEINPUT_NONE;
    if (readSelf(m_sch, CLIENT_OUTPUT_MUTED, &value))
        m_voice.outputMuted = value != MUTEOUTPUT_NONE;
    if (m_engine) { // a client muted in TeamSpeak (by the user, or a voice message's MicGuard) is never made to send
        m_engine->setSendAllowed(!m_voice.inputMuted && !m_voice.outputMuted);
        m_engine->setMonitorAllowed(!m_voice.outputMuted); // 2.3 review: sound muted, not even here
    }
}

QString MusicController::problemText(MusicBar::Problem* kind) const
{
    *kind = MusicBar::Problem::Warning;
    if (!m_voice.canTalk)
        return i18n::t("You don't have talk power in this channel, so nobody hears the music.");
    if (m_voice.outputMuted)
        return i18n::t("Your sound is muted in TeamSpeak, so nobody hears the music, not even you.");
    if (m_voice.inputMuted)
        return i18n::t("Your microphone is muted in TeamSpeak, so the channel can't hear the music. Unmute it to share.");
    if (!m_voice.hardware)
        return i18n::t("TeamSpeak has no microphone open on this server, so the channel can't hear the music.");
    if (m_pttGaveUp)
        return i18n::t("Push-to-talk: the channel hears the music only while you hold your key.");
    if (m_vadGaveUp)
        return i18n::t("Voice activation: the channel hears the music only while you talk.");
    *kind = MusicBar::Problem::Info;
    if (m_muted || m_volume == 0) // 2.3 live test: not "the channel hears the music" while it is muted
        return i18n::t("The music is muted, so the channel doesn't hear it.");
    if (m_inputForced)
        return i18n::t("Push-to-talk: the channel hears the music; your voice isn't sent while it plays.");
    if (m_voice.codec >= 0 && m_voice.codec != CODEC_OPUS_MUSIC && nowMs() - m_startedMs < kCodecHintMs)
        return i18n::t("This channel uses a voice codec. Music sounds best in channels set to Opus Music.");
    *kind = MusicBar::Problem::None;
    return QString();
}

void MusicController::updateBar(const music::Engine::Snapshot& s)
{
    if (!m_bar || m_notice)
        return placeBar();
    MusicBar::View v;
    switch (s.state) {
    case music::Engine::State::Opening:
        v.mode = MusicBar::Mode::Loading;
        break;
    case music::Engine::State::Paused:
        v.mode = MusicBar::Mode::Paused;
        break;
    default:
        v.mode = MusicBar::Mode::Playing;
        break;
    }
    v.title       = m_title;
    v.artist      = m_artist;
    v.positionMs  = s.positionMs;
    v.durationMs  = s.durationMs;
    v.volume      = m_volume;
    v.muted       = m_muted;
    v.live        = s.mixing;
    v.problem     = problemText(&v.problemKind);
    v.animate     = ui::animationsEnabled();
    // 2.3: on another server tab the compact bar says where it plays (play and stop work from there), and
    // (live test) that it waits there: TeamSpeak sends only from the tab in front.
    const quint64 visible = m_host.visibleConnection ? m_host.visibleConnection() : 0;
    if (visible != m_sch) {
        v.away    = awayLabel();
        v.problem.clear();
        v.problemKind = MusicBar::Problem::None;
        if (m_resumeOnReturn && s.state != music::Engine::State::Playing) {
            v.problem     = m_switchFailed ? i18n::t("Go back to its server tab to play on: TeamSpeak only sends from the tab in front.")
                                           : i18n::t("Paused while you're on another server tab. Play takes you back there.");
            v.problemKind = MusicBar::Problem::Info;
        }
    }
    // The chat above gets shorter or taller with the problem line: one that showed its end still does.
    QTextBrowser* chat   = m_bar->isVisible() ? chatAbove(m_barInput) : nullptr;
    const bool    bottom = atEnd(chat);
    const int     height = m_bar->sizeHint().height();
    m_bar->setView(v);
    placeBar();
    if (chat && bottom && m_bar->isVisible() && m_bar->sizeHint().height() != height)
        keepAtEnd(chat);
}

void MusicController::keepAtEnd(QTextBrowser* chat)
{
    if (QWidget* holder = m_bar ? m_bar->parentWidget() : nullptr; holder && holder->layout() && !m_barOverlay)
        holder->layout()->activate();
    chat->verticalScrollBar()->setValue(chat->verticalScrollBar()->maximum());
    const QPointer<QTextBrowser> guard(chat);
    QTimer::singleShot(0, this, [guard] { // a posted event, freed with this object
        if (guard)
            guard->verticalScrollBar()->setValue(guard->verticalScrollBar()->maximum());
    });
}

QString MusicController::awayLabel() const
{
    // Names come from the server: no controls, bidi marks or line breaks, and short (sanitizeCaption).
    QString label = m_host.placeName ? cleanTag(m_host.placeName(m_sch, m_channel)) : QString();
    if (label.size() > 64) {
        int cut = 63;
        if (label.at(cut - 1).isHighSurrogate())
            --cut; // never half an emoji
        label = label.left(cut) + QChar(0x2026);
    }
    return label.isEmpty() ? i18n::t("another server tab") : label;
}

// ---- the bar ---------------------------------------------------------------------------------------------

QWidget* MusicController::input()
{
    if (m_input && m_input->isVisible())
        return m_input.data();
    m_input = m_host.input ? m_host.input() : nullptr;
    return m_input.data();
}

void MusicController::ensureBar()
{
    if (m_bar)
        return;
    auto* bar = new MusicBar; // goes into TeamSpeak's layout (placeBar); hidden until then
    bar->hide();
    m_bar = bar;
    connect(bar, &MusicBar::playPauseClicked, this, &MusicController::togglePause);
    connect(bar, &MusicBar::stopClicked, this, &MusicController::stop);
    connect(bar, &MusicBar::seekRequested, this, &MusicController::seek);
    connect(bar, &MusicBar::volumeChanged, this, &MusicController::setVolume);
    connect(bar, &MusicBar::muteToggled, this, &MusicController::toggleMute);
    connect(bar, &MusicBar::noticeClosed, this, &MusicController::endNotice);
}

void MusicController::placeBar()
{
    if (!m_bar)
        return;
    const quint64 owner   = m_notice ? m_noticeSch : m_sch;
    const quint64 visible = m_host.visibleConnection ? m_host.visibleConnection() : 0;
    // Every server tab shares the one input. A notice belongs to its tab; the music shows on every tab (2.3:
    // compact on the others, updateBar), so it can always be paused or stopped.
    const bool    shown   = owner && (m_notice ? owner == visible : m_engine != nullptr);
    QWidget*      in      = shown ? input() : nullptr;
    if (!in) {
        if (!m_bar->isHidden()) {
            QTextBrowser* chat   = chatAbove(m_barInput);
            const bool    bottom = atEnd(chat);
            m_bar->hide();
            if (bottom && chat)
                chat->verticalScrollBar()->setValue(chat->verticalScrollBar()->maximum());
        }
        return;
    }
    // TeamSpeak's theme: the input's background and font (as the hold strip and the mic button read it).
    const QPalette pal  = in->palette();
    const QColor   base = pal.color(QPalette::Base);
    m_bar->setTheme(base.lightness() < 128 || pal.color(QPalette::Text).lightness() > 170, base, in->font());
    if (m_barInput != in || !m_bar->parentWidget()) {
        m_barInput   = in;
        m_barOverlay = !inputbars::insertAbove(in, m_bar);
    }
    if (m_barOverlay)
        inputbars::placeOverlay(in, m_bar, m_bar->hasHeightForWidth() ? m_bar->heightForWidth(in->width()) : m_bar->sizeHint().height());
    if (m_bar->isHidden()) {
        QTextBrowser* chat   = chatAbove(in);
        const bool    bottom = atEnd(chat);
        m_bar->show();
        if (QWidget* holder = m_bar->parentWidget(); holder && holder->layout() && !m_barOverlay)
            holder->layout()->activate();
        if (bottom && chat) // the chat got shorter: one that showed its end still does
            keepAtEnd(chat);
    }
}

void MusicController::showNotice(MusicBar::NoticeIcon icon, const QString& text, quint64 sch)
{
    if (m_shutDown)
        return;
    ensureBar();
    m_notice    = true;
    m_noticeSch = sch ? sch : (m_host.visibleConnection ? m_host.visibleConnection() : 0);
    MusicBar::View v;
    v.mode       = MusicBar::Mode::Notice;
    v.notice     = text;
    v.noticeIcon = icon;
    v.animate    = ui::animationsEnabled();
    m_bar->setView(v);
    placeBar();
    const int windows = ui::notificationDurationMs();
    m_noticeTimer->start(qMax(icon == MusicBar::NoticeIcon::Error ? 8000 : 4000, windows > 5000 ? windows : 0));
}

void MusicController::endNotice()
{
    m_noticeTimer->stop();
    if (!m_notice)
        return;
    m_notice    = false;
    m_noticeSch = 0;
    if (m_engine)
        updateBar(m_engine->snapshot());
    else
        placeBar();
}

// ---- diagnostics -----------------------------------------------------------------------------------------

QString MusicController::diagnosticsTitle()
{
    return i18n::t("Live music");
}

QStringList MusicController::diagnosticLines() const
{
    QStringList lines;
    lines << i18n::t("This session: %1 started, %2 couldn't be played").arg(m_diagStarted).arg(m_diagFailed);
    if (!m_engine) {
        lines << i18n::t("Now: nothing playing");
        return lines;
    }
    const music::Engine::Snapshot s = m_engine->snapshot();
    lines << i18n::t("Now: %1, the voice gets it: %2, played here: %3")
                 .arg(s.state == music::Engine::State::Paused ? i18n::t("paused") : i18n::t("playing"), s.mixing ? i18n::t("yes") : i18n::t("no"),
                      s.monitoring ? i18n::t("yes") : i18n::t("no"));
    lines << i18n::t("Push-to-talk input kept open: %1 (opened %2 times); decoder late %3 times")
                 .arg(m_inputForced ? i18n::t("yes") : i18n::t("no"))
                 .arg(m_diagReopened)
                 .arg(s.underruns);
    lines << i18n::t("Voice activation paused to start sending the music: %1 times").arg(m_diagVadKicks); // 2.3 live test
    lines << i18n::t("Paused while another server tab was in front: %1 times; waiting for its tab now: %2") // 2.3 live test
                 .arg(m_diagTabPauses)
                 .arg(m_resumeOnReturn ? i18n::t("yes") : i18n::t("no"));
    return lines;
}
