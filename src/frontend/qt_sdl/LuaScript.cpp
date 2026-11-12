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

#include <stdio.h>
#include <string.h>

#include <chrono>
#include <deque>
#include <map>

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include "LuaScript.h"
#include "LuaScriptInternal.h"
#include "EmuInstance.h"
#include "Config.h"

using namespace melonDS;

// Scripts get a count hook so a runaway loop can be interrupted. This is how
// many VM instructions run between checks of the abort flag.
const int kLuaHookInterval = 1000;

// How long the stop event handlers of one script may run before they are cut off.
const double kLuaStopTimeout = 1.0;

// Console messages kept for consoles opened late.
const int kLuaBacklogMax = 1000;

const char* kLuaEventNames[luaEvent_Max] =
{
    "frame_start",
    "frame_end",
    "state_loaded",
    "rom_changed",
    "reset",
    "stop",
};

struct LuaModuleReg
{
    const char* Name;
    void (*Open)(lua_State* L);
};

// Every global table the host provides. A new module is one more line here.
const LuaModuleReg kLuaModules[] =
{
    {"emu", LuaOpenEmu},
    {"mem", LuaOpenMem},
    {"rom", LuaOpenROM},
    {"pad", LuaOpenPad},
    {"touch", LuaOpenTouch},
    {"input", LuaOpenInput},
    {"event", LuaOpenEvent},
    {"speech", LuaOpenSpeech},
};

enum
{
    luaRequest_Load,
    luaRequest_Stop,
    luaRequest_Restart,
    luaRequest_Remove,
    luaRequest_StopAll,
};

struct LuaRequest
{
    int Type;
    int ID;
};


class LuaScriptEngine
{
public:
    LuaScriptEngine(LuaScriptManager* mgr, EmuInstance* inst);
    ~LuaScriptEngine();

    // any thread
    int RequestLoad(const QString& path, const QStringList& args);
    void Request(int type, int id);
    QList<LuaScriptInfo> Scripts();
    QList<LuaConsoleMessage> Backlog();
    void ClearBacklog();
    bool HasActiveScripts();
    void Post(int id, int kind, const QString& text);

    // emu thread
    void OnLoopPass();
    void OnFrameStart();
    void OnFrameEnd();
    void QueueEvent(int event);
    void Shutdown();

    // used by the API modules
    EmuInstance* Inst;
    u64 FrameCount = 0;
    int PendingPause = 0;
    double Clock();

    // Queues an event without dispatching it yet. Used for the notifications
    // EmuThread sends from inside its message handler, where it holds its
    // message lock: running Lua there could deadlock on emu.pause().
    void PostEvent(int event) { PendingEvents.push_back(event); }

private:
    LuaScriptManager* Mgr;

    QMutex Lock;
    QList<std::shared_ptr<LuaScriptEntry>> Entries;
    std::deque<LuaRequest> Requests;
    std::deque<LuaConsoleMessage> Messages;
    int NextID = 1;

    // emu thread only
    std::map<int, std::unique_ptr<LuaScript>> Running;
    u64 PassCount = 0;
    std::deque<int> PendingEvents;
    bool Dispatching = false;
    std::chrono::steady_clock::time_point ClockBase;

    std::shared_ptr<LuaScriptEntry> FindEntry(int id);
    void SetState(LuaScriptEntry* entry, int state);

    void ProcessRequests();
    void Start(std::shared_ptr<LuaScriptEntry> entry);
    void Resume(LuaScript* s, int nargs);
    bool RunCallback(LuaScript* s, int funcRef);
    void FireEvent(int event);
    void FlushEvents();
    void Fail(LuaScript* s, lua_State* co);
    void Stop(LuaScript* s, int state, bool fireStop);
    void Close(LuaScript* s);
    void CheckFinished(LuaScript* s);
    void ApplyOverrides();

    static void Hook(lua_State* L, lua_Debug* ar);
    static int Print(lua_State* L);
};


LuaScriptEngine::LuaScriptEngine(LuaScriptManager* mgr, EmuInstance* inst)
    : Inst(inst), Mgr(mgr)
{
    ClockBase = std::chrono::steady_clock::now();
}

LuaScriptEngine::~LuaScriptEngine()
{
    // Normally Shutdown() already ran on the emu thread. If the thread never ran,
    // close what is left without running any more Lua.
    for (auto& [id, s] : Running)
        lua_close(s->L);
    Running.clear();
}

