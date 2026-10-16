#pragma once

// One session, with everything the GUI layers on top of it (PRD F3, F4, F5,
// F12, F13, F15, F22): the profile it was opened with, its line discipline,
// paced paste and the session log.
//
// It owns the Session; widgets talk to this class and to the Session's
// signals, never to a transport.

#include "core/pacing.h"
#include "core/profiles.h"
#include "core/session.h"
#include "core/session_log.h"

#include <QObject>
#include <QPointer>

#include <memory>

namespace portside::gui {

class SessionController : public QObject
{
    Q_OBJECT

public:
    SessionController(std::unique_ptr<Session> session, Profile profile,
                      std::unique_ptr<SessionLogger> logger = {}, QObject *parent = nullptr);
    ~SessionController() override;

    Session *session() const { return m_session.get(); }
    QString port() const { return m_session->port(); }
    SessionState state() const { return m_session->state(); }
    bool isConnected() const { return m_session->isConnected(); }
    std::optional<DeviceIdentity> identity() const { return m_session->identity(); }
    SerialSettings settings() const { return m_session->settings(); }

    const Profile &profile() const { return m_profile; }
    // Adopt edited settings without dropping the session (F3).
    void setProfile(const Profile &profile);
    const TextPolicy &textPolicy() const { return m_profile.text; }
    void setTextPolicy(const TextPolicy &policy) { m_profile.text = policy; }
    const PacePolicy &pace() const { return m_profile.pacing; }

    // -- actions -----------------------------------------------------------

    QFuture<qint64> send(const QByteArray &data) { return m_session->send(data); }
    QFuture<BreakResult> sendBreak() { return m_session->sendBreak(m_profile.breakDuration); }

    // Paste `text` at the profile's pace (F13); a paste in progress is
    // cancelled first.
    void pasteText(const QString &text);
    // Paste a file; a file that cannot be read is reported, not thrown.
    bool pasteFile(const QString &path);
    void cancelPaste();
    bool isPasting() const;

    // -- logging (F15) -----------------------------------------------------

    bool isLogging() const { return m_logger && m_logger->isLogging(); }
    // Begin logging; returns the path.  Throws PortsideError.
    QString startLogging();
    void stopLogging();
    SessionLogger *logger() const { return m_logger.get(); }

    // Close the session and the log.  Idempotent.
    void close();

Q_SIGNALS:
    void pasteProgress(int line, int total);
    void pasteFinished(const portside::PasteResult &result);
    // Session errors, plus the controller's own (paste and file problems).
    void errorRaised(const portside::PortsideError &error, bool fatal);

private:
    std::unique_ptr<Session> m_session;
    Profile m_profile;
    std::unique_ptr<SessionLogger> m_logger;
    QPointer<PasteJob> m_paste;
};

} // namespace portside::gui
