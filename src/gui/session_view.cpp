#include "gui/session_view.h"

#include "gui/keymap.h"
#include "gui/session_controller.h"
#include "gui/signals_panel.h"
#include "gui/terminal_widget.h"
#include "gui/transfer_dialog.h"

#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace portside::gui {

namespace {
// The status line is refreshed at most this often while data flows.
constexpr int kStatusIntervalMs = 100;
} // namespace

SessionView::SessionView(SessionController *controller, ThemeName theme, QWidget *parent)
    : QWidget(parent)
    , m_controller(controller)
{
    m_controller->setParent(this);
    const TerminalOptions &options = controller->profile().terminal;
    m_terminal = std::make_unique<Terminal>(options.columns, options.rows, options.scrollback,
                                            options.charset, options.stripHighBit);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_view = new TerminalWidget(m_terminal.get(), theme, options.fontFamily, options.fontSize, this);
    m_view->setSelectToCopy(options.selectToCopy);
    connect(m_view, &TerminalWidget::keyPressed, this, &SessionView::onKey);
    layout->addWidget(m_view, 1);

    m_findBar = buildFindBar();
    m_findBar->setVisible(false);
    layout->addWidget(m_findBar);

    m_pasteProgress = new QProgressBar(this);
    m_pasteProgress->setVisible(false);
    m_pasteProgress->setAccessibleName(tr("Paste progress"));
    layout->addWidget(m_pasteProgress);

    m_transferPanel = buildTransferPanel();
    m_transferPanel->setVisible(false);
    layout->addWidget(m_transferPanel);

    auto *bottom = new QWidget(this);
    auto *bottomLayout = new QHBoxLayout(bottom);
    bottomLayout->setContentsMargins(4, 2, 4, 2);
    m_signals = new SignalsPanel(bottom);
    bottomLayout->addWidget(m_signals);
    m_statusLabel = new QLabel(bottom);
    m_statusLabel->setAccessibleName(tr("Session status"));
    bottomLayout->addWidget(m_statusLabel, 1, Qt::AlignRight);
    layout->addWidget(bottom);

    Session *session = controller->session();
    connect(session, &Session::dataReceived, this, &SessionView::onData);
    connect(session, &Session::stateChanged, this,
            [this](SessionState, SessionState state, const QString &detail) {
                m_notice = enumName(state) + (detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail);
                m_signals->setEnabledForSession(state == SessionState::Connected);
                updateStatus();
            });
    connect(session, &Session::reconnectAttempt, this, [this](int attempt, double delay, const QString &) {
        m_notice = tr("reconnecting (attempt %1, next try in %2s)").arg(attempt).arg(delay, 0, 'f', 1);
        updateStatus();
    });
    connect(controller, &SessionController::errorRaised, this,
            [this](const PortsideError &error, bool fatal) {
                // A device that is away while auto-reconnect waits for it is
                // news for the status line, not for a dialog per attempt.
                const bool waiting = !fatal
                                     && (error.kind() == ErrorKind::DeviceVanished
                                         || error.kind() == ErrorKind::PortNotFound);
                if (fatal || waiting) {
                    m_notice = error.text();
                    updateStatus();
                }
                if (!waiting)
                    Q_EMIT errorRaised(error);
            });
    connect(controller, &SessionController::pasteProgress, this, [this](int line, int total) {
        m_pasteProgress->setVisible(true);
        m_pasteProgress->setMaximum(std::max(1, total));
        m_pasteProgress->setValue(line);
        m_pasteNote = tr("paste %1/%2").arg(line).arg(total);
        updateStatus();
    });
    connect(controller, &SessionController::pasteFinished, this, [this](const PasteResult &result) {
        m_pasteProgress->setVisible(false);
        m_pasteNote.clear();
        m_notice = result.describe();
        updateStatus();
    });

    connect(controller, &SessionController::transferStarted, this, [this] {
        const TransferJob *job = m_controller->transfer();
        const bool sending = job->direction() == TransferDirection::Send;
        m_transferTitle->setText(sending ? tr("Sending with %1…").arg(protocolTitle(job->options().protocol))
                                         : tr("Receiving with %1 into %2…")
                                               .arg(protocolTitle(job->options().protocol),
                                                    QDir::toNativeSeparators(job->options().directory)));
        m_transferStats->setText(transferHint(job->options().protocol, job->direction()));
        m_transferFileBar->setRange(0, 0);
        m_transferTotalBar->setVisible(false);
        m_transferPanel->setVisible(true);
        m_transferNote = sending ? tr("sending") : tr("receiving");
        updateStatus();
        Q_EMIT transferStateChanged();
    });
    connect(controller, &SessionController::transferProgress, this, &SessionView::showTransferProgress);
    connect(controller, &SessionController::transferFinished, this, [this](const TransferResult &result) {
        m_transferPanel->setVisible(false);
        m_transferNote.clear();
        m_notice = result.summary();
        updateStatus();
        Q_EMIT transferStateChanged();
    });
    connect(controller, &SessionController::transferFailed, this, [this](const PortsideError &error) {
        m_transferPanel->setVisible(false);
        m_transferNote.clear();
        m_notice = error.kind() == ErrorKind::Cancelled ? tr("transfer cancelled")
                                                        : tr("transfer failed: %1").arg(error.message());
        updateStatus();
        Q_EMIT transferStateChanged();
    });
    connect(controller, &SessionController::zmodemRequested, this, &SessionView::zmodemRequested);

    connect(m_signals, &SignalsPanel::dtrToggled, this, [this](bool value) {
        m_controller->session()->setDtr(value).then(this, [this](QFuture<bool> done) {
            try {
                if (!done.result())
                    m_signals->setControlLinesSupported(false);
            } catch (const std::exception &) {
            }
        });
    });
    connect(m_signals, &SignalsPanel::rtsToggled, this, [this](bool value) {
        m_controller->session()->setRts(value).then(this, [this](QFuture<bool> done) {
            try {
                if (!done.result())
                    m_signals->setControlLinesSupported(false);
            } catch (const std::exception &) {
            }
        });
    });
    m_signals->setEnabledForSession(controller->isConnected());
    m_signals->startPolling([this] { pollSignals(); });
    // Read the lines once now, rather than showing "unknown" for a second.
    QTimer::singleShot(0, this, &SessionView::pollSignals);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    connect(m_statusTimer, &QTimer::timeout, this, [this] {
        if (m_statusDirty)
            updateStatus();
    });
    m_statusTimer->start();
    updateStatus();
}

