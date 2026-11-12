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

// The emu, mem, rom, pad, touch and input modules of the Lua scripting host.
// Everything here runs on the emu thread, inside a script.

#include <string.h>

#include <QFileInfo>

#include "LuaScriptInternal.h"
#include "EmuInstance.h"
#include "NDS.h"
#include "NDSCart.h"

using namespace melonDS;


// ---- EmuInstance access ----

NDS* LuaHost::ActiveNDS(EmuInstance* inst)
{
    if (!inst->nds || !inst->emuThread->emuIsActive())
        return nullptr;
    return inst->nds;
}

NDS* LuaHost::AnyNDS(EmuInstance* inst)
{
    return inst->nds;
}

EmuThread* LuaHost::Thread(EmuInstance* inst)
{
    return inst->emuThread;
}

bool LuaHost::SaveState(EmuInstance* inst, const std::string& path)
{
    return inst->saveState(path);
}

bool LuaHost::LoadState(EmuInstance* inst, const std::string& path)
{
    return inst->loadState(path);
}

u32 LuaHost::PhysicalButtons(EmuInstance* inst)
{
    return inst->keyInputMask & inst->joyInputMask & 0xFFF;
}

void LuaHost::ApplyOverrides(EmuInstance* inst, u32 press, u32 release, bool touch, int touchX, int touchY)
{
    inst->inputMask = (inst->inputMask | release) & ~press;

    inst->luaTouchOverride = touch;
    inst->luaTouchX = touchX;
    inst->luaTouchY = touchY;
}

QString LuaHost::ROMPath(EmuInstance* inst)
{
    if (inst->loadedROMPath.isEmpty())
        return QString();
    return QFileInfo(inst->loadedROMPath[0]).absoluteFilePath();
}

QStringList LuaHost::HeldKeys(EmuInstance* inst)
{
    return inst->luaKeys.held();
}

void LuaHost::RequestPause(EmuInstance* inst, bool pause)
{
    // We are on the emu thread, so this must not wait for the message to be
    // handled: it is picked up at the end of the current loop pass.
    inst->emuThread->sendMessage({
        .type = pause ? EmuThread::msg_EmuPause : EmuThread::msg_EmuUnpause,
        .noReply = true});
}


// ---- helpers ----

static EmuInstance* Inst(lua_State* L)
{
    return LuaEngineInstance(LuaGetScript(L)->Engine);
}

static NDS* RequireNDS(lua_State* L)
{
    NDS* nds = LuaHost::ActiveNDS(Inst(L));
    if (!nds)
        luaL_error(L, "no game is running");
    return nds;
}

static void SetFuncs(lua_State* L, const luaL_Reg* funcs, const char* name)
{
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_setglobal(L, name);
}


// ---- emu ----

static int WaitFor(lua_State* L, int wait, const char* name)
{
    LuaScript* s = LuaGetScript(L);

    if (L != s->MainCo)
        return luaL_error(L, "%s can only be called from the script's main coroutine, "
                             "not from an event callback or a coroutine the script created", name);
    if (!lua_isyieldable(L))
        return luaL_error(L, "%s cannot be called here (inside a metamethod or a C function)", name);

    s->Wait = wait;
    s->WaitFrame = LuaEngineFrameCount(s->Engine);
    return lua_yield(L, 0);
}

static int Emu_FrameAdvance(lua_State* L)
{
    return WaitFor(L, luaWait_Frame, "emu.frameadvance");
}

static int Emu_Yield(lua_State* L)
{
    return WaitFor(L, luaWait_Pass, "emu.yield");
}

static int Emu_FrameCount(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)LuaEngineFrameCount(LuaGetScript(L)->Engine));
    return 1;
}

static bool IsPaused(lua_State* L)
{
    int pending = LuaEnginePendingPause(LuaGetScript(L)->Engine);
    if (pending != 0)
        return pending > 0;
    return !LuaHost::Thread(Inst(L))->emuIsRunning();
}

static int Emu_Pause(lua_State* L)
{
    if (!IsPaused(L))
    {
        LuaHost::RequestPause(Inst(L), true);
        LuaEnginePendingPause(LuaGetScript(L)->Engine) = 1;
    }
    return 0;
}

