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

#ifndef LUASCRIPT_H
#define LUASCRIPT_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>

class EmuInstance;

// Lua scripting host for one emulator instance.
//
// Threading model: every Lua state lives on the instance's EmuThread and is only
// ever touched there. Each loaded script gets its own lua_State, and its main
// chunk runs as a coroutine. The UI thread never touches a lua_State; it talks to
// the manager through the request methods below, which only queue work that the
// emu thread picks up on its next loop pass, and it hears back through signals
// (emitted on the emu thread, so receivers on the UI thread get them queued).
//
// Scheduling: a script coroutine runs until it calls emu.frameadvance() or
// emu.yield(), returns, or raises an error.
// - emu.frameadvance() suspends it until the next emulated frame is about to run.
//   It is resumed from onFrameStart(), before input is applied to the console,
//   so input it sets takes effect on that frame.
// - emu.yield() suspends it until the next pass of the emu loop, whether the
//   emulator is running or paused. This is what keeps hotkeys and speech
//   working while paused.
// - A coroutine is resumed at most once per loop pass.
// - A script whose main chunk returns stays loaded as long as it has event
//   callbacks registered; otherwise it becomes luaScript_Finished.

enum
{
    luaScript_Loading = 0,  // requested, not yet started on the emu thread
    luaScript_Running,      // main coroutine alive, or finished with callbacks registered
    luaScript_Finished,     // main chunk returned and no callbacks remain
    luaScript_Errored,      // stopped by an uncaught error
    luaScript_Stopped,      // stopped on request
};

enum
{
    luaMessage_Output = 0,  // print() and friends
    luaMessage_Error,       // uncaught error, text includes a traceback
    luaMessage_Info,        // host notices: loaded, stopped, and so on
};

struct LuaScriptInfo
{
    int id;
    QString path;
    int state;      // luaScript_*
};

struct LuaConsoleMessage
{
    int scriptID;   // -1 for messages not tied to one script
    int kind;       // luaMessage_*
    QString text;
};

class LuaScriptManager : public QObject
{
    Q_OBJECT

public:
    explicit LuaScriptManager(EmuInstance* inst);
    ~LuaScriptManager();

    // ---- callable from any thread ----

    // Queues a script for loading and returns its ID straight away. The script
    // is loaded as a file chunk, so debug.getinfo reports "@" plus the absolute
    // path, and args are passed to the main chunk as its varargs.
    int loadScript(const QString& path, const QStringList& args = QStringList());

    // Stops a script: fires its stop event, clears its input overrides, closes
    // its lua_State. The entry stays in the list so it can be restarted.
    void stopScript(int id);

    // Stops the script if needed and loads it again from disk under the same ID.
    void restartScript(int id);

    // Stops the script if needed and drops it from the list.
    void removeScript(int id);

    void stopAll();

    // Snapshot of the script list.
    QList<LuaScriptInfo> scripts();

    // Recent console messages (bounded), oldest first, so a console opened late
    // can show what it missed.
    QList<LuaConsoleMessage> backlog();
    void clearBacklog();

    // Scripts loaded automatically when this instance starts. Stored in the
    // instance's config.
    QStringList autoloadScripts();
    void setAutoloadScripts(const QStringList& paths);

    // True while any script is loading or running. The emu thread uses this to
    // shorten its sleep while paused.
    bool hasActiveScripts();

    // ---- emu thread only, called from EmuThread ----

    // Once per pass of the emu loop, running or paused, before anything else:
    // applies queued requests and resumes scripts waiting in emu.yield().
    void onLoopPass();

    // Just before a frame is emulated and before input is applied: resumes
    // scripts waiting in emu.frameadvance(), then fires frame_start.
    void onFrameStart();

    // Right after a frame has been emulated: fires frame_end.
    void onFrameEnd();

    void onStateLoaded();
    void onROMChanged();
    void onReset();

    // The emu thread is about to exit: stops every script.
    void shutdown();

signals:
    void consoleMessage(int scriptID, int kind, QString text);
    void scriptStateChanged(int scriptID, int state);
    void scriptListChanged();

private:
    EmuInstance* emuInstance;

    struct Private;
    Private* d;
};

#endif // LUASCRIPT_H