SessionView::~SessionView()
{
    closeSession();
}

// -- find bar -----------------------------------------------------------------

QWidget *SessionView::buildFindBar()
{
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(4, 2, 4, 2);
    m_findInput = new QLineEdit(bar);
    m_findInput->setPlaceholderText(tr("Find in scrollback"));
    m_findInput->setAccessibleName(tr("Find"));
    connect(m_findInput, &QLineEdit::returnPressed, this, &SessionView::findNext);
    connect(m_findInput, &QLineEdit::textChanged, this, &SessionView::runFind);
    layout->addWidget(m_findInput, 1);
    const std::pair<QString, void (SessionView::*)()> buttons[] = {
        {tr("Previous"), &SessionView::findPrevious},
        {tr("Next"), &SessionView::findNext},
        {tr("Close"), &SessionView::closeFind},
    };
    for (const auto &[text, slot] : buttons) {
        auto *button = new QPushButton(text, bar);
        button->setAccessibleName(text);
        connect(button, &QPushButton::clicked, this, slot);
        layout->addWidget(button);
    }
    m_findStatus = new QLabel(bar);
    layout->addWidget(m_findStatus);
    return bar;
}

void SessionView::openFind()
{
    m_findBar->setVisible(true);
    m_findInput->setFocus();
    m_findInput->selectAll();
}

void SessionView::closeFind()
{
    m_findBar->setVisible(false);
    m_matches.clear();
    m_currentMatch.reset();
    m_view->setMatches({});
    m_view->setFocus();
}

void SessionView::runFind(const QString &query)
{
    if (query.isEmpty()) {
        m_matches.clear();
        m_view->setMatches({});
        m_findStatus->setText(QString());
        return;
    }
    QList<Match> matches;
    try {
        matches = findAll(m_terminal->allTextLines(), query);
    } catch (const PortsideError &error) {
        m_findStatus->setText(error.text());
        return;
    }
    m_matches = matches;
    // Map text offsets onto cells: they differ when a line holds wide or
    // non-BMP characters.
    QList<CellMatch> cells;
    int mappedLine = -1;
    std::vector<int> columns;
    for (const Match &match : std::as_const(matches)) {
        if (match.line != mappedLine) {
            columns = m_terminal->textColumns(match.line);
            mappedLine = match.line;
        }
        const auto column = [&](int unit) {
            return unit < int(columns.size()) ? columns[std::size_t(unit)] : unit;
        };
        cells.append({match.line, column(match.start), column(match.end)});
    }
    m_view->setMatches(cells);
    m_findStatus->setText(matches.isEmpty() ? tr("no match") : tr("%1 matches").arg(matches.size()));
    if (!matches.isEmpty()) {
        m_currentMatch = matches.first();
        scrollToMatch();
    }
}

