#include "updateworker.h"

#include <QMetaObject>

#include <windows.h>

#include "crypto.h"
#include "pecheck.h"
#include "updatepolicy.h"

namespace upd {

namespace {

class Job
{
  public:
    explicit Job(std::shared_ptr<WorkerLink> link)
        : m_link(std::move(link))
    {
    }
    virtual ~Job() = default;
    virtual void run() = 0;

    HMODULE module = nullptr; // the pin on our DLL, released by FreeLibraryAndExitThread

  protected:
    bool canceled() const { return m_link->cancel.load(); }

    // Runs fn on the receiver's thread, unless the receiver is gone.
    template <typename Fn>
    void post(Fn&& fn)
    {
        QMutexLocker lock(&m_link->mutex);
        if (QObject* receiver = m_link->receiver.data())
            QMetaObject::invokeMethod(receiver, std::forward<Fn>(fn), Qt::QueuedConnection);
    }

    std::shared_ptr<WorkerLink> m_link;
};

DWORD WINAPI threadMain(void* parameter)
{
    auto*   job    = static_cast<Job*>(parameter);
    HMODULE module = job->module;
    job->run();
    delete job;
    // Drops the pin and ends the thread without returning into a DLL that may be unloaded now.
    FreeLibraryAndExitThread(module, 0);
}

void* launch(Job* job)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&threadMain), &module)) {
        delete job;
        return nullptr;
    }
    job->module   = module;
    HANDLE thread = CreateThread(nullptr, 0, &threadMain, job, 0, nullptr);
    if (!thread) {
        delete job;
        FreeLibrary(module);
        return nullptr;
    }
    return thread;
}

// ---- check ---------------------------------------------------------------------------------------

class CheckJob : public Job
{
  public:
    CheckJob(std::shared_ptr<WorkerLink> link, QVector<TrustedKey> keys, QSet<int> revoked, std::function<void(const CheckOutcome&)> done)
        : Job(std::move(link))
        , m_keys(std::move(keys))
        , m_revoked(std::move(revoked))
        , m_done(std::move(done))
    {
    }

    void run() override
    {
        CheckOutcome  outcome;
        http::Request request;
        request.url        = manifestUrl();
        request.maxBytes   = kMaxOuterBytes;
        request.deadlineMs = kCheckDeadlineMs;
        request.cancel     = &m_link->cancel;
        const http::Response response = http::get(request);
        outcome.httpError     = response.error;
        outcome.httpStatus    = response.status;
        outcome.retryAfterSec = response.retryAfterSec;
        outcome.winError      = response.winError;
        outcome.host          = response.finalHost;
        if (response.error == http::Error::None)
            outcome.manifestError = readManifest(response.body, m_keys, m_revoked, &outcome.manifest, &outcome.detail);
        auto done = m_done;
        post([done, outcome] { done(outcome); });
    }

  private:
    QVector<TrustedKey>                      m_keys;
    QSet<int>                                m_revoked;
    std::function<void(const CheckOutcome&)> m_done;
};

// ---- prepare -------------------------------------------------------------------------------------

class FileWriter
{
  public:
    explicit FileWriter(const QString& path)
    {
        m_handle = CreateFileW(fs::native(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE)
            m_error = GetLastError();
    }
    ~FileWriter() { close(); }
    FileWriter(const FileWriter&)            = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool ok() const { return m_handle != INVALID_HANDLE_VALUE; }
    bool write(const char* data, qint64 size)
    {
        DWORD written = 0;
        if (!WriteFile(m_handle, data, static_cast<DWORD>(size), &written, nullptr) || written != static_cast<DWORD>(size)) {
            m_error = GetLastError();
            return false;
        }
        m_sha.add(data, static_cast<size_t>(size));
        return true;
    }
    bool close()
    {
        if (m_handle == INVALID_HANDLE_VALUE)
            return true;
        const bool flushed = FlushFileBuffers(m_handle) != 0;
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return flushed;
    }
    QByteArray    digest() { return m_sha.finish(); }
    unsigned long error() const { return m_error; }

  private:
    HANDLE         m_handle = INVALID_HANDLE_VALUE;
    crypto::Sha256 m_sha;
    unsigned long  m_error = 0;
};

class PrepareJob : public Job
{
  public:
    PrepareJob(std::shared_ptr<WorkerLink> link, Layout layout, Manifest manifest, std::function<void(qint64, qint64)> progress,
               std::function<void(const PrepareOutcome&)> done)
        : Job(std::move(link))
        , m_layout(std::move(layout))
        , m_manifest(std::move(manifest))
        , m_progress(std::move(progress))
        , m_done(std::move(done))
    {
    }

