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

// The speech module of the Lua scripting host: speech.say, speech.stop and
// speech.available, spoken through the user's screen reader or a system voice.
//
// On Windows this uses Prism (https://github.com/ethindp/prism, MPL-2.0)
// through its C API, loaded at run time from prism.dll beside the executable.
// There is no import library and no Prism header here: the few types and
// function pointers needed are declared below, so melonDS builds and runs
// without Prism and speech just reports itself unavailable. Elsewhere speech
// is always unavailable for now.
//
// One Prism context and one backend are shared by every script in every
// instance. A backend is not thread-safe and each instance runs scripts on its
// own emu thread, so every Prism call is made under one mutex.

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>

#include <QString>

#include "LuaScriptInternal.h"
#include "LuaScriptSpeech.h"
#include "LuaScript.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace LuaSpeech
{

#ifdef _WIN32

// ---- the parts of prism.h used here ----

struct PrismContext;
struct PrismBackend;
typedef uint64_t PrismBackendId;
typedef int PrismError;

const PrismError kPrismOK = 0;
const PrismError kPrismAlreadyInitialized = 15;
const uint64_t kPrismSupportsStop = 1ULL << 7;

typedef void (__cdecl *PrismAvailabilityCallback)(void* userdata, PrismBackendId backend, const char* name, bool available);

// PrismConfig as of version 4. Later versions only append fields, and
// prism_config_init returns the structure by value into caller memory, so the
// spare room at the end keeps a newer DLL from writing past it.
struct PrismConfig
{
    uint8_t version;
    void* registry;
    PrismAvailabilityCallback availability_callback;
    void* availability_userdata;
    uint32_t availability_poll_interval_ms;
    uint32_t availability_debounce_samples;
    uint32_t availability_backoff_max_ms;
    bool availability_auto_power_manage;
    void* availability_baseline_callback;
    uint8_t spare[256];
};

typedef PrismConfig (__cdecl *FnConfigInit)();
typedef PrismContext* (__cdecl *FnInit)(PrismConfig* cfg);
typedef void (__cdecl *FnShutdown)(PrismContext* ctx);
typedef PrismBackend* (__cdecl *FnCreateBest)(PrismContext* ctx);
typedef PrismBackend* (__cdecl *FnCreate)(PrismContext* ctx, PrismBackendId id);
typedef PrismBackendId (__cdecl *FnRegistryID)(PrismContext* ctx, const char* name);
typedef bool (__cdecl *FnRegistryExists)(PrismContext* ctx, PrismBackendId id);
typedef size_t (__cdecl *FnRegistryCount)(PrismContext* ctx);
typedef PrismBackendId (__cdecl *FnRegistryIDAt)(PrismContext* ctx, size_t index);
typedef const char* (__cdecl *FnRegistryName)(PrismContext* ctx, PrismBackendId id);
typedef PrismError (__cdecl *FnInitialize)(PrismBackend* backend);
typedef PrismError (__cdecl *FnSpeak)(PrismBackend* backend, const char* text, bool interrupt);
typedef PrismError (__cdecl *FnStop)(PrismBackend* backend);
typedef const char* (__cdecl *FnName)(PrismBackend* backend);
typedef uint64_t (__cdecl *FnFeatures)(PrismBackend* backend);
typedef void (__cdecl *FnFree)(PrismBackend* backend);
typedef const char* (__cdecl *FnErrorString)(PrismError error);

struct PrismAPI
{
    FnConfigInit ConfigInit;
    FnInit Init;
    FnShutdown Shutdown;
    FnCreateBest CreateBest;
    FnCreate Create;
    FnRegistryID RegistryID;
    FnRegistryExists RegistryExists;
    FnRegistryCount RegistryCount;
    FnRegistryIDAt RegistryIDAt;
    FnRegistryName RegistryName;
    FnInitialize Initialize;
    FnSpeak Speak;
    FnStop Stop;
    FnName Name;
    FnFeatures Features;
    FnFree Free;
    FnErrorString ErrorString;
};


// ---- shared state, guarded by Lock (except Reselect) ----

std::mutex Lock;
bool LoadTried = false;
bool ShutDown = false;
HMODULE Library = nullptr;
PrismAPI API {};
PrismContext* Context = nullptr;
PrismBackend* Backend = nullptr;
DWORD LastSelect = 0;

// set when MELONDS_SPEECH_BACKEND names a backend: no automatic re-selection
bool Override = false;
PrismBackendId OverrideID = 0;

// set by Prism's availability thread when a screen reader starts or exits
std::atomic<bool> Reselect {false};

// one-time notices for the console, handed out by TakeNotice
std::string Notice;

// how long to wait before trying again when no backend could be found
const DWORD kRetryMs = 3000;

void __cdecl OnAvailability(void*, PrismBackendId, const char*, bool)
{
    // Prism's own thread: only note it, the next speech call re-selects
    Reselect = true;
}

std::string ErrorText(PrismError err)
{
    const char* s = API.ErrorString ? API.ErrorString(err) : nullptr;
    return s ? s : ("error " + std::to_string(err));
}

void FreeBackend()
{
    if (Backend)
    {
        API.Free(Backend);
        Backend = nullptr;
    }
}

void Select()
{
    FreeBackend();
    LastSelect = GetTickCount();

    if (!Override)
    {
        Backend = API.CreateBest(Context);
        return;
    }

    if (!OverrideID) return;

    Backend = API.Create(Context, OverrideID);
    if (!Backend) return;

    PrismError err = API.Initialize(Backend);
    if (err != kPrismOK && err != kPrismAlreadyInitialized)
    {
        Notice = "speech: the backend chosen by MELONDS_SPEECH_BACKEND failed to start (" + ErrorText(err) + ")";
        FreeBackend();
    }
}

template <typename T> bool Resolve(T& fn, const char* name)
{
    fn = (T)(void*)GetProcAddress(Library, name);
    return fn != nullptr;
}

void Load()
{
    LoadTried = true;

    // prism.dll beside melonDS.exe, by full path, so it is never picked up
    // from anywhere else
    wchar_t exe[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    if (len == 0 || len >= MAX_PATH * 4) return;
    std::wstring path(exe, len);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"prism.dll";

    Library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Library)
    {
        Notice = "speech: prism.dll was not found beside melonDS, so speech is unavailable";
        return;
    }

    bool ok = Resolve(API.ConfigInit, "prism_config_init")
        && Resolve(API.Init, "prism_init")
        && Resolve(API.Shutdown, "prism_shutdown")
        && Resolve(API.CreateBest, "prism_registry_create_best")
        && Resolve(API.Create, "prism_registry_create")
        && Resolve(API.RegistryID, "prism_registry_id")
        && Resolve(API.RegistryExists, "prism_registry_exists")
        && Resolve(API.RegistryCount, "prism_registry_count")
        && Resolve(API.RegistryIDAt, "prism_registry_id_at")
        && Resolve(API.RegistryName, "prism_registry_name")
        && Resolve(API.Initialize, "prism_backend_initialize")
        && Resolve(API.Speak, "prism_backend_speak")
        && Resolve(API.Stop, "prism_backend_stop")
        && Resolve(API.Name, "prism_backend_name")
        && Resolve(API.Features, "prism_backend_get_features")
        && Resolve(API.Free, "prism_backend_free")
        && Resolve(API.ErrorString, "prism_error_string");
    if (!ok)
    {
        Notice = "speech: prism.dll is missing functions melonDS needs, so speech is unavailable";
        FreeLibrary(Library);
        Library = nullptr;
        return;
    }

    // Ask Prism to watch for screen readers starting and exiting. Its
    // callback fields exist from version 3 of the structure on.
    PrismConfig cfg = API.ConfigInit();
    if (cfg.version >= 3)
    {
        cfg.availability_callback = OnAvailability;
        cfg.availability_userdata = nullptr;
        cfg.availability_backoff_max_ms = 4000;
    }
    Context = API.Init(&cfg);
    if (!Context)
        Context = API.Init(nullptr);
    if (!Context)
    {
        Notice = "speech: Prism failed to initialise, so speech is unavailable";
        FreeLibrary(Library);
        Library = nullptr;
        return;
    }

    // MELONDS_SPEECH_BACKEND forces one backend by its Prism name
    const wchar_t* forced = _wgetenv(L"MELONDS_SPEECH_BACKEND");
    if (forced && *forced)
    {
        Override = true;
        std::string name = QString::fromWCharArray(forced).toStdString();
        OverrideID = API.RegistryID(Context, name.c_str());
        if (!OverrideID || !API.RegistryExists(Context, OverrideID))
        {
            OverrideID = 0;
            std::string known;
            size_t count = API.RegistryCount(Context);
            for (size_t i = 0; i < count; i++)
            {
                const char* n = API.RegistryName(Context, API.RegistryIDAt(Context, i));
                if (!n) continue;
                if (!known.empty()) known += ", ";
                known += n;
            }
            Notice = "speech: MELONDS_SPEECH_BACKEND names '" + name + "', which this prism.dll does not have, so speech is unavailable. It has: " + known;
        }
    }

    Select();
}

// Under Lock. True when a backend is ready to use.
bool Ready()
{
    if (ShutDown) return false;
    if (!LoadTried) Load();
    if (!Context) return false;

    if (!Override)
    {
        if (Reselect.exchange(false))
            Select();
        else if (!Backend && GetTickCount() - LastSelect >= kRetryMs)
            Select();
    }

    return Backend != nullptr;
}

bool Say(const char* text, bool interrupt)
{
    std::lock_guard<std::mutex> guard(Lock);
    if (!Ready()) return false;

    PrismError err = API.Speak(Backend, text, interrupt);
    if (err != kPrismOK && !Override)
    {
        // the screen reader may have gone away; pick again and retry once
        Select();
        if (Backend)
            err = API.Speak(Backend, text, interrupt);
    }
    return err == kPrismOK;
}

bool Stop()
{
    std::lock_guard<std::mutex> guard(Lock);
    if (!Ready()) return false;
    if (!(API.Features(Backend) & kPrismSupportsStop)) return false;
    return API.Stop(Backend) == kPrismOK;
}

bool Available(std::string& name)
{
    std::lock_guard<std::mutex> guard(Lock);
    if (!Ready()) return false;
    const char* n = API.Name(Backend);
    name = n ? n : "";
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> guard(Lock);
    ShutDown = true;
    FreeBackend();
    if (Context)
    {
        API.Shutdown(Context);
        Context = nullptr;
    }
    if (Library)
    {
        FreeLibrary(Library);
        Library = nullptr;
    }
}

#else // !_WIN32

std::mutex Lock;
std::string Notice;
bool NoticeGiven = false;

bool Say(const char*, bool) { return false; }
bool Stop() { return false; }

bool Available(std::string&)
{
    std::lock_guard<std::mutex> guard(Lock);
    if (!NoticeGiven)
    {
        NoticeGiven = true;
        Notice = "speech: not supported on this platform yet";
    }
    return false;
}

void Shutdown() {}

#endif

bool TakeNotice(std::string& out)
{
    std::lock_guard<std::mutex> guard(Lock);
    if (Notice.empty()) return false;
    out.swap(Notice);
    Notice.clear();
    return true;
}

}


