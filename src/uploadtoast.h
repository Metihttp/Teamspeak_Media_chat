#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QPointer>

#include "core.h"

class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

// Small overlay in the bottom corner of the chat that shows running uploads. One row per file, at
// most a few of them (the rest are summed up in one "+N more" line), and for several files a header
// with the overall progress and "Cancel all". Follows the chat's light or dark theme.
class UploadToast : public QFrame
{
    Q_OBJECT

  public:
    UploadToast(Core* core, QWidget* host);

    void setHost(QWidget* host);
    void setDark(bool dark); // the theme of the chat it is shown in (PreviewStyle::dark)
    void updateJob(int id);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    class ProgressLine;
    class GlyphButton;
    class ElidedLabel;
    class AlertGlyph;

    // Transfer rate in bytes per second, smoothed so that the time left settles within ~2 s.
    struct Speed {
        qint64 startMs   = -1;
        qint64 lastMs    = -1;
        double lastBytes = 0.0;
        double rate      = -1.0; // < 0: no estimate yet

        void   reset() { *this = Speed(); }
        void   sample(qint64 nowMs, double bytes);
        bool   hasRate(qint64 nowMs) const;                    // after a short warm-up
        qint64 timeLeftMs(qint64 nowMs, double remaining) const; // -1: not worth showing
    };

    struct Row {
        QWidget*      widget  = nullptr;
        ElidedLabel*  title   = nullptr;
        QLabel*       percent = nullptr; // "45%" while uploading
        QToolButton*  retry   = nullptr; // failed jobs that can be sent again
        GlyphButton*  close   = nullptr; // cancels while running, dismisses afterwards
        ProgressLine* bar     = nullptr;
        AlertGlyph*   alert   = nullptr; // in front of the error text of a failed job
        ElidedLabel*  status  = nullptr;
        QTimer*       removal = nullptr; // finished rows go away after a while, never while hovered
        UploadState   state   = UploadState::Preparing;
        bool          waiting = false;
        bool          known   = false; // state and waiting were set at least once
        int           spokenStep = -1; // last quarter of the upload given to screen readers
        QString       styleState;      // the status label's "state" style property
        Speed         speed;
    };

    // What the header counts: every job since the toast was last idle, also those already removed.
    struct Tally {
        quint64     size  = 0;
        quint64     sent  = 0;
        UploadState state = UploadState::Preparing;
        bool        held  = false; // uploaded, its message waiting for earlier files: can still be canceled
    };

    Row& rowFor(int id);
    void fillRow(int id, Row& row, const UploadJob& job);
    void removeRow(int id, bool dismiss);
    void cancelAll();
    void relayout();
    void updateHeader();
    int  rowLimit(bool expanded) const;
    void reposition();
    int  toastWidth() const;
    bool pointerInside() const;
    void applyTheme();

    Core*             m_core;
    QPointer<QWidget> m_host;
    QVBoxLayout*      m_layout       = nullptr;
    QVBoxLayout*      m_rowsLayout   = nullptr;
    QWidget*          m_header       = nullptr;
    ElidedLabel*      m_headerTitle  = nullptr;
    ElidedLabel*      m_headerStatus = nullptr;
    QToolButton*      m_cancelAll    = nullptr;
    ProgressLine*     m_headerBar    = nullptr;
    QFrame*           m_divider      = nullptr; // between the header and the rows
    QToolButton*      m_more         = nullptr;
    QHash<int, Row>   m_rows;
    QHash<int, Tally> m_tally;
    QList<int>        m_order; // m_rows in the order they are laid out
    Speed             m_totalSpeed;
    QElapsedTimer     m_clock;
    bool              m_dark     = true;
    bool              m_themed   = false;
    bool              m_expanded = false; // "+N more" was clicked: as many rows as fit
};
