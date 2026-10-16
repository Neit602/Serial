#include "gui/keymap.h"

#include <QKeyEvent>

namespace portside::gui {

namespace {

QByteArray sequenceFor(int key, bool applicationCursor)
{
    const auto cursor = [applicationCursor](char final) {
        return applicationCursor ? QByteArray("\x1bO") + final : QByteArray("\x1b[") + final;
    };
    switch (key) {
    case Qt::Key_Up:
        return cursor('A');
    case Qt::Key_Down:
        return cursor('B');
    case Qt::Key_Right:
        return cursor('C');
    case Qt::Key_Left:
        return cursor('D');
    case Qt::Key_Home:
        return cursor('H');
    case Qt::Key_End:
        return cursor('F');
    case Qt::Key_Insert:
        return QByteArrayLiteral("\x1b[2~");
    case Qt::Key_Delete:
        return QByteArrayLiteral("\x1b[3~");
    case Qt::Key_Escape:
        return QByteArrayLiteral("\x1b");
    case Qt::Key_Tab:
        return QByteArrayLiteral("\t");
    case Qt::Key_Backtab:
        return QByteArrayLiteral("\x1b[Z");
    case Qt::Key_F1:
        return QByteArrayLiteral("\x1bOP");
    case Qt::Key_F2:
        return QByteArrayLiteral("\x1bOQ");
    case Qt::Key_F3:
        return QByteArrayLiteral("\x1bOR");
    case Qt::Key_F4:
        return QByteArrayLiteral("\x1bOS");
    case Qt::Key_F5:
        return QByteArrayLiteral("\x1b[15~");
    case Qt::Key_F6:
        return QByteArrayLiteral("\x1b[17~");
    case Qt::Key_F7:
        return QByteArrayLiteral("\x1b[18~");
    case Qt::Key_F8:
        return QByteArrayLiteral("\x1b[19~");
    case Qt::Key_F9:
        return QByteArrayLiteral("\x1b[20~");
    case Qt::Key_F10:
        return QByteArrayLiteral("\x1b[21~");
    case Qt::Key_F11:
        return QByteArrayLiteral("\x1b[23~");
    case Qt::Key_F12:
        return QByteArrayLiteral("\x1b[24~");
    default:
        return {};
    }
}

// Ctrl-A…Ctrl-Z and the classic punctuation control codes.
QByteArray controlByte(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return QByteArray(1, char(key - Qt::Key_A + 1));
    // The VT100's "subtract 64" rule: Ctrl-@ is NUL, Ctrl-[ is ESC, and so on.
    switch (key) {
    case Qt::Key_At:
    case Qt::Key_BracketLeft:
    case Qt::Key_Backslash:
    case Qt::Key_BracketRight:
    case Qt::Key_AsciiCircum:
    case Qt::Key_Underscore:
        return QByteArray(1, char(key - 0x40));
    case Qt::Key_Space:
        return QByteArray(1, '\0');
    default:
        return {};
    }
}

} // namespace

QByteArray keyToBytes(const QKeyEvent &event, const TextPolicy &policy, bool applicationCursor)
{
    const int key = event.key();
    const Qt::KeyboardModifiers modifiers = event.modifiers();

    if (key == Qt::Key_PageUp || key == Qt::Key_PageDown)
        return {};
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        const QByteArray ending = endingBytes(policy.sendEnding);
        return ending.isEmpty() ? QByteArrayLiteral("\r") : ending;
    }
    if (key == Qt::Key_Backspace)
        return QByteArray(1, backspaceByte(policy.backspace));
    if (const QByteArray sequence = sequenceFor(key, applicationCursor); !sequence.isEmpty())
        return sequence;

    // On macOS Qt reports the Control key as MetaModifier.
#if defined(Q_OS_MACOS)
    const bool control = modifiers.testFlag(Qt::MetaModifier);
#else
    const bool control = modifiers.testFlag(Qt::ControlModifier);
#endif
    if (control)
        return controlByte(key);

    const QString text = event.text();
    if (text.isEmpty())
        return {};
    if (text == QLatin1String("\r") || text == QLatin1String("\n") || text == QLatin1String("\t"))
        return text.toUtf8();
    for (const QChar c : text) {
        if (!c.isPrint())
            return {};
    }
    return text.toUtf8();
}

} // namespace portside::gui
