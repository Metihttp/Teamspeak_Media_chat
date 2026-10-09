#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QPointer>

class Core;
class QLabel;
class QToolButton;
class QVBoxLayout;

// Small overlay in the bottom corner of the chat that shows running uploads.
class UploadToast : public QFrame
{
    Q_OBJECT

  public:
    UploadToast(Core* core, QWidget* host);

    void setHost(QWidget* host);
    void updateJob(int id);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    class ProgressLine;

    struct Row {
        QWidget*      widget = nullptr;
        QLabel*       title  = nullptr;
        QLabel*       status = nullptr;
        ProgressLine* bar    = nullptr;
        QToolButton*  close  = nullptr; // cancels while running, dismisses afterwards
        qint64        uploadStartMs       = -1; // for the speed estimate
        double        uploadStartProgress = 0.0;
        bool          removalScheduled    = false;
    };

    Row& rowFor(int id);
    void removeRow(int id);
    void reposition();
    int  toastWidth() const;

    Core*             m_core;
    QPointer<QWidget> m_host;
    QVBoxLayout*      m_layout;
    QHash<int, Row>   m_rows;
    QElapsedTimer     m_clock;
};
