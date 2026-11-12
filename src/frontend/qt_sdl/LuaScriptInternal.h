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

#ifndef LUASCRIPTINTERNAL_H
#define LUASCRIPTINTERNAL_H

// Private to the Lua scripting host: shared between LuaScript.cpp (engine and
// scheduler) and LuaScriptApi.cpp (the emu, mem, rom, pad, touch and input
// modules). Nothing outside those files should include this.

#include <lua.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include "types.h"

class EmuInstance;
class EmuThread;
class LuaScriptEngine;

namespace melonDS
{
class NDS;
namespace NDSCart { class CartCommon; }
}

enum
{
    luaEvent_FrameStart = 0,
    luaEvent_FrameEnd,
    luaEvent_StateLoaded,
    luaEvent_ROMChanged,
    luaEvent_Reset,
    luaEvent_Stop,
    luaEvent_Max,
};

enum
{
    luaWait_None = 0,
    luaWait_Frame,      // emu.frameadvance
    luaWait_Pass,       // emu.yield, or a bare coroutine.yield from the main coroutine
};

// The part of a script that other threads may see. Guarded by the engine's lock,
// except Abort, which any thread may set at any time to interrupt the script.
struct LuaScriptEntry
{
    int ID;
    QString Path;
    QStringList Args;
    int State;
    std::atomic<bool> Abort {false};
};

struct LuaCallback
{
    int Handle;
    int Event;
    int FuncRef;
};

// A loaded script. Only ever touched on the emu thread.
struct LuaScript
{
    std::shared_ptr<LuaScriptEntry> Entry;
    LuaScriptEngine* Engine = nullptr;

    lua_State* L = nullptr;             // the state's main thread, never run directly
    lua_State* MainCo = nullptr;        // the script's main chunk
    int MainCoRef = LUA_NOREF;
    bool MainAlive = false;
    int Wait = luaWait_None;
    melonDS::u64 WaitFrame = 0;         // frame count when emu.frameadvance was called
    melonDS::u64 LastResumePass = 0;

    lua_State* CallbackCo = nullptr;    // reused to run event callbacks
    int CallbackCoRef = LUA_NOREF;

    std::vector<LuaCallback> Callbacks;
    int NextHandle = 1;

    // While the stop event runs, the abort flag is ignored and this deadline
    // (in emu.clock seconds) bounds how long the handlers may take.
    bool Stopping = false;
    double StopDeadline = 0;

    // Input overrides, as masks over the 12 buttons in EmuInstance::buttonNames order.
    melonDS::u32 PadForcePress = 0;
    melonDS::u32 PadForceRelease = 0;
    bool TouchActive = false;
    int TouchX = 0, TouchY = 0;
};

// Access to EmuInstance internals for the scripting host. EmuInstance declares
// this a friend so the host does not widen EmuInstance's public interface.
struct LuaHost
{
    // The console if one is running, otherwise nullptr.
    static melonDS::NDS* ActiveNDS(EmuInstance* inst);
    // The console object even when emulation is stopped (may be nullptr).
    static melonDS::NDS* AnyNDS(EmuInstance* inst);
    static EmuThread* Thread(EmuInstance* inst);

    static bool SaveState(EmuInstance* inst, const std::string& path);
    static bool LoadState(EmuInstance* inst, const std::string& path);

    // Physical button state (bits clear = pressed), ignoring script overrides.
    static melonDS::u32 PhysicalButtons(EmuInstance* inst);
    // Applies script overrides on top of the physical state for the coming frame.
    static void ApplyOverrides(EmuInstance* inst, melonDS::u32 press, melonDS::u32 release,
                               bool touch, int touchX, int touchY);

    static QString ROMPath(EmuInstance* inst);

    // Called by EmuInstance::loadROM before the cart is inserted: keeps a copy
    // of the bytes inserting it may change, so rom.bytes can undo the change.
    static void KeepOriginalROM(EmuInstance* inst, melonDS::NDSCart::CartCommon* cart);
    // Copies n bytes of the ROM as it is on disk, starting at offset, into out.
    static void ReadOriginalROM(EmuInstance* inst, melonDS::NDSCart::CartCommon* cart,
                                melonDS::u32 offset, melonDS::u32 n, char* out);
    static QStringList HeldKeys(EmuInstance* inst);

    static void RequestPause(EmuInstance* inst, bool pause);
};

// Returns the script owning a Lua state or coroutine.
inline LuaScript* LuaGetScript(lua_State* L)
{
    return *(LuaScript**)lua_getextraspace(L);
}

// Module registration. Each function creates one global table in a fresh state.
// Adding a module (for example a future speech module) means writing one more of
// these and listing it in kLuaModules in LuaScript.cpp.
void LuaOpenEmu(lua_State* L);
void LuaOpenMem(lua_State* L);
void LuaOpenROM(lua_State* L);
void LuaOpenPad(lua_State* L);
void LuaOpenTouch(lua_State* L);
void LuaOpenInput(lua_State* L);
void LuaOpenEvent(lua_State* L);

// Engine services used by the API modules.
melonDS::u64 LuaEngineFrameCount(LuaScriptEngine* engine);
double LuaEngineClock(LuaScriptEngine* engine);
// Pending script pause requests not yet applied by the emu thread: 1 pause, -1 unpause, 0 none.
int& LuaEnginePendingPause(LuaScriptEngine* engine);
EmuInstance* LuaEngineInstance(LuaScriptEngine* engine);
void LuaEngineQueueEvent(LuaScriptEngine* engine, int event);

#endif // LUASCRIPTINTERNAL_H