double LuaScriptEngine::Clock()
{
    auto d = std::chrono::steady_clock::now() - ClockBase;
    return std::chrono::duration<double>(d).count();
}

int LuaScriptEngine::RequestLoad(const QString& path, const QStringList& args)
{
    auto entry = std::make_shared<LuaScriptEntry>();
    entry->Path = QFileInfo(path).absoluteFilePath();
    entry->Args = args;
    entry->State = luaScript_Loading;

    int id;
    {
        QMutexLocker locker(&Lock);
        id = NextID++;
        entry->ID = id;
        Entries.append(entry);
        Requests.push_back({luaRequest_Load, id});
    }

    emit Mgr->scriptListChanged();
    emit Mgr->scriptStateChanged(id, luaScript_Loading);
    return id;
}

void LuaScriptEngine::Request(int type, int id)
{
    QMutexLocker locker(&Lock);

    // Raise the abort flag straight away so a script stuck in a loop is
    // interrupted by its count hook without waiting for the emu thread.
    for (auto& entry : Entries)
    {
        if (type == luaRequest_StopAll || entry->ID == id)
            entry->Abort = true;
    }

    Requests.push_back({type, id});
}

QList<LuaScriptInfo> LuaScriptEngine::Scripts()
{
    QMutexLocker locker(&Lock);

    QList<LuaScriptInfo> ret;
    for (auto& entry : Entries)
        ret.append({entry->ID, entry->Path, entry->State});
    return ret;
}

QList<LuaConsoleMessage> LuaScriptEngine::Backlog()
{
    QMutexLocker locker(&Lock);
    return QList<LuaConsoleMessage>(Messages.begin(), Messages.end());
}

void LuaScriptEngine::ClearBacklog()
{
    QMutexLocker locker(&Lock);
    Messages.clear();
}

bool LuaScriptEngine::HasActiveScripts()
{
    QMutexLocker locker(&Lock);
    for (auto& entry : Entries)
    {
        if (entry->State == luaScript_Loading || entry->State == luaScript_Running)
            return true;
    }
    return false;
}

void LuaScriptEngine::Post(int id, int kind, const QString& text)
{
    // mirrored to stdout, which is visible when melonDS runs from a terminal
    QByteArray utf8 = text.toUtf8();
    static const char* kKindNames[] = {"", " error", " info"};
    printf("[lua %d%s] %s\n", id, kKindNames[kind], utf8.constData());
    fflush(stdout);

    {
        QMutexLocker locker(&Lock);
        Messages.push_back({id, kind, text});
        while (Messages.size() > kLuaBacklogMax)
            Messages.pop_front();
    }

    emit Mgr->consoleMessage(id, kind, text);
}

std::shared_ptr<LuaScriptEntry> LuaScriptEngine::FindEntry(int id)
{
    QMutexLocker locker(&Lock);
    for (auto& entry : Entries)
    {
        if (entry->ID == id)
            return entry;
    }
    return nullptr;
}

void LuaScriptEngine::SetState(LuaScriptEntry* entry, int state)
{
    {
        QMutexLocker locker(&Lock);
        if (entry->State == state) return;
        entry->State = state;
    }

    emit Mgr->scriptStateChanged(entry->ID, state);
}


void LuaScriptEngine::ProcessRequests()
{
    for (;;)
    {
        LuaRequest req;
        {
            QMutexLocker locker(&Lock);
            if (Requests.empty()) break;
            req = Requests.front();
            Requests.pop_front();
        }

        if (req.Type == luaRequest_StopAll)
        {
            while (!Running.empty())
                Stop(Running.begin()->second.get(), luaScript_Stopped, true);

            QList<std::shared_ptr<LuaScriptEntry>> entries;
            {
                QMutexLocker locker(&Lock);
                entries = Entries;
            }
            for (auto& entry : entries)
            {
                if (entry->State == luaScript_Loading)
                    SetState(entry.get(), luaScript_Stopped);
            }
            continue;
        }

        std::shared_ptr<LuaScriptEntry> entry = FindEntry(req.ID);
        if (!entry) continue;

        auto it = Running.find(req.ID);
        LuaScript* s = (it != Running.end()) ? it->second.get() : nullptr;

        switch (req.Type)
        {
        case luaRequest_Load:
            if (s) break;
            if (entry->Abort)
                SetState(entry.get(), luaScript_Stopped);
            else
                Start(entry);
            break;

        case luaRequest_Restart:
            if (s) Stop(s, luaScript_Stopped, true);
            entry->Abort = false;
            Post(entry->ID, luaMessage_Info, "Restarting " + entry->Path);
            Start(entry);
            break;

        case luaRequest_Stop:
            if (s)
                Stop(s, luaScript_Stopped, true);
            else if (entry->State == luaScript_Loading)
                SetState(entry.get(), luaScript_Stopped);
            break;

        case luaRequest_Remove:
            if (s) Stop(s, luaScript_Stopped, true);
            {
                QMutexLocker locker(&Lock);
                Entries.removeAll(entry);
            }
            emit Mgr->scriptListChanged();
            break;
        }
    }
}

