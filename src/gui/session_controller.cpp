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
    m_session->close();
    if (m_logger)
        m_logger->detach();
}

} // namespace portside::gui
