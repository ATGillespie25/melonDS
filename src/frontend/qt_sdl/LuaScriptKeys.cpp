/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <QKeyEvent>
#include <QKeySequence>
#include <QMutexLocker>

#include "LuaScriptKeys.h"

void LuaKeyTracker::press(QKeyEvent* event)
{
    QString name = keyName(event);
    if (name.isEmpty()) return;

    QMutexLocker locker(&lock);
    keys[keyID(event)] = name;
}

void LuaKeyTracker::release(QKeyEvent* event)
{
    QMutexLocker locker(&lock);
    keys.erase(keyID(event));
}

void LuaKeyTracker::releaseAll()
{
    QMutexLocker locker(&lock);
    keys.clear();
}

QStringList LuaKeyTracker::held()
{
    QMutexLocker locker(&lock);

    QStringList ret;
    for (auto& [id, name] : keys)
    {
        if (!ret.contains(name))
            ret.append(name);
    }
    return ret;
}

quint64 LuaKeyTracker::keyID(QKeyEvent* event)
{
    // Prefer the physical key. Platforms that report no scan code fall back to
    // the virtual key, then to Qt's key code.
    if (event->nativeScanCode() != 0)
        return (1ULL << 32) | event->nativeScanCode();
    if (event->nativeVirtualKey() != 0)
        return (2ULL << 32) | event->nativeVirtualKey();
    return (3ULL << 32) | (quint32)event->key();
}

QString LuaKeyTracker::keyName(QKeyEvent* event)
{
    int key = event->key();
    bool keypad = (event->modifiers() & Qt::KeypadModifier) != 0;

    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return QString(QChar('A' + (key - Qt::Key_A)));

    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return (keypad ? "Numpad" : "") + QString(QChar('0' + (key - Qt::Key_0)));

    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return "F" + QString::number(key - Qt::Key_F1 + 1);

    switch (key)
    {
    case Qt::Key_Up: return "Up";
    case Qt::Key_Down: return "Down";
    case Qt::Key_Left: return "Left";
    case Qt::Key_Right: return "Right";
    case Qt::Key_Home: return "Home";
    case Qt::Key_End: return "End";
    case Qt::Key_PageUp: return "PageUp";
    case Qt::Key_PageDown: return "PageDown";
    case Qt::Key_Insert: return "Insert";
    case Qt::Key_Delete: return "Delete";
    case Qt::Key_Space: return "Space";
    case Qt::Key_Escape: return "Escape";
    case Qt::Key_Return: return "Enter";
    case Qt::Key_Enter: return keypad ? "NumpadEnter" : "Enter";
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return "Tab";
    case Qt::Key_Backspace: return "Backspace";

    case Qt::Key_Shift: return "Shift";
    case Qt::Key_Control: return "Ctrl";
    case Qt::Key_Alt:
    case Qt::Key_AltGr: return "Alt";
    case Qt::Key_Meta: return "Meta";
    case Qt::Key_CapsLock: return "CapsLock";
    case Qt::Key_NumLock: return "NumLock";
    case Qt::Key_ScrollLock: return "ScrollLock";
    case Qt::Key_Pause: return "Pause";
    case Qt::Key_Print: return "PrintScreen";
    case Qt::Key_Menu: return "Menu";

    // Punctuation is named after the unshifted key on a US layout, so the name
    // does not change when Shift is held.
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return "LeftBracket";
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return "RightBracket";
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return "Semicolon";
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return "Apostrophe";
    case Qt::Key_Comma:
    case Qt::Key_Less: return "Comma";
    case Qt::Key_Period:
    case Qt::Key_Greater: return keypad ? "NumpadPeriod" : "Period";
    case Qt::Key_Slash:
    case Qt::Key_Question: return keypad ? "NumpadSlash" : "Slash";
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return "Backslash";
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return "Backquote";
    case Qt::Key_Minus:
    case Qt::Key_Underscore: return keypad ? "NumpadMinus" : "Minus";
    case Qt::Key_Equal: return "Equals";
    case Qt::Key_Plus: return keypad ? "NumpadPlus" : "Equals";
    case Qt::Key_Asterisk: return keypad ? "NumpadMultiply" : "8";

    case Qt::Key_Exclam: return "1";
    case Qt::Key_At: return "2";
    case Qt::Key_NumberSign: return "3";
    case Qt::Key_Dollar: return "4";
    case Qt::Key_Percent: return "5";
    case Qt::Key_AsciiCircum: return "6";
    case Qt::Key_Ampersand: return "7";
    case Qt::Key_ParenLeft: return "9";
    case Qt::Key_ParenRight: return "0";
    }

    if (key == 0 || key == Qt::Key_unknown)
        return QString();

    // Anything else gets Qt's portable name with spaces removed, which covers
    // media keys and the like.
    QString name = QKeySequence(key).toString(QKeySequence::PortableText);
    name.remove(' ');
    return name;
}
