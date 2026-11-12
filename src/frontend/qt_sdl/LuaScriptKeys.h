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

#ifndef LUASCRIPTKEYS_H
#define LUASCRIPTKEYS_H

#include <map>

#include <QMutex>
#include <QString>
#include <QStringList>

class QKeyEvent;

// Tracks every host keyboard key currently held in an instance's windows, mapped
// or not, for Lua's input.keys(). Fed from the UI thread by EmuInstance's key
// handlers and read from the emu thread.
//
// Keys are identified by their physical scan code, so a key released after its
// modifier (Shift+1 pressed, Shift let go, then 1 let go) is still matched. The
// name is decided when the key goes down. Gamepad buttons would get a tracker
// of their own alongside this one.
class LuaKeyTracker
{
public:
    void press(QKeyEvent* event);
    void release(QKeyEvent* event);
    void releaseAll();

    // Names of the held keys, without duplicates.
    QStringList held();

    // The script-facing name of a key event, documented in doc/lua-api.md.
    static QString keyName(QKeyEvent* event);

private:
    static quint64 keyID(QKeyEvent* event);

    QMutex lock;
    std::map<quint64, QString> keys;
};

#endif // LUASCRIPTKEYS_H
