#include "cli/console.h"

#include <cstdio>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace portside::cli {

namespace {
// While the terminal is raw it no longer turns LF into CRLF, so status lines
// written to stderr need the CR themselves.
std::atomic<bool> g_rawActive{false};
} // namespace

struct Console::Saved
{
#if defined(Q_OS_WIN)
    DWORD inputMode = 0;
    DWORD outputMode = 0;
    UINT inputCodePage = 0;
    UINT outputCodePage = 0;
#else
    termios attributes{};
#endif
};

Console::Console()
{
#if defined(Q_OS_WIN)
    // Device bytes are binary; no CRLF translation in either direction.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

Console::~Console()
{
    restore();
}

bool Console::stdinIsTty()
{
#if defined(Q_OS_WIN)
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

bool Console::stdoutIsTty()
{
#if defined(Q_OS_WIN)
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

void Console::enterRaw()
{
    if (m_saved != nullptr || !stdinIsTty())
        return;
    auto *saved = new Saved;
#if defined(Q_OS_WIN)
    const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    ::GetConsoleMode(in, &saved->inputMode);
    ::GetConsoleMode(out, &saved->outputMode);
    saved->inputCodePage = ::GetConsoleCP();
    saved->outputCodePage = ::GetConsoleOutputCP();
    // Keys as VT sequences, no line editing, no echo, Ctrl-C as a byte.
    ::SetConsoleMode(in, ENABLE_VIRTUAL_TERMINAL_INPUT);
    ::SetConsoleMode(out, saved->outputMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                              | DISABLE_NEWLINE_AUTO_RETURN);
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleOutputCP(CP_UTF8);
#else
    if (::tcgetattr(STDIN_FILENO, &saved->attributes) != 0) {
        delete saved;
        return;
    }
    termios raw = saved->attributes;
    ::cfmakeraw(&raw);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
    m_saved = saved;
    g_rawActive = true;
}

void Console::restore()
{
    if (m_saved == nullptr)
        return;
#if defined(Q_OS_WIN)
    ::SetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), m_saved->inputMode);
    ::SetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), m_saved->outputMode);
    ::SetConsoleCP(m_saved->inputCodePage);
    ::SetConsoleOutputCP(m_saved->outputCodePage);
#else
    ::tcsetattr(STDIN_FILENO, TCSADRAIN, &m_saved->attributes);
#endif
    delete m_saved;
    m_saved = nullptr;
    g_rawActive = false;
}

void Console::writeOut(const QByteArray &data)
{
    std::fwrite(data.constData(), 1, std::size_t(data.size()), stdout);
    std::fflush(stdout);
}

void Console::writeErr(const QString &text)
{
    QByteArray bytes = text.toUtf8() + '\n';
    if (g_rawActive)
        bytes.replace("\n", "\r\n");
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stderr);
    std::fflush(stderr);
}

ConsoleReader::ConsoleReader(QObject *parent)
    : QObject(parent)
{
}

ConsoleReader::~ConsoleReader()
{
    stop();
}

void ConsoleReader::start()
{
    if (m_thread.joinable())
        return;
    m_stop = false;
    m_thread = std::thread([this] { run(); });
}

void ConsoleReader::stop()
{
    m_stop = true;
    if (!m_thread.joinable())
        return;
#if defined(Q_OS_WIN)
    // A blocking read on a pipe cannot be interrupted; the process is about
    // to exit, so the thread is left to the OS rather than joined.
    if (!Console::stdinIsTty()) {
        m_thread.detach();
        return;
    }
#endif
    m_thread.join();
}

void ConsoleReader::run()
{
    const auto deliver = [this](QByteArray data) {
        QMetaObject::invokeMethod(
            this, [this, data] { Q_EMIT received(data); }, Qt::QueuedConnection);
    };
    const auto finish = [this] {
        QMetaObject::invokeMethod(this, [this] { Q_EMIT ended(); }, Qt::QueuedConnection);
    };
    char buffer[4096];

#if defined(Q_OS_WIN)
    const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    const bool interactive = Console::stdinIsTty();
    while (!m_stop) {
        if (interactive) {
            if (::WaitForSingleObject(in, 100) != WAIT_OBJECT_0)
                continue;
            // The handle also signals for focus and mouse events; only read
            // when a key press with a character is waiting, or ReadFile
            // would block.
            INPUT_RECORD records[64];
            DWORD count = 0;
            if (!::PeekConsoleInputW(in, records, 64, &count)) {
                finish();
                return;
            }
            bool hasKey = false;
            for (DWORD i = 0; i < count; ++i) {
                const auto &event = records[i];
                if (event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown
                    && event.Event.KeyEvent.uChar.UnicodeChar != 0) {
                    hasKey = true;
                }
            }
            if (!hasKey) {
                ::ReadConsoleInputW(in, records, count, &count);
                continue;
            }
        }
        DWORD got = 0;
        if (!::ReadFile(in, buffer, sizeof(buffer), &got, nullptr) || got == 0) {
            if (!interactive) {
                finish();
                return;
            }
            continue;
        }
        deliver(QByteArray(buffer, qsizetype(got)));
    }
#else
    while (!m_stop) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(STDIN_FILENO, &ready);
        timeval timeout{0, 100'000};
        const int result = ::select(STDIN_FILENO + 1, &ready, nullptr, nullptr, &timeout);
        if (result < 0) {
            finish();
            return;
        }
        if (result == 0)
            continue;
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (got <= 0) {
            finish();
            return;
        }
        deliver(QByteArray(buffer, qsizetype(got)));
    }
#endif
}

} // namespace portside::cli