void LuaScriptEngine::Start(std::shared_ptr<LuaScriptEntry> entry)
{
    SetState(entry.get(), luaScript_Loading);

    lua_State* L = luaL_newstate();
    if (!L)
    {
        Post(entry->ID, luaMessage_Error, "Could not create a Lua state for " + entry->Path);
        SetState(entry.get(), luaScript_Errored);
        return;
    }

    auto script = std::make_unique<LuaScript>();
    LuaScript* s = script.get();
    s->Entry = entry;
    s->Engine = this;
    s->L = L;
    *(LuaScript**)lua_getextraspace(L) = s;

    luaL_openlibs(L);

    lua_pushcfunction(L, Print);
    lua_setglobal(L, "print");

    for (const LuaModuleReg& mod : kLuaModules)
        mod.Open(L);

    // let require find pure Lua modules that sit beside the script
    QString dir = QFileInfo(entry->Path).absolutePath();
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    QByteArray path = (dir + "/?.lua;" + dir + "/?/init.lua;").toUtf8();
    path += lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_pushlstring(L, path.constData(), path.size());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    // Hooks are inherited by every coroutine created from here on, including
    // the ones the script makes itself.
    lua_sethook(L, Hook, LUA_MASKCOUNT, kLuaHookInterval);

    s->MainCo = lua_newthread(L);
    s->MainCoRef = luaL_ref(L, LUA_REGISTRYINDEX);

    Running[entry->ID] = std::move(script);

    QByteArray file = entry->Path.toUtf8();
    if (luaL_loadfilex(s->MainCo, file.constData(), "t") != LUA_OK)
    {
        // nothing has run yet, so there is no traceback worth showing
        Post(entry->ID, luaMessage_Error, QString::fromUtf8(lua_tostring(s->MainCo, -1)));
        Stop(s, luaScript_Errored, false);
        return;
    }

    for (const QString& arg : entry->Args)
    {
        QByteArray a = arg.toUtf8();
        lua_pushlstring(s->MainCo, a.constData(), a.size());
    }

    Post(entry->ID, luaMessage_Info, "Loaded " + entry->Path);
    SetState(entry.get(), luaScript_Running);

    s->MainAlive = true;
    Resume(s, entry->Args.size());
}

void LuaScriptEngine::Resume(LuaScript* s, int nargs)
{
    s->Wait = luaWait_None;
    s->LastResumePass = PassCount;

    bool wasDispatching = Dispatching;
    Dispatching = true;
    int nres = 0;
    int status = lua_resume(s->MainCo, nullptr, nargs, &nres);
    Dispatching = wasDispatching;

    if (s->Entry->Abort)
    {
        Stop(s, luaScript_Stopped, true);
        return;
    }

    if (status == LUA_YIELD)
    {
        lua_pop(s->MainCo, nres);
        // a bare coroutine.yield() from the main chunk behaves like emu.yield()
        if (s->Wait == luaWait_None)
            s->Wait = luaWait_Pass;
    }
    else if (status == LUA_OK)
    {
        lua_settop(s->MainCo, 0);
        s->MainAlive = false;
        CheckFinished(s);
    }
    else
    {
        Fail(s, s->MainCo);
    }
}