void SessionView::findNext()
{
    m_currentMatch = portside::findNext(m_matches, m_currentMatch);
    scrollToMatch();
}

void SessionView::findPrevious()
{
    m_currentMatch = portside::findPrevious(m_matches, m_currentMatch);
    scrollToMatch();
}

void SessionView::scrollToMatch()
{
    if (!m_currentMatch)
        return;
    const int offset = std::max(0, m_terminal->totalLines() - m_currentMatch->line - m_terminal->rows() / 2);
    m_terminal->scrollTo(offset);
    m_view->update();
    updateStatus();
}

// -- file transfers ---------------------------------------------------------------

QWidget *SessionView::buildTransferPanel()
{
    auto *panel = new QWidget(this);
    panel->setAccessibleName(tr("File transfer"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 4, 6, 4);
    auto *top = new QHBoxLayout;
    m_transferTitle = new QLabel(panel);
    m_transferTitle->setAccessibleName(tr("Transfer"));
    top->addWidget(m_transferTitle, 1);
    m_transferCancel = new QPushButton(tr("Cancel transfer"), panel);
    m_transferCancel->setAccessibleName(tr("Cancel transfer"));
    connect(m_transferCancel, &QPushButton::clicked, this, [this] { m_controller->cancelTransfer(); });
    top->addWidget(m_transferCancel);
    layout->addLayout(top);
    m_transferFileBar = new QProgressBar(panel);
    m_transferFileBar->setAccessibleName(tr("This file"));
    layout->addWidget(m_transferFileBar);
    m_transferTotalBar = new QProgressBar(panel);
    m_transferTotalBar->setAccessibleName(tr("All files"));
    m_transferTotalBar->setFormat(tr("all files: %p%"));
    layout->addWidget(m_transferTotalBar);
    m_transferStats = new QLabel(panel);
    m_transferStats->setAccessibleName(tr("Transfer details"));
    m_transferStats->setWordWrap(true);
    layout->addWidget(m_transferStats);
    return panel;
}

void SessionView::showTransferProgress(const TransferProgress &progress)
{
    const TransferJob *job = m_controller->transfer();
    if (job == nullptr || progress.file.isEmpty())
        return;
    const bool sending = job->direction() == TransferDirection::Send;
    QString which = progress.file;
    if (progress.fileCount > 1)
        which = tr("%1 (file %2 of %3)").arg(progress.file).arg(progress.fileIndex).arg(progress.fileCount);
    else if (progress.fileIndex > 1)
        which = tr("%1 (file %2)").arg(progress.file).arg(progress.fileIndex);
    m_transferTitle->setText((sending ? tr("Sending %1 with %2") : tr("Receiving %1 with %2"))
                                 .arg(which, protocolTitle(job->options().protocol)));
    // Percentages in thousandths, so a 4 GB file still fits an int.
    if (progress.fileSize > 0) {
        m_transferFileBar->setRange(0, 1000);
        m_transferFileBar->setValue(int(1000 * progress.fileBytes / progress.fileSize));
    } else if (progress.fileSize == 0) {
        m_transferFileBar->setRange(0, 1);
        m_transferFileBar->setValue(1);
    } else {
        // XMODEM: no size to measure against.
        m_transferFileBar->setRange(0, 0);
    }
    const bool overall = progress.totalSize > 0 && progress.fileCount > 1;
    m_transferTotalBar->setVisible(overall);
    if (overall) {
        m_transferTotalBar->setRange(0, 1000);
        m_transferTotalBar->setValue(int(1000 * progress.totalBytes / progress.totalSize));
    }
    QStringList stats;
    stats << (progress.fileSize >= 0 ? tr("%1 of %2").arg(formatBytes(progress.fileBytes),
                                                          formatBytes(progress.fileSize))
                                     : formatBytes(progress.fileBytes));
    if (progress.bytesPerSecond() > 0) {
        stats << tr("%1/s").arg(formatBytes(qint64(progress.bytesPerSecond())));
        if (progress.fileSize > 0 && progress.fileBytes < progress.fileSize) {
            const double left = double(progress.fileSize - progress.fileBytes) / progress.bytesPerSecond();
            stats << tr("about %1 s left").arg(qRound(left));
        }
    }
    if (progress.retries > 0)
        stats << tr("%n retries", nullptr, progress.retries);
    m_transferStats->setText(stats.join(QStringLiteral(" · ")));
    m_transferNote = progress.fileSize > 0
                         ? tr("%1 %2%").arg(sending ? tr("sending") : tr("receiving"))
                               .arg(int(100 * progress.fileBytes / progress.fileSize))
                         : (sending ? tr("sending") : tr("receiving"));
    m_statusDirty = true;
}

// -- session events ---------------------------------------------------------------

void SessionView::onData(const QByteArray &bytes)
{
    const QByteArray display = m_controller->textPolicy().translateInput(bytes);
    m_view->feed(bytes, display);
    // Cursor position reports and device attributes go back to the device.
    const QByteArray responses = m_terminal->takeResponses();
    if (!responses.isEmpty() && m_controller->isConnected())
        m_controller->send(responses);
    m_statusDirty = true;
}

void SessionView::pollSignals()
{
    if (!m_controller->isConnected())
        return;
    m_controller->session()->modemLines().then(this, [this](QFuture<ModemLines> done) {
        try {
            m_signals->setLines(done.result());
        } catch (const std::exception &) {
        }
    });
}

// -- input ------------------------------------------------------------------------

void SessionView::onKey(QKeyEvent *event)
{
    // Scrollback is local: these keys never reach the device.
    if (event->key() == Qt::Key_PageUp) {
        m_view->scrollPages(1);
        updateStatus();
        return;
    }
    if (event->key() == Qt::Key_PageDown) {
        m_view->scrollPages(-1);
        updateStatus();
        return;
    }
    const QByteArray payload =
        keyToBytes(*event, m_controller->textPolicy(), m_terminal->applicationCursorKeys());
    if (payload.isEmpty())
        return;
    if (m_controller->isTransferring()) {
        // The port belongs to the transfer; keys would corrupt it.
        m_notice = tr("typing is paused while a file transfer runs");
        updateStatus();
        return;
    }
    // Typing returns the view to the live tail.
    m_terminal->follow();
    if (m_controller->textPolicy().localEcho)
        m_terminal->feedText(QString::fromUtf8(payload));
    if (m_controller->isConnected())
        m_controller->send(payload);
    updateStatus();
}

// -- view options -----------------------------------------------------------------

void SessionView::toggleHex()
{
    m_view->setViewMode(m_view->viewMode() == ViewMode::Text ? ViewMode::Hex : ViewMode::Text);
    updateStatus();
}

void SessionView::setTheme(ThemeName theme)
{
    m_view->setTheme(theme);
}

QString SessionView::copySelection()
{
    return m_view->copySelection();
}

void SessionView::pasteFile(const QString &path)
{
    m_controller->pasteFile(path);
}

void SessionView::clear()
{
    m_terminal->reset();
    m_view->update();
    updateStatus();
}

void SessionView::applyTerminalOptions()
{
    const TerminalOptions &options = m_controller->profile().terminal;
    m_view->setTerminalFont(options.fontFamily, options.fontSize);
    m_view->setSelectToCopy(options.selectToCopy);
    m_terminal->setCharset(options.charset);
    m_terminal->setStripHighBit(options.stripHighBit);
}

// -- status -----------------------------------------------------------------------

QString SessionView::statusText() const
{
    const Session *session = m_controller->session();
    QStringList parts = {m_controller->port(), m_controller->settings().describe(),
                         tr("in %1 B").arg(session->bytesIn()), tr("out %1 B").arg(session->bytesOut())};
    if (m_controller->isLogging())
        parts << tr("recording");
    if (m_terminal->scrollOffset() > 0)
        parts << tr("scrollback -%1").arg(m_terminal->scrollOffset());
    if (m_view->viewMode() != ViewMode::Text)
        parts << enumName(m_view->viewMode()).toUpper();
    if (!m_pasteNote.isEmpty())
        parts << m_pasteNote;
    if (!m_transferNote.isEmpty())
        parts << m_transferNote;
    parts << (m_notice.isEmpty() ? enumName(m_controller->state()) : m_notice);
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

void SessionView::updateStatus()
{
    m_statusDirty = false;
    const QString text = statusText();
    m_statusLabel->setText(text);
    Q_EMIT statusChanged(text);
}

// -- shutdown ---------------------------------------------------------------------

void SessionView::closeSession()
{
    if (m_closed)
        return;
    m_closed = true;
    m_signals->stopPolling();
    m_statusTimer->stop();
    m_controller->close();
}

} // namespace portside::gui
