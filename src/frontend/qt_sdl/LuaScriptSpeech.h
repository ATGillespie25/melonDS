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

#ifndef LUASCRIPTSPEECH_H
#define LUASCRIPTSPEECH_H

#include <string>

// Process-wide speech output for Lua scripts (see LuaScriptSpeech.cpp). Safe
// to call from any thread; nothing here ever throws or raises a Lua error.
namespace LuaSpeech
{

bool Say(const char* text, bool interrupt);
bool Stop();
// True and the backend's name when speech works.
bool Available(std::string& name);
// A one-time message explaining why speech is unavailable, if there is one.
bool TakeNotice(std::string& out);
// Frees the backend and the library. Call once at process exit, after every
// emu thread has finished.
void Shutdown();

}

#endif // LUASCRIPTSPEECH_H
