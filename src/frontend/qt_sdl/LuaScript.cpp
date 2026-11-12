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

#include <lua.hpp>

#include "LuaScript.h"
#include "EmuInstance.h"

// Skeleton only: the scripting engine is not implemented yet. Every method is a
// stub so the frontend builds and links against the vendored Lua library.

struct LuaScriptManager::Private
{
};

LuaScriptManager::LuaScriptManager(EmuInstance* inst) : QObject(nullptr), emuInstance(inst), d(new Private)
{
}

LuaScriptManager::~LuaScriptManager()
{
    delete d;
}

int LuaScriptManager::loadScript(const QString& path, const QStringList& args)
{
    return -1;
}

void LuaScriptManager::stopScript(int id)
{
}

void LuaScriptManager::restartScript(int id)
{
}

void LuaScriptManager::removeScript(int id)
{
}

void LuaScriptManager::stopAll()
{
}

QList<LuaScriptInfo> LuaScriptManager::scripts()
{
    return QList<LuaScriptInfo>();
}

QList<LuaConsoleMessage> LuaScriptManager::backlog()
{
    return QList<LuaConsoleMessage>();
}

void LuaScriptManager::clearBacklog()
{
}

QStringList LuaScriptManager::autoloadScripts()
{
    return QStringList();
}

void LuaScriptManager::setAutoloadScripts(const QStringList& paths)
{
}

bool LuaScriptManager::hasActiveScripts()
{
    return false;
}

void LuaScriptManager::onLoopPass()
{
}

void LuaScriptManager::onFrameStart()
{
}

void LuaScriptManager::onFrameEnd()
{
}

void LuaScriptManager::onStateLoaded()
{
}

void LuaScriptManager::onROMChanged()
{
}

void LuaScriptManager::onReset()
{
}

void LuaScriptManager::shutdown()
{
}