static int Emu_Unpause(lua_State* L)
{
    if (IsPaused(L) && LuaHost::Thread(Inst(L))->emuIsActive())
    {
        LuaHost::RequestPause(Inst(L), false);
        LuaEnginePendingPause(LuaGetScript(L)->Engine) = -1;
    }
    return 0;
}

static int Emu_Paused(lua_State* L)
{
    lua_pushboolean(L, IsPaused(L));
    return 1;
}

static int Emu_Clock(lua_State* L)
{
    lua_pushnumber(L, LuaEngineClock(LuaGetScript(L)->Engine));
    return 1;
}

static int Emu_SaveState(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);

    if (!LuaHost::ActiveNDS(Inst(L)))
    {
        luaL_pushfail(L);
        lua_pushstring(L, "no game is running");
        return 2;
    }

    if (!LuaHost::SaveState(Inst(L), path))
    {
        luaL_pushfail(L);
        lua_pushfstring(L, "could not save state to '%s'", path);
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int Emu_LoadState(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);

    if (!LuaHost::ActiveNDS(Inst(L)))
    {
        luaL_pushfail(L);
        lua_pushstring(L, "no game is running");
        return 2;
    }

    if (!LuaHost::LoadState(Inst(L), path))
    {
        luaL_pushfail(L);
        lua_pushfstring(L, "could not load state from '%s'", path);
        return 2;
    }

    // dispatched once this script yields or returns
    LuaEngineQueueEvent(LuaGetScript(L)->Engine, luaEvent_StateLoaded);

    lua_pushboolean(L, 1);
    return 1;
}

void LuaOpenEmu(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"frameadvance", Emu_FrameAdvance},
        {"yield", Emu_Yield},
        {"framecount", Emu_FrameCount},
        {"pause", Emu_Pause},
        {"unpause", Emu_Unpause},
        {"paused", Emu_Paused},
        {"clock", Emu_Clock},
        {"savestate", Emu_SaveState},
        {"loadstate", Emu_LoadState},
        {nullptr, nullptr}
    };

    SetFuncs(L, funcs, "emu");
}


// ---- mem ----

template <int cpu> static u8 Read8(NDS* nds, u32 addr)
{
    return cpu ? nds->ARM7Read8(addr) : nds->ARM9Read8(addr);
}

template <int cpu> static u16 Read16(NDS* nds, u32 addr)
{
    return cpu ? nds->ARM7Read16(addr) : nds->ARM9Read16(addr);
}

template <int cpu> static u32 Read32(NDS* nds, u32 addr)
{
    return cpu ? nds->ARM7Read32(addr) : nds->ARM9Read32(addr);
}

static u32 CheckAddr(lua_State* L, int idx)
{
    return (u32)luaL_checkinteger(L, idx);
}

template <int cpu> static int Mem_U8(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    lua_pushinteger(L, Read8<cpu>(nds, CheckAddr(L, 1)));
    return 1;
}

template <int cpu> static int Mem_U16(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    lua_pushinteger(L, Read16<cpu>(nds, CheckAddr(L, 1)));
    return 1;
}

template <int cpu> static int Mem_U32(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    lua_pushinteger(L, Read32<cpu>(nds, CheckAddr(L, 1)));
    return 1;
}

template <int cpu> static int Mem_S8(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    lua_pushinteger(L, (s8)Read8<cpu>(nds, CheckAddr(L, 1)));
    return 1;
}

template <int cpu> static int Mem_S16(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    lua_pushinteger(L, (s16)Read16<cpu>(nds, CheckAddr(L, 1)));
    return 1;
}

template <int cpu> static int Mem_S32(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    lua_pushinteger(L, (s32)Read32<cpu>(nds, CheckAddr(L, 1)));
    return 1;
}

template <int cpu> static int Mem_W8(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    u32 addr = CheckAddr(L, 1);
    u8 val = (u8)luaL_checkinteger(L, 2);
    if (cpu) nds->ARM7Write8(addr, val);
    else     nds->ARM9Write8(addr, val);
    return 0;
}