// Runs one callback on the script's callback coroutine. Returns false if the
// script was stopped or failed as a result.
bool LuaScriptEngine::RunCallback(LuaScript* s, int funcRef)
{
    if (!s->CallbackCo)
    {
        s->CallbackCo = lua_newthread(s->L);
        s->CallbackCoRef = luaL_ref(s->L, LUA_REGISTRYINDEX);
    }

    lua_State* co = s->CallbackCo;
    lua_rawgeti(co, LUA_REGISTRYINDEX, funcRef);

    bool wasDispatching = Dispatching;
    Dispatching = true;
    int nres = 0;
    int status = lua_resume(co, nullptr, 0, &nres);
    Dispatching = wasDispatching;

    if (status == LUA_OK)
    {
        lua_settop(co, 0);
        if (s->Entry->Abort && !s->Stopping)
        {
            Stop(s, luaScript_Stopped, true);
            return false;
        }
        return true;
    }

    if (s->Stopping)
    {
        // a stop handler failed or ran out of time; the caller gives up on the rest
        if (status != LUA_YIELD)
        {
            const char* msg = lua_tostring(co, -1);
            Post(s->Entry->ID, luaMessage_Error,
                 QString("Error in stop handler: ") + (msg ? msg : "(non-string error)"));
        }
        lua_closethread(co, nullptr);
        return false;
    }

    if (s->Entry->Abort)
    {
        lua_closethread(co, nullptr);
        Stop(s, luaScript_Stopped, true);
        return false;
    }

    if (status == LUA_YIELD)
    {
        lua_pop(co, nres);
        luaL_traceback(s->L, co, "event callbacks cannot yield; emu.frameadvance and emu.yield only work in the main script body", 0);
        lua_xmove(s->L, co, 1);
    }

    Fail(s, co);
    return false;
}

void LuaScriptEngine::FireEvent(int event)
{
    // Snapshot the scripts and their callbacks first: callbacks may add or
    // remove callbacks, and scripts may stop as a result of a callback.
    std::vector<int> ids;
    for (auto& [id, s] : Running)
        ids.push_back(id);

    for (int id : ids)
    {
        auto it = Running.find(id);
        if (it == Running.end()) continue;
        LuaScript* s = it->second.get();

        std::vector<int> handles;
        for (const LuaCallback& cb : s->Callbacks)
        {
            if (cb.Event == event)
                handles.push_back(cb.Handle);
        }

        for (int handle : handles)
        {
            int ref = LUA_NOREF;
            for (const LuaCallback& cb : s->Callbacks)
            {
                if (cb.Handle == handle) { ref = cb.FuncRef; break; }
            }
            if (ref == LUA_NOREF) continue;

            if (!RunCallback(s, ref)) break;
        }

        if (Running.count(id))
            CheckFinished(s);
    }
}

void LuaScriptEngine::QueueEvent(int event)
{
    PendingEvents.push_back(event);
    FlushEvents();
}

// Events are never dispatched while Lua code is already running (for example
// state_loaded caused by a script's own emu.loadstate); they wait until control
// is back in the host, then run in order.
void LuaScriptEngine::FlushEvents()
{
    if (Dispatching) return;

    while (!PendingEvents.empty())
    {
        int event = PendingEvents.front();
        PendingEvents.pop_front();
        FireEvent(event);
    }
}

void LuaScriptEngine::Fail(LuaScript* s, lua_State* co)
{
    // the error object is on top of co's stack, which is not unwound yet
    const char* msg = nullptr;
    if (lua_type(co, -1) == LUA_TSTRING)
        msg = lua_tostring(co, -1);
    else
        msg = luaL_tolstring(co, -1, nullptr);

    // errors from RunCallback's yield case already carry a traceback
    QString text;
    if (strstr(msg, "stack traceback:"))
        text = QString::fromUtf8(msg);
    else
    {
        luaL_traceback(s->L, co, msg, 0);
        text = QString::fromUtf8(lua_tostring(s->L, -1));
        lua_pop(s->L, 1);
    }

    Post(s->Entry->ID, luaMessage_Error, text);
    Stop(s, luaScript_Errored, false);
}

void LuaScriptEngine::Stop(LuaScript* s, int state, bool fireStop)
{
    if (fireStop && !s->Stopping)
    {
        s->Stopping = true;
        s->StopDeadline = Clock() + kLuaStopTimeout;

        std::vector<int> refs;
        for (const LuaCallback& cb : s->Callbacks)
        {
            if (cb.Event == luaEvent_Stop)
                refs.push_back(cb.FuncRef);
        }
        for (int ref : refs)
        {
            if (!RunCallback(s, ref)) break;
        }
    }

    int id = s->Entry->ID;
    std::shared_ptr<LuaScriptEntry> entry = s->Entry;
    Close(s);

    if (state == luaScript_Stopped)
        Post(id, luaMessage_Info, "Stopped " + entry->Path);
    else if (state == luaScript_Finished)
        Post(id, luaMessage_Info, "Finished " + entry->Path);

    SetState(entry.get(), state);
}

