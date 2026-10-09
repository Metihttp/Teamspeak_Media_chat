#pragma once

#include <QObject>
#include <QTimer>

#include <utility>

// Runs fn once after ms milliseconds, unless owner is deleted first. Use it instead of
// QTimer::singleShot(ms > 0, context, functor): Qt 5.15 parents that timer to the event dispatcher,
// not to the context, so deleting the context neither stops it nor frees the functor. The functor is
// destroyed later (when the timer fires, or when QApplication goes) through this DLL's code, which
// crashes TeamSpeak once the plugin is unloaded. This timer is a child of owner: plugin shutdown,
// which deletes Core and ChatIntegration, frees it while the DLL is still loaded.
// (A zero delay is fine with QTimer::singleShot: that is a posted event, freed with its receiver.)
template <class Fn>
QTimer* singleShotOwned(int ms, QObject* owner, Fn fn)
{
    auto* timer = new QTimer(owner);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, owner, [timer, fn = std::move(fn)]() mutable {
        timer->deleteLater();
        fn();
    });
    timer->start(ms);
    return timer;
}
