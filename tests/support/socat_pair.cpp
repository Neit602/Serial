#include "support/socat_pair.h"

#include "support/support.h"

#include <QFileInfo>
#include <QStandardPaths>

namespace portside::test {

SocatPair::SocatPair() = default;

SocatPair::~SocatPair()
{
    stop();
}

bool SocatPair::available()
{
#if defined(Q_OS_WIN)
    return false;
#else
    return !QStandardPaths::findExecutable(QStringLiteral("socat")).isEmpty();
#endif
}

bool SocatPair::start()
{
    stop();
    m_process = std::make_unique<QProcess>();
    m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    const QString end = QStringLiteral("pty,raw,echo=0,link=%1");
    m_process->start(QStringLiteral("socat"), {end.arg(left()), end.arg(right())});
    if (!m_process->waitForStarted(5000))
        return false;
    return waitFor([this] { return QFileInfo::exists(left()) && QFileInfo::exists(right()); }, 5000);
}

void SocatPair::stop()
{
    if (!m_process)
        return;
    m_process->terminate();
    if (!m_process->waitForFinished(3000)) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    m_process.reset();
    waitFor([this] { return !QFileInfo::exists(left()) && !QFileInfo::exists(right()); }, 3000);
}

} // namespace portside::test
