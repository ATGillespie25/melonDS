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

#ifndef LUACONSOLEDIALOG_H
#define LUACONSOLEDIALOG_H

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>

class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class EmuInstance;
class LuaScriptManager;

// Console window for the Lua scripting host: load, stop, restart and remove
// scripts, choose which ones load at startup, and read their output and errors.
//
// There is at most one console per main window. Each console talks to the
// LuaScriptManager of its own window's emulator instance, so several instances
// can each have their own console open at once. The manager outlives the
// dialog; the dialog only listens to its signals and calls its request methods.
//
// Everything is built from standard Qt widgets so it works with screen readers.
class LuaConsoleDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LuaConsoleDialog(QWidget* parent);
    ~LuaConsoleDialog();

    // Shows the console belonging to the given main window, creating it if
    // that window does not have one yet. Unlike the other dialogs this does not
    // use a static pointer, because one static would be shared by every
    // emulator instance in the process.
    static LuaConsoleDialog* openDlg(QWidget* parent);

private slots:
    void done(int r) override;

    void onConsoleMessage(int scriptID, int kind, QString text);
    void onScriptStateChanged(int scriptID, int state);
    void onScriptListChanged();

    void onSelectionChanged();
    void onLoadScript();
    void onStopScript();
    void onRestartScript();
    void onRemoveScript();
    void onStopAll();
    void onAutoloadClicked(bool checked);
    void onClearOutput();
    void onCopyOutput();

private:
    EmuInstance* emuInstance;
    LuaScriptManager* luaManager;

    // Set once the dialog has been asked to close, so openDlg does not hand
    // back a dialog that is about to be deleted.
    bool closing;

    QListWidget* scriptList;
    QPushButton* btnLoad;
    QPushButton* btnStop;
    QPushButton* btnRestart;
    QPushButton* btnRemove;
    QCheckBox* cbAutoload;
    QPushButton* btnStopAll;
    QPlainTextEdit* output;
    QPushButton* btnClear;
    QPushButton* btnCopy;
    QPushButton* btnClose;

    // Script paths and states as last seen, keyed by script ID. Paths are kept
    // after a script is removed so late messages from it can still be labelled.
    QHash<int, QString> scriptPaths;
    QHash<int, int> scriptStates;

    QStringList autoloadPaths;

    // ID of a script we just asked to load, to select it once it shows up.
    int pendingSelectID;

    void buildUI();

    QListWidgetItem* findItem(int scriptID);
    int selectedScriptID();
    bool isAutoload(const QString& path);
    QString scriptName(int scriptID);
    QString itemText(int scriptID);
    void refreshItem(QListWidgetItem* item);
    void syncScriptList();
    void updateButtons();

    void appendMessage(int scriptID, int kind, const QString& text);
    void announce(const QString& text);
};

#endif // LUACONSOLEDIALOG_H