    void run() override
    {
        PrepareOutcome outcome = prepare();
        if (!outcome.ok())
            fs::removeTree(m_layout.stagingDir(m_manifest.version));
        auto done = m_done;
        post([done, outcome] { done(outcome); });
    }

  private:
    PrepareOutcome prepare()
    {
        PrepareOutcome outcome;
        const Arch     arch = m_layout.arch;
        QVector<const UpdateFile*> files;
        if (const UpdateFile* own = m_manifest.file(FileKind::Plugin, arch))
            files.append(own);
        if (files.isEmpty()) { // checked before; never install without the running architecture's DLL
            outcome.failure = PrepareOutcome::Failure::WrongMachine;
            return outcome;
        }
        if (const UpdateFile* other = m_manifest.file(FileKind::Plugin, arch == Arch::Win64 ? Arch::Win32 : Arch::Win64))
            files.append(other);
        if (const UpdateFile* helper = m_manifest.file(FileKind::Helper, arch))
            files.append(helper);

        const qint64 total = UpdateWorker::downloadSize(m_manifest, arch);
        fs::ensureDir(m_layout.updateDir);
        const quint64 needed = static_cast<quint64>(total) * 3 + 1024 * 1024;
        const quint64 free   = fs::freeBytes(m_layout.updateDir);
        if (free != 0 && free < needed) {
            outcome.failure     = PrepareOutcome::Failure::DiskSpace;
            outcome.neededBytes = needed;
            return outcome;
        }
        const QString staging = m_layout.stagingDir(m_manifest.version);
        fs::removeTree(staging);
        if (!fs::ensureDir(m_layout.downloadDir()) || !fs::ensureDir(staging)) {
            outcome.failure  = PrepareOutcome::Failure::Disk;
            outcome.winError = GetLastError();
            return outcome;
        }

        qint64 doneBefore = 0;
        for (const UpdateFile* f : files) {
            if (canceled()) {
                outcome.failure = PrepareOutcome::Failure::Canceled;
                return outcome;
            }
            const QString asset = assetName(f->kind, f->arch, m_manifest.version);
            const QString part  = m_layout.downloadDir() + QLatin1Char('/') + asset + QLatin1String(".part");
            outcome.fileName    = asset;
            QByteArray digest;
            qint64     received = 0;
            {
                FileWriter writer(part);
                if (!writer.ok()) {
                    outcome.failure  = PrepareOutcome::Failure::Disk;
                    outcome.winError = writer.error();
                    return outcome;
                }
                http::Request request;
                request.url        = assetUrl(QLatin1Char('v') + m_manifest.version.toString(), asset);
                request.maxBytes   = f->size; // never more than the signed size
                request.deadlineMs = downloadDeadlineMs(f->size);
                request.cancel     = &m_link->cancel;
                request.sink       = [&writer](const char* data, qint64 size) { return writer.write(data, size); };
                request.progress   = [this, doneBefore, total](qint64 bytes) {
                    auto progressFn = m_progress;
                    if (progressFn)
                        post([progressFn, doneBefore, bytes, total] { progressFn(doneBefore + bytes, total); });
                };
                const http::Response response = http::get(request);
                const bool           closed   = writer.close();
                received                      = response.received;
                if (response.error != http::Error::None) {
                    fs::removeFile(part);
                    if (response.error == http::Error::Canceled) {
                        outcome.failure = PrepareOutcome::Failure::Canceled;
                    } else if (response.error == http::Error::Sink || !closed) {
                        outcome.failure  = PrepareOutcome::Failure::Disk;
                        outcome.winError = writer.error();
                    } else if (response.error == http::Error::TooLarge) {
                        outcome.failure = PrepareOutcome::Failure::Mismatch;
                    } else {
                        outcome.failure       = PrepareOutcome::Failure::Network;
                        outcome.httpError     = response.error;
                        outcome.retryAfterSec = response.retryAfterSec;
                        outcome.winError      = response.winError;
                    }
                    return outcome;
                }
                if (!closed) {
                    fs::removeFile(part);
                    outcome.failure = PrepareOutcome::Failure::Disk;
                    return outcome;
                }
                digest = writer.digest();
            }
            if (received != f->size || digest != f->sha256) {
                fs::removeFile(part);
                outcome.failure = PrepareOutcome::Failure::Mismatch;
                return outcome;
            }
            // The signed hash matched; the CPU and file type must match the name too.
            QByteArray image;
            pe::ImageInfo info;
            if (!fs::readAll(part, &image, kMaxFileSize) || !pe::readInfo(image, &info) || info.machine != f->machine
                || info.isDll != (f->kind == FileKind::Plugin)) {
                fs::removeFile(part);
                outcome.failure = PrepareOutcome::Failure::WrongMachine;
                return outcome;
            }
            const QString staged = staging + QLatin1Char('/') + targetFileName(f->kind, f->arch) + QLatin1String(".new");
            unsigned long error  = 0;
            if (!fs::moveFile(part, staged, true, &error)) {
                fs::removeFile(part);
                outcome.failure  = PrepareOutcome::Failure::Disk;
                outcome.winError = error;
                return outcome;
            }
            StagedFile s;
            s.kind   = f->kind;
            s.arch   = f->arch;
            s.path   = staged;
            s.sha256 = f->sha256;
            s.size   = f->size;
            if (f->kind == FileKind::Helper) {
                outcome.hasHelper = true;
                outcome.helper    = s;
            } else {
                outcome.plugins.append(s);
            }
            doneBefore += f->size;
        }

        // Antivirus programs often quarantine new DLLs a moment after they are written.
        for (int waited = 0; waited < UpdateWorker::kQuarantineWaitMs; waited += 100) {
            if (canceled()) {
                outcome.failure = PrepareOutcome::Failure::Canceled;
                return outcome;
            }
            Sleep(100);
        }
        QVector<StagedFile> all = outcome.plugins;
        if (outcome.hasHelper)
            all.append(outcome.helper);
        for (const StagedFile& s : all) {
            qint64 size = -1;
            if (fs::sha256(s.path, &size, kMaxFileSize) != s.sha256 || size != s.size) {
                outcome.failure  = PrepareOutcome::Failure::Quarantined;
                outcome.fileName = assetName(s.kind, s.arch, m_manifest.version);
                return outcome;
            }
        }

        // Would TeamSpeak be able to load the new DLL? Every import must resolve in this process.
        QByteArray                 image;
        QVector<pe::ImportedModule> imports;
        if (!fs::readAll(outcome.plugins.first().path, &image, kMaxFileSize) || !pe::readImports(image, &imports)) {
            outcome.failure = PrepareOutcome::Failure::WrongMachine;
            return outcome;
        }
        outcome.missingImports = pe::unresolvedImports(imports);
        if (!outcome.missingImports.isEmpty()) {
            outcome.failure  = PrepareOutcome::Failure::Imports;
            outcome.fileName = targetFileName(FileKind::Plugin, arch);
            return outcome;
        }
        outcome.fileName.clear();
        return outcome;
    }

    Layout                                     m_layout;
    Manifest                                   m_manifest;
    std::function<void(qint64, qint64)>        m_progress;
    std::function<void(const PrepareOutcome&)> m_done;
};

} // namespace

void* UpdateWorker::startCheck(const std::shared_ptr<WorkerLink>& link, const QVector<TrustedKey>& keys, const QSet<int>& revoked,
                               std::function<void(const CheckOutcome&)> done)
{
    return launch(new CheckJob(link, keys, revoked, std::move(done)));
}

void* UpdateWorker::startPrepare(const std::shared_ptr<WorkerLink>& link, const Layout& layout, const Manifest& manifest,
                                 std::function<void(qint64, qint64)> progress, std::function<void(const PrepareOutcome&)> done)
{
    return launch(new PrepareJob(link, layout, manifest, std::move(progress), std::move(done)));
}

qint64 UpdateWorker::downloadSize(const Manifest& manifest, Arch arch)
{
    qint64 total = 0;
    for (const UpdateFile& f : manifest.files) {
        if (f.kind == FileKind::Plugin || (f.kind == FileKind::Helper && f.arch == arch))
            total += f.size;
    }
    return total;
}

} // namespace upd
