#pragma once

// One session's page: terminal, find bar, paste progress, signals panel and
// status line (PRD F22).
//
// Everything a tab (or a detached window) needs to show one connection.  It
// owns no I/O: it talks to a SessionController.

#include "core/errors.h"
#include "core/find.h"
#include "core/terminal.h"
#include "gui/palette.h"

#include <QList>
#include <QWidget>

#include <memory>
#include <optional>

class QLabel;
class QLineEdit;
class QProgressBar;
class QTimer;

namespace portside::gui {

class SessionController;
class SignalsPanel;
class TerminalWidget;

class SessionView : public QWidget
{
    Q_OBJECT

public:
    // Takes ownership of `controller`.
    explicit SessionView(SessionController *controller, ThemeName theme = ThemeName::Dark,
                         QWidget *parent = nullptr);
    ~SessionView() override;

    SessionController *controller() const { return m_controller; }
    Terminal *terminal() const { return m_terminal.get(); }
    TerminalWidget *view() const { return m_view; }
    SignalsPanel *signalsPanel() const { return m_signals; }
    QWidget *findBar() const { return m_findBar; }
    QLineEdit *findInput() const { return m_findInput; }
    QLabel *findStatus() const { return m_findStatus; }
    QProgressBar *pasteProgress() const { return m_pasteProgress; }
    QLabel *statusLabel() const { return m_statusLabel; }

    // -- find (F8) ---------------------------------------------------------

    void openFind();
    void closeFind();
    void findNext();
    void findPrevious();
    QList<Match> matches() const { return m_matches; }

    // -- view options ------------------------------------------------------

    // Switch between the terminal and a hex dump (F10).
    void toggleHex();
    void setTheme(ThemeName theme);
    QString copySelection();
    void pasteFile(const QString &path);
    // Clear the screen and scrollback.
    void clear();
    // Apply the controller's (edited) terminal options: font, charset...
    void applyTerminalOptions();

    // The status line for this session (F22).
    QString statusText() const;

    // Stop polling and close the session.
    void closeSession();

Q_SIGNALS:
    void statusChanged(const QString &text);
    void errorRaised(const portside::PortsideError &error);

private:
    QWidget *buildFindBar();
    void runFind(const QString &query);
    void scrollToMatch();
    void onData(const QByteArray &bytes);
    void onKey(QKeyEvent *event);
    void pollSignals();
    void updateStatus();

    SessionController *m_controller;
    std::unique_ptr<Terminal> m_terminal;
    TerminalWidget *m_view = nullptr;
    SignalsPanel *m_signals = nullptr;
    QWidget *m_findBar = nullptr;
    QLineEdit *m_findInput = nullptr;
    QLabel *m_findStatus = nullptr;
    QProgressBar *m_pasteProgress = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_statusTimer = nullptr;

    QString m_notice;
    QString m_pasteNote;
    QList<Match> m_matches;
    std::optional<Match> m_currentMatch;
    bool m_statusDirty = false;
    bool m_closed = false;
};

} // namespace portside::gui
