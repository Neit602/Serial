#include "gui/session_controller.h"

#include <QFile>

namespace portside::gui {

SessionController::SessionController(std::unique_ptr<Session> session, Profile profile,
                                     std::unique_ptr<SessionLogger> logger, QObject *parent)
    : QObject(parent)
    , m_session(std::move(session))
    , m_profile(std::move(profile))
    , m_logger(std::move(logger))
{
    m_session->setParent(nullptr); // owned here, not by a QObject parent
    connect(m_session.get(), &Session::errorRaised, this, &SessionController::errorRaised);
    connect(m_session.get(), &Session::dataReceived, this, &SessionController::detectZmodem);
    if (m_logger) {
        m_logger->setPort(m_session->port());
        m_logger->attach(m_session.get());
    }
}

SessionController::~SessionController()
{
    close();
}

void SessionController::setProfile(const Profile &profile)
{
    m_profile = profile;
    m_session->setBreakMode(profile.breakMode);
    if (m_logger && !m_logger->isLogging())
        m_logger->setSettings(profile.logging);
}

void SessionController::pasteText(const QString &text)
{
    cancelPaste();
    auto *job = new PasteJob(m_session.get(), text, m_profile.pacing, m_profile.text, this);
    connect(job, &PasteJob::progress, this,
            [this](const PasteProgress &progress) { Q_EMIT pasteProgress(progress.line, progress.totalLines); });
    connect(job, &PasteJob::finished, this, [this, job](const PasteResult &result) {
        if (m_logger && m_logger->isLogging())
            m_logger->writeNote(QStringLiteral("paste %1").arg(result.describe()));
        if (result.error)
            Q_EMIT errorRaised(*result.error, false);
        Q_EMIT pasteFinished(result);
        job->deleteLater();
    });
    m_paste = job;
    try {
        job->start();
    } catch (const PortsideError &error) {
        m_paste = nullptr;
        job->deleteLater();
        Q_EMIT errorRaised(error, false);
    }
}

bool SessionController::pasteFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT errorRaised(PortsideError(ErrorKind::Generic,
                                         QStringLiteral("cannot read %1: %2").arg(path, file.errorString()),
                                         port()),
                           false);
        return false;
    }
    pasteText(QString::fromUtf8(file.readAll()));
    return true;
}

void SessionController::cancelPaste()
{
    if (m_paste)
        m_paste->cancel();
    m_paste = nullptr;
}

bool SessionController::isPasting() const
{
    return m_paste && m_paste->isRunning();
}

// -- file transfers ------------------------------------------------------------

TransferOptions SessionController::transferOptions(std::optional<Protocol> protocol) const
{
    TransferOptions options;
    const TransferDefaults &defaults = m_profile.transfer;
    options.protocol = protocol.value_or(defaults.protocol);
    options.directory = defaults.directory.isEmpty() ? defaultDownloadDirectory() : defaults.directory;
    options.overwrite = defaults.overwrite;
    return options;
}

bool SessionController::startTransfer(TransferDirection direction, const QStringList &files,
                                      const TransferOptions &options)
{
    if (m_transfer) {
        Q_EMIT errorRaised(sessionError(QStringLiteral("a file transfer is already running"), port()),
                           false);
        return false;
    }
    cancelPaste();
    auto *job = new TransferJob(m_session.get(), direction, files, options, this);
    connect(job, &TransferJob::progressed, this, &SessionController::transferProgress);
    connect(job, &TransferJob::finished, this, [this](const TransferResult &result) {
        if (m_logger && m_logger->isLogging())
            m_logger->writeNote(result.summary());
        endTransfer();
        Q_EMIT transferFinished(result);
    });
    connect(job, &TransferJob::failed, this, [this](const PortsideError &error) {
        if (m_logger && m_logger->isLogging())
            m_logger->writeNote(QStringLiteral("transfer failed: %1").arg(error.text()));
        endTransfer();
        Q_EMIT transferFailed(error);
        if (error.kind() != ErrorKind::Cancelled)
            Q_EMIT errorRaised(error, false);
    });
    try {
        job->start();
    } catch (const PortsideError &error) {
        delete job;
        Q_EMIT errorRaised(error, false);
        return false;
    }
    m_transfer = job;
    if (m_logger && m_logger->isLogging()) {
        const QString what = direction == TransferDirection::Send
                                 ? QStringLiteral("send %1").arg(files.join(QStringLiteral(", ")))
                                 : QStringLiteral("receive into %1").arg(options.directory);
        m_logger->writeNote(QStringLiteral("%1 %2").arg(protocolTitle(options.protocol), what));
    }
    Q_EMIT transferStarted();
    return true;
}

void SessionController::cancelTransfer()
{
    if (m_transfer)
        m_transfer->cancel();
}

void SessionController::endTransfer()
{
    if (m_transfer)
        m_transfer->deleteLater();
    m_transfer = nullptr;
    m_detector.reset();
    m_sinceTransfer.start();
}

void SessionController::detectZmodem(const QByteArray &data)
{
    if (!m_profile.transfer.zmodemAutoStart || m_transfer)
        return;
    const auto event = m_detector.feed(data);
    if (event == transfer::zmodem::StartDetector::Event::None)
        return;
    if (m_sinceTransfer.isValid() && m_sinceTransfer.elapsed() < 5000)
        return;
    Q_EMIT zmodemRequested(event == transfer::zmodem::StartDetector::Event::SenderReady);
}

QString SessionController::startLogging()
{
    if (!m_logger) {
        m_logger = std::make_unique<SessionLogger>(m_profile.logging, port(), m_profile.name);
        m_logger->attach(m_session.get());
    }
    m_logger->setPort(port());
    return m_logger->start();
}

void SessionController::stopLogging()
{
    if (m_logger)
        m_logger->stop();
}

void SessionController::close()
{
    cancelPaste();
    // The engine stops (and gives the stream back) before the port closes.
    delete m_transfer;
    m_transfer = nullptr;
    m_session->close();
    if (m_logger)
        m_logger->detach();
}

} // namespace portside::gui
