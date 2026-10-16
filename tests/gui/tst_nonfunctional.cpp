// Non-functional requirements a test can hold: memory, accessibility,
// shortcut hygiene, and the CLI staying free of GUI libraries.

#include "gui/gui_support.h"
#include "gui/palette.h"
#include "gui/terminal_widget.h"
#include "support/support.h"

#include <QAction>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

using namespace portside;
using namespace portside::gui;
using namespace portside::test;

namespace {

// Resident set size in MB, from /proc (Linux only).
std::optional<double> residentMegabytes()
{
#if defined(Q_OS_LINUX)
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QList<QByteArray> fields = statm.readAll().split(' ');
    if (fields.size() < 2)
        return std::nullopt;
    return fields.at(1).toDouble() * double(::sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#else
    return std::nullopt;
#endif
}

} // namespace

class TestNonFunctional : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fourSessionsWithFullScrollbackStayWithinBudget()
    {
        // PRD: under 200 MB with 4 sessions and 10,000-line scrollback each.
        // What a test can measure reliably is the growth those sessions cause
        // (DECISIONS D38).
#if defined(__SANITIZE_ADDRESS__)
        QSKIP("AddressSanitizer's shadow memory makes resident size meaningless");
#endif
        const auto baseline = residentMegabytes();
        if (!baseline)
            QSKIP("resident memory is only readable from /proc on Linux");
        std::vector<std::unique_ptr<ViewFixture>> views;
        for (int index = 0; index < 4; ++index) {
            views.push_back(std::make_unique<ViewFixture>());
            // Enough to fill the scrollback and the screen below it.
            for (int line = 0; line < 10'000 + views.back()->view->terminal()->rows(); ++line)
                views.back()->view->view()->feed(QStringLiteral("line %1 of session %2 — boot log\r\n")
                                                     .arg(line)
                                                     .arg(index)
                                                     .toUtf8());
            QCOMPARE(views.back()->view->terminal()->historySize(), 10'000);
        }
        const double growth = *residentMegabytes() - *baseline;
        qInfo("four sessions with full scrollback grew the process by %.0f MB", growth);
        QVERIFY2(growth < 200, qPrintable(QStringLiteral("grew by %1 MB").arg(growth, 0, 'f', 0)));
    }

    void keyboardShortcutsAreUnique()
    {
        WindowFixture fixture;
        auto window = fixture.window();
        QHash<QString, QString> seen;
        QStringList duplicates;
        for (QAction *action : window->findChildren<QAction *>()) {
            const QString shortcut = action->shortcut().toString();
            if (shortcut.isEmpty())
                continue;
            if (seen.contains(shortcut) && seen.value(shortcut) != action->iconText())
                duplicates << QStringLiteral("%1: %2 and %3").arg(shortcut, seen.value(shortcut), action->iconText());
            seen.insert(shortcut, action->iconText());
        }
        QVERIFY2(duplicates.isEmpty(), qPrintable(duplicates.join(QStringLiteral(", "))));
        window->showShortcuts();
        QVERIFY(window->lastMessage().contains(QStringLiteral("Send break")));
    }

    void theHighContrastThemeMeetsAContrastRatio()
    {
        const TerminalPalette &palette = theme(ThemeName::HighContrast);
        QVERIFY2(contrastRatio(palette.foreground, palette.background) >= 7.0, "body text must clear WCAG AAA");
        for (std::size_t i = 1; i < palette.ansi.size(); ++i) // black on black is the one exception
            QVERIFY2(contrastRatio(palette.ansi[i], palette.background) >= 4.5, qPrintable(QString::number(i)));
    }

    void theCommandLineNeedsNoGuiLibraries()
    {
        // DECISIONS D35: a headless lab box runs `portside` without Qt Widgets
        // or Qt Gui installed.
#if defined(Q_OS_LINUX)
        if (QStandardPaths::findExecutable(QStringLiteral("ldd")).isEmpty())
            QSKIP("ldd is not available");
        QProcess ldd;
        ldd.start(QStringLiteral("ldd"), {qEnvironmentVariable("PORTSIDE_CLI")});
        QVERIFY(ldd.waitForFinished());
        const QByteArray libraries = ldd.readAllStandardOutput();
        QVERIFY2(libraries.contains("Qt6Core"), libraries.constData());
        QVERIFY2(!libraries.contains("Qt6Gui") && !libraries.contains("Qt6Widgets"), libraries.constData());
#else
        QSKIP("checked with ldd on Linux");
#endif
    }
};

QTEST_MAIN(TestNonFunctional)
#include "tst_nonfunctional.moc"