void LuaScriptEngine::Close(LuaScript* s)
{
    int id = s->Entry->ID;

    // __gc metamethods run during lua_close; give them the same time limit
    // as stop handlers
    s->Stopping = true;
    s->StopDeadline = Clock() + kLuaStopTimeout;
    lua_close(s->L);
    s->L = nullptr;

    // dropping the script drops its input overrides with it
    Running.erase(id);
}

void LuaScriptEngine::CheckFinished(LuaScript* s)
{
    if (s->MainAlive || !s->Callbacks.empty()) return;
    Stop(s, luaScript_Finished, false);
}

void LuaScriptEngine::ApplyOverrides()
{
    u32 press = 0, release = 0;
    bool touch = false;
    int tx = 0, ty = 0;

    // scripts apply in load order, so a later script wins over an earlier one
    for (auto& [id, s] : Running)
    {
        press = (press & ~s->PadForceRelease) | s->PadForcePress;
        release = (release & ~s->PadForcePress) | s->PadForceRelease;

        if (s->TouchActive)
        {
            touch = true;
            tx = s->TouchX;
            ty = s->TouchY;
        }
    }

    LuaHost::ApplyOverrides(Inst, press, release, touch, tx, ty);
}

void LuaScriptEngine::OnLoopPass()
{
    PassCount++;

    // script pause requests queued last pass have been handled by now
    PendingPause = 0;

    FlushEvents();
    ProcessRequests();

    std::vector<int> ids;
    for (auto& [id, s] : Running)
        ids.push_back(id);

    for (int id : ids)
    {
        auto it = Running.find(id);
        if (it == Running.end()) continue;
        LuaScript* s = it->second.get();

        if (s->MainAlive && s->Wait == luaWait_Pass && s->LastResumePass != PassCount)
            Resume(s, 0);

        FlushEvents();
    }
}

void LuaScriptEngine::OnFrameStart()
{
    std::vector<int> ids;
    for (auto& [id, s] : Running)
        ids.push_back(id);

    for (int id : ids)
    {
        auto it = Running.find(id);
        if (it == Running.end()) continue;
        LuaScript* s = it->second.get();

        // only once a frame has actually run since emu.frameadvance was
        // called, which matters when it was called while paused
        if (s->MainAlive && s->Wait == luaWait_Frame && FrameCount > s->WaitFrame
            && s->LastResumePass != PassCount)
            Resume(s, 0);

        FlushEvents();
    }

    QueueEvent(luaEvent_FrameStart);
    ApplyOverrides();
}

void LuaScriptEngine::OnFrameEnd()
{
    FrameCount++;
    QueueEvent(luaEvent_FrameEnd);
}

void LuaScriptEngine::Shutdown()
{
    // events that were still queued would refer to a console that is going away
    PendingEvents.clear();

    while (!Running.empty())
        Stop(Running.begin()->second.get(), luaScript_Stopped, true);

    ApplyOverrides();
}

void LuaScriptEngine::Hook(lua_State* L, lua_Debug* ar)
{
    LuaScript* s = LuaGetScript(L);

    if (s->Stopping)
    {
        if (s->Engine->Clock() < s->StopDeadline) return;
    }
    else if (!s->Entry->Abort.load(std::memory_order_relaxed))
        return;

    // Yielding escapes any pcall the script wraps around its loop. Where a
    // yield is impossible (inside a C call boundary) raise an error instead;
    // once it unwinds to Lua code the next hook yields.
    if (lua_isyieldable(L))
        lua_yield(L, 0);
    else
        luaL_error(L, s->Stopping ? "stop handler took too long" : "script stopped");
}

int LuaScriptEngine::Print(lua_State* L)
{
    LuaScript* s = LuaGetScript(L);

    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++)
    {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);

    size_t len;
    const char* str = lua_tolstring(L, -1, &len);
    s->Engine->Post(s->Entry->ID, luaMessage_Output, QString::fromUtf8(str, len));
    return 0;
}


// ---- event module ----

static int Event_On(lua_State* L)
{
    LuaScript* s = LuaGetScript(L);

    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int event = -1;
    for (int i = 0; i < luaEvent_Max; i++)
    {
        if (!strcmp(name, kLuaEventNames[i]))
        {
            event = i;
            break;
        }
    }
    if (event == -1)
        return luaL_argerror(L, 1, lua_pushfstring(L, "unknown event '%s'", name));

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    int handle = s->NextHandle++;
    s->Callbacks.push_back({handle, event, ref});

    lua_pushinteger(L, handle);
    return 1;
}