// ---- Lua module ----

static void PostNotice(lua_State* L)
{
    std::string notice;
    if (LuaSpeech::TakeNotice(notice))
    {
        LuaScript* s = LuaGetScript(L);
        LuaEnginePost(s->Engine, s->Entry->ID, luaMessage_Info, QString::fromStdString(notice));
    }
}

static int Speech_Say(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    bool interrupt = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);

    bool ok = LuaSpeech::Say(text, interrupt);
    PostNotice(L);
    lua_pushboolean(L, ok);
    return 1;
}

static int Speech_Stop(lua_State* L)
{
    bool ok = LuaSpeech::Stop();
    PostNotice(L);
    lua_pushboolean(L, ok);
    return 1;
}

static int Speech_Available(lua_State* L)
{
    std::string name;
    bool ok = LuaSpeech::Available(name);
    PostNotice(L);

    lua_pushboolean(L, ok);
    if (!ok) return 1;
    lua_pushlstring(L, name.data(), name.size());
    return 2;
}

void LuaOpenSpeech(lua_State* L)
{
    static const luaL_Reg funcs[] =
    {
        {"say", Speech_Say},
        {"stop", Speech_Stop},
        {"available", Speech_Available},
        {nullptr, nullptr}
    };

    luaL_newlib(L, funcs);
    lua_setglobal(L, "speech");
}