template <int cpu> static int Mem_W16(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    u32 addr = CheckAddr(L, 1);
    u16 val = (u16)luaL_checkinteger(L, 2);
    if (cpu) nds->ARM7Write16(addr, val);
    else     nds->ARM9Write16(addr, val);
    return 0;
}

template <int cpu> static int Mem_W32(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    u32 addr = CheckAddr(L, 1);
    u32 val = (u32)luaL_checkinteger(L, 2);
    if (cpu) nds->ARM7Write32(addr, val);
    else     nds->ARM9Write32(addr, val);
    return 0;
}

template <int cpu> static int Mem_Bytes(lua_State* L)
{
    NDS* nds = RequireNDS(L);
    u32 addr = CheckAddr(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);
    luaL_argcheck(L, n >= 0, 2, "length must not be negative");
    luaL_argcheck(L, n <= 0x1000000, 2, "length must be at most 16 MB");

    luaL_Buffer b;
    char* out = luaL_buffinitsize(L, &b, (size_t)n);

    u64 end = (u64)addr + (u64)n;
    if (addr >= 0x02000000 && end <= 0x03000000)
    {
        // Entirely in the main RAM region, which both buses map straight to the
        // MainRAM array (mirrored every MainRAMMask+1 bytes).
        u32 mask = nds->MainRAMMask;
        u32 done = 0;
        while (done < (u32)n)
        {
            u32 offset = (addr + done) & mask;
            u32 chunk = mask + 1 - offset;
            if (chunk > (u32)n - done) chunk = (u32)n - done;
            memcpy(out + done, &nds->MainRAM[offset], chunk);
            done += chunk;
        }
    }
    else
    {
        for (lua_Integer i = 0; i < n; i++)
            out[i] = (char)Read8<cpu>(nds, addr + (u32)i);
    }

    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

template <int cpu> static void PushMemFuncs(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"u8", Mem_U8<cpu>},
        {"u16", Mem_U16<cpu>},
        {"u32", Mem_U32<cpu>},
        {"s8", Mem_S8<cpu>},
        {"s16", Mem_S16<cpu>},
        {"s32", Mem_S32<cpu>},
        {"w8", Mem_W8<cpu>},
        {"w16", Mem_W16<cpu>},
        {"w32", Mem_W32<cpu>},
        {"bytes", Mem_Bytes<cpu>},
        {nullptr, nullptr}
    };

    luaL_newlibtable(L, funcs);
    luaL_setfuncs(L, funcs, 0);
}

void LuaOpenMem(lua_State* L)
{
    PushMemFuncs<0>(L);
    PushMemFuncs<1>(L);
    lua_setfield(L, -2, "arm7");
    lua_setglobal(L, "mem");
}


// ---- rom ----

static NDSCart::CartCommon* Cart(lua_State* L)
{
    NDS* nds = LuaHost::AnyNDS(Inst(L));
    if (!nds) return nullptr;
    return nds->NDSCartSlot.GetCart();
}

static int ROM_Code(lua_State* L)
{
    NDSCart::CartCommon* cart = Cart(L);
    if (!cart) { lua_pushnil(L); return 1; }

    lua_pushlstring(L, cart->GetHeader().GameCode, 4);
    return 1;
}

static int ROM_Title(lua_State* L)
{
    NDSCart::CartCommon* cart = Cart(L);
    if (!cart) { lua_pushnil(L); return 1; }

    const char* title = cart->GetHeader().GameTitle;
    size_t len = strnlen(title, 12);
    while (len > 0 && title[len-1] == ' ')
        len--;
    lua_pushlstring(L, title, len);
    return 1;
}

static int ROM_Path(lua_State* L)
{
    QString path = LuaHost::ROMPath(Inst(L));
    if (!Cart(L) || path.isEmpty()) { lua_pushnil(L); return 1; }

    QByteArray utf8 = path.toUtf8();
    lua_pushlstring(L, utf8.constData(), utf8.size());
    return 1;
}

static int ROM_Size(lua_State* L)
{
    NDSCart::CartCommon* cart = Cart(L);
    if (!cart) { lua_pushnil(L); return 1; }

    lua_pushinteger(L, cart->GetROMLength());
    return 1;
}

