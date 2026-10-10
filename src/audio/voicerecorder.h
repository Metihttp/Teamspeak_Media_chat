#pragma once

// 2.2 voice: records a voice message on a worker thread and encodes it. GUI-thread API.
//
//   start(factory)  Opening -> Recording        the backend is created and opened on the worker
//   stop()          Recording -> Captured       the sound stays in memory (16-bit mono)
//   encode(path)    Captured -> Encoding -> Encoded, or EncodeFailed (the sound is kept: encode again)
//   cancel()        anything -> Idle            stops and joins the worker, forgets the sound
//
// The capture ends on its own at maxMs (limitReached()), when the device goes away (deviceLost(), the
// sound so far is kept) or on an error (CaptureFailed). Nothing is sent across threads with queued
// functors: the worker only writes atomics and mutex-guarded fields, and a child QTimer polls them
// (every 33 ms while the worker runs) and emits stateChanged() on the GUI thread. The destructor
// cancels and joins, so nothing of this DLL runs after it returns.

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "audio/capture.h"

class QTimer;

namespace voice {

class VoiceRecorder : public QObject
{
    Q_OBJECT

  public:
    enum class State { Idle, Opening, Recording, Captured, Encoding, Encoded, CaptureFailed, EncodeFailed };

    // Called on the worker thread (COM is initialized there): the backend to record from.
    using BackendFactory = std::function<std::unique_ptr<CaptureBackend>()>;

    explicit VoiceRecorder(QObject* parent = nullptr);
    ~VoiceRecorder() override;

    void start(BackendFactory factory, qint64 maxMs);
    void stop();
    void encode(const QString& path);
    void cancel();

    State state() const { return m_state.load(); }

    // While and after recording (any state but Idle).
    qint64         elapsedMs() const { return m_elapsedMs.load(); } // sound captured so far
    float          levelDb() const { return m_levelDb.load(); }     // meter: instant attack, 300 ms release
    float          loudestDb() const { return m_loudestDb.load(); } // the loudest bin so far
    int            binCount() const;
    QVector<float> bins(int from = 0) const; // 50 ms peak bins (dBFS) from index `from`
    CaptureFormat  format() const;
    QString        deviceName() const;
    bool           limitReached() const { return m_limitReached.load(); }
    bool           deviceLost() const { return m_deviceLost.load(); }
    CaptureResult  captureError() const; // CaptureFailed (and deviceLost)

    // After stop: the link's waveform (MediaLink::kWaveformLevels levels of 0..15).
    QByteArray waveformLevels() const;

    // Encoding, and after it until the next start or encode: how much of the sound the encoder has
    // written, in ms (it stops where it was when canceled).
    qint64 encodedMs() const { return m_encodedMs.load(); }

    // Encoded / EncodeFailed.
    QString   encodedPath() const;
    qint64    encodedBytes() const;
    QString   encodeError() const; // for the log
    long      encodeCode() const;

  signals:
    void stateChanged(); // state() changed (GUI thread)
    void tick();         // every 33 ms while recording: elapsed time and levels moved on

  private:
    void joinWorker();
    void poll();
    void setState(State state);
    void captureLoop(BackendFactory factory, qint64 maxMs);
    void encodeRun(QString path);

    std::thread       m_worker;
    std::atomic<bool> m_workerDone{true};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_cancel{false};
    QTimer*           m_poll = nullptr;
    State             m_reported = State::Idle; // last state stateChanged() was emitted for

    std::atomic<State>  m_state{State::Idle};
    std::atomic<qint64> m_elapsedMs{0};
    std::atomic<qint64> m_encodedMs{0};
    std::atomic<float>  m_levelDb{-96.0f};
    std::atomic<float>  m_loudestDb{-96.0f};
    std::atomic<bool>   m_limitReached{false};
    std::atomic<bool>   m_deviceLost{false};

    mutable QMutex       m_mutex; // the fields below
    std::vector<int16_t> m_pcm;   // the recording (written by the capture worker only; read after it ended)
    QVector<float>       m_bins;
    CaptureFormat        m_format;
    QString              m_deviceName;
    CaptureResult        m_captureError;
    QString              m_encodedPath;
    qint64               m_encodedBytes = 0;
    QString              m_encodeError;
    long                 m_encodeCode   = 0;

    std::shared_ptr<CaptureBackend> m_backend; // set by the worker while it records (for interrupt())
};

} // namespace voice
