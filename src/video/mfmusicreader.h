#pragma once

// 2.3 live music: a music file decoded to what TeamSpeak's voice takes: 48 kHz, interleaved stereo, 32-bit
// float. Media Foundation's source reader decodes (MP3, AAC / M4A, FLAC, WAV, WMA; Ogg and Opus with the
// Store's Web Media Extensions), resamples and mixes down. Synchronous, for one worker thread that holds a
// ThreadScope (COM and Media Foundation for that thread). Nothing is played here.

#include <QString>

#include <memory>
#include <vector>

namespace mf {

struct MusicInfo {
    QString title;  // the file's tags as they are (untrusted: sanitize before showing); empty if none
    QString artist;
    qint64  durationMs     = 0; // 0 if unknown
    int     sourceRate     = 0; // the file's own sound, for the log
    int     sourceChannels = 0;
};

// COM (multithreaded) and Media Foundation started on the calling thread while it exists.
class ThreadScope
{
  public:
    ThreadScope();
    ~ThreadScope();
    ThreadScope(const ThreadScope&)            = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;
    bool ok() const; // false: Media Foundation can't be used here (the reader's open() says so)

  private:
    struct Data;
    std::unique_ptr<Data> d;
};

class MusicReader
{
  public:
    static constexpr int kRate     = 48000;
    static constexpr int kChannels = 2;

    MusicReader();
    ~MusicReader();
    MusicReader(const MusicReader&)            = delete;
    MusicReader& operator=(const MusicReader&) = delete;

    // Opens path's first sound stream for decoding. False: error() says why, for the user.
    bool             open(const QString& path);
    const MusicInfo& info() const { return m_info; }

    enum class Read { Data, End, Error };
    // The next decoded piece appended to out (interleaved stereo frames); *first is the frame it starts at
    // (from the stream's time stamp; -1 if it has none). Data may append nothing (a gap in the stream).
    Read read(std::vector<float>* out, qint64* first);
    // The next read() starts at frame or a little before it (the caller skips to frame by *first).
    bool seek(qint64 frame);

    QString error() const { return m_error; } // for the user; empty while fine
    long    code() const { return m_code; }   // the failed call's HRESULT

  private:
    bool fail(long hr, const QString& path = QString());

    struct Data;
    std::unique_ptr<Data> d;
    MusicInfo             m_info;
    QString               m_error;
    long                  m_code = 0;
};

} // namespace mf
