#pragma once

// 2.2 foundation: SHA-256 of files (the "sha" link parameter) and of small buffers (the preview's "ph").
// QCryptographicHash only, so it works the same in every build; QtCore only (unit-tested). Safe on any
// thread: nothing is shared, nothing outlives a call.

#include <QByteArray>
#include <QString>

#include <atomic>
#include <functional>

namespace hashing {

constexpr qint64 kChunkBytes        = 1 << 20;  // read size
constexpr qint64 kProgressStepBytes = 64 << 20; // progress is reported about this often

// done/total in bytes; called on the hashing thread, every kProgressStepBytes and once at the end.
using Progress = std::function<void(qint64 done, qint64 total)>;

// The 32-byte SHA-256 of the file, or empty when it can't be read completely or *cancel became true
// (checked before every chunk). *error, when given, says why it is empty ("canceled" for a cancel).
QByteArray sha256File(const QString& path, const std::atomic<bool>* cancel = nullptr, const Progress& progress = {}, QString* error = nullptr);

QByteArray sha256(const QByteArray& data);

QString toHex(const QByteArray& digest);       // upper case, like Windows' Get-FileHash
QString toBase64Url(const QByteArray& digest); // without padding, as in links

} // namespace hashing