static int Event_Off(lua_State* L)
{
    LuaScript* s = LuaGetScript(L);
    int handle = (int)luaL_checkinteger(L, 1);

    bool found = false;
    for (auto it = s->Callbacks.begin(); it != s->Callbacks.end(); it++)
    {
        if (it->Handle == handle)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, it->FuncRef);
            s->Callbacks.erase(it);
            found = true;
            break;
        }
    }

    lua_pushboolean(L, found);
    return 1;
}

void LuaOpenEvent(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"on", Event_On},
        {"off", Event_Off},
        {nullptr, nullptr}
    };

    luaL_newlib(L, funcs);
    lua_setglobal(L, "event");
}


// ---- engine services for the API modules ----

u64 LuaEngineFrameCount(LuaScriptEngine* engine) { return engine->FrameCount; }
double LuaEngineClock(LuaScriptEngine* engine) { return engine->Clock(); }
int& LuaEnginePendingPause(LuaScriptEngine* engine) { return engine->PendingPause; }
EmuInstance* LuaEngineInstance(LuaScriptEngine* engine) { return engine->Inst; }
void LuaEngineQueueEvent(LuaScriptEngine* engine, int event) { engine->QueueEvent(event); }
void LuaEnginePost(LuaScriptEngine* engine, int scriptID, int kind, const QString& text) { engine->Post(scriptID, kind, text); }


// ---- LuaScriptManager ----

struct LuaScriptManager::Private
{
    LuaScriptEngine Engine;

    Private(LuaScriptManager* mgr, EmuInstance* inst) : Engine(mgr, inst) {}
};

LuaScriptManager::LuaScriptManager(EmuInstance* inst) : QObject(nullptr), emuInstance(inst), d(new Private(this, inst))
{
}

LuaScriptManager::~LuaScriptManager()
{
    delete d;
}

int LuaScriptManager::loadScript(const QString& path, const QStringList& args)
{
    return d->Engine.RequestLoad(path, args);
}

void LuaScriptManager::stopScript(int id)
{
    d->Engine.Request(luaRequest_Stop, id);
}

void LuaScriptManager::restartScript(int id)
{
    d->Engine.Request(luaRequest_Restart, id);
}

void LuaScriptManager::removeScript(int id)
{
    d->Engine.Request(luaRequest_Remove, id);
}

void LuaScriptManager::stopAll()
{
    d->Engine.Request(luaRequest_StopAll, -1);
}

QList<LuaScriptInfo> LuaScriptManager::scripts()
{
    return d->Engine.Scripts();
}

QList<LuaConsoleMessage> LuaScriptManager::backlog()
{
    return d->Engine.Backlog();
}

void LuaScriptManager::clearBacklog()
{
    d->Engine.ClearBacklog();
}

QStringList LuaScriptManager::autoloadScripts()
{
    Config::Array arr = emuInstance->getLocalConfig().GetArray("Lua.AutoloadScripts");

    QStringList ret;
    for (size_t i = 0; i < arr.Size(); i++)
    {
        QString path = arr.GetQString(i);
        if (!path.isEmpty())
            ret.append(path);
    }
    return ret;
}

void LuaScriptManager::setAutoloadScripts(const QStringList& paths)
{
    Config::Array arr = emuInstance->getLocalConfig().GetArray("Lua.AutoloadScripts");
    arr.Clear();
    for (int i = 0; i < paths.size(); i++)
        arr.SetQString(i, paths[i]);

    Config::Save();
}

bool LuaScriptManager::hasActiveScripts()
{
    return d->Engine.HasActiveScripts();
}

void LuaScriptManager::onLoopPass()
{
    d->Engine.OnLoopPass();
}

void LuaScriptManager::onFrameStart()
{
    d->Engine.OnFrameStart();
}

void LuaScriptManager::onFrameEnd()
{
    d->Engine.OnFrameEnd();
}

// These three are called from EmuThread's message handler. The events are
// dispatched at the start of the next loop pass, a moment later.

void LuaScriptManager::onStateLoaded()
{
    d->Engine.PostEvent(luaEvent_StateLoaded);
}

void LuaScriptManager::onROMChanged()
{
    d->Engine.PostEvent(luaEvent_ROMChanged);
}

void LuaScriptManager::onReset()
{
    d->Engine.PostEvent(luaEvent_Reset);
}

void LuaScriptManager::shutdown()
{
    d->Engine.Shutdown();
}