static int ROM_Bytes(lua_State* L)
{
    lua_Integer offset = luaL_checkinteger(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);

    NDSCart::CartCommon* cart = Cart(L);
    if (!cart) { lua_pushnil(L); return 1; }

    lua_Integer len = cart->GetROMLength();
    if (offset < 0 || n < 0 || offset > len || n > len - offset)
        return luaL_error(L, "rom.bytes: range 0x%I+%I is outside the ROM (size 0x%I)",
                          (LUAI_UACINT)offset, (LUAI_UACINT)n, (LUAI_UACINT)len);

    lua_pushlstring(L, (const char*)cart->GetROM() + offset, (size_t)n);
    return 1;
}

void LuaOpenROM(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"code", ROM_Code},
        {"title", ROM_Title},
        {"path", ROM_Path},
        {"size", ROM_Size},
        {"bytes", ROM_Bytes},
        {nullptr, nullptr}
    };

    SetFuncs(L, funcs, "rom");
}


// ---- pad ----

// EmuInstance::buttonNames gives bit order: A B Select Start Right Left Up Down R L X Y.
static int ButtonIndex(const char* name)
{
    for (int i = 0; i < 12; i++)
    {
        if (!strcmp(name, EmuInstance::buttonNames[i]))
            return i;
    }
    return -1;
}

static int Pad_Set(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    u32 press = 0, release = 0;

    lua_pushnil(L);
    while (lua_next(L, 1))
    {
        if (lua_type(L, -2) != LUA_TSTRING)
            return luaL_error(L, "pad.set: button names must be strings");

        const char* name = lua_tostring(L, -2);
        int btn = ButtonIndex(name);
        if (btn == -1)
            return luaL_error(L, "pad.set: unknown button '%s'", name);
        if (!lua_isboolean(L, -1))
            return luaL_error(L, "pad.set: value for '%s' must be true or false", name);

        if (lua_toboolean(L, -1))
            press |= (1 << btn);
        else
            release |= (1 << btn);

        lua_pop(L, 1);
    }

    LuaScript* s = LuaGetScript(L);
    s->PadForcePress = press;
    s->PadForceRelease = release;
    return 0;
}

static int Pad_Clear(lua_State* L)
{
    LuaScript* s = LuaGetScript(L);
    s->PadForcePress = 0;
    s->PadForceRelease = 0;
    return 0;
}

static int Pad_Get(lua_State* L)
{
    u32 mask = LuaHost::PhysicalButtons(Inst(L));

    lua_createtable(L, 0, 12);
    for (int i = 0; i < 12; i++)
    {
        lua_pushboolean(L, !(mask & (1 << i)));
        lua_setfield(L, -2, EmuInstance::buttonNames[i]);
    }
    return 1;
}

void LuaOpenPad(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"set", Pad_Set},
        {"clear", Pad_Clear},
        {"get", Pad_Get},
        {nullptr, nullptr}
    };

    SetFuncs(L, funcs, "pad");
}


// ---- touch ----

static int Touch_Set(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    luaL_argcheck(L, x >= 0 && x <= 255, 1, "x must be between 0 and 255");
    luaL_argcheck(L, y >= 0 && y <= 191, 2, "y must be between 0 and 191");

    LuaScript* s = LuaGetScript(L);
    s->TouchActive = true;
    s->TouchX = (int)x;
    s->TouchY = (int)y;
    return 0;
}

static int Touch_Release(lua_State* L)
{
    LuaScript* s = LuaGetScript(L);
    s->TouchActive = false;
    return 0;
}

void LuaOpenTouch(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"set", Touch_Set},
        {"release", Touch_Release},
        {nullptr, nullptr}
    };

    SetFuncs(L, funcs, "touch");
}


// ---- input ----

static int Input_Keys(lua_State* L)
{
    QStringList keys = LuaHost::HeldKeys(Inst(L));

    lua_createtable(L, 0, keys.size());
    for (const QString& key : keys)
    {
        QByteArray name = key.toUtf8();
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, name.constData());
    }
    return 1;
}

void LuaOpenInput(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"keys", Input_Keys},
        {nullptr, nullptr}
    };

    SetFuncs(L, funcs, "input");
}
