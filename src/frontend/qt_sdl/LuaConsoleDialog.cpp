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

#include <QAccessible>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QTextCursor>
#include <QVBoxLayout>

#include "LuaConsoleDialog.h"
#include "LuaScript.h"
#include "EmuInstance.h"
#include "main.h"

// Lines kept in the output area. Older lines are dropped from the top.
const int kMaxOutputLines = 10000;

// Folder the load dialog starts in. Shared by all consoles for this session.
static QString lastScriptDir;

static QString scriptStateText(int state)
{
    switch (state)
    {
    case luaScript_Loading: return "loading";
    case luaScript_Running: return "running";
    case luaScript_Finished: return "finished";
    case luaScript_Errored: return "stopped by an error";
    case luaScript_Stopped: return "stopped";
    default: return "unknown state";
    }
}

static bool samePath(const QString& a, const QString& b)
{
    QString ca = QDir::cleanPath(QFileInfo(a).absoluteFilePath());
    QString cb = QDir::cleanPath(QFileInfo(b).absoluteFilePath());
#ifdef Q_OS_WIN
    return ca.compare(cb, Qt::CaseInsensitive) == 0;
#else
    return ca == cb;
#endif
}


LuaConsoleDialog* LuaConsoleDialog::openDlg(QWidget* parent)
{
    // Look for a console already attached to this particular window. A dialog
    // that has been closed but not yet deleted is skipped.
    const QList<LuaConsoleDialog*> existing = parent->findChildren<LuaConsoleDialog*>(QString(), Qt::FindDirectChildrenOnly);
    for (LuaConsoleDialog* dlg : existing)
    {
        if (dlg->closing)
            continue;

        dlg->show();
        dlg->raise();
        dlg->activateWindow();
        return dlg;
    }

    LuaConsoleDialog* dlg = new LuaConsoleDialog(parent);
    dlg->show();
    return dlg;
}

LuaConsoleDialog::LuaConsoleDialog(QWidget* parent) : QDialog(parent), closing(false), pendingSelectID(-1)
{
    emuInstance = ((MainWindow*)parent)->getEmuInstance();
    luaManager = emuInstance->getLuaManager();

    setAttribute(Qt::WA_DeleteOnClose);

    int instID = emuInstance->getInstanceID();
    if (instID > 0)
        setWindowTitle(QString("Lua console - melonDS instance %1").arg(instID + 1));
    else
        setWindowTitle("Lua console - melonDS");

    buildUI();

    // The manager emits from the emu thread. Queued connections make sure the
    // slots run on the UI thread, and they are dropped automatically if this
    // dialog is deleted first. Connect before taking the snapshots below so
    // nothing emitted in between is lost; at worst a message shows up twice.
    connect(luaManager, &LuaScriptManager::consoleMessage, this, &LuaConsoleDialog::onConsoleMessage, Qt::QueuedConnection);
    connect(luaManager, &LuaScriptManager::scriptStateChanged, this, &LuaConsoleDialog::onScriptStateChanged, Qt::QueuedConnection);
    connect(luaManager, &LuaScriptManager::scriptListChanged, this, &LuaConsoleDialog::onScriptListChanged, Qt::QueuedConnection);

    syncScriptList();
    if (scriptList->count() > 0 && !scriptList->currentItem())
        scriptList->setCurrentRow(0);

    const QList<LuaConsoleMessage> backlog = luaManager->backlog();
    for (const LuaConsoleMessage& msg : backlog)
        appendMessage(msg.scriptID, msg.kind, msg.text);
    output->moveCursor(QTextCursor::End);

    updateButtons();
    scriptList->setFocus();
}

LuaConsoleDialog::~LuaConsoleDialog()
{
}

void LuaConsoleDialog::buildUI()
{
    resize(640, 480);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // Script list and its actions.

    QLabel* lblScripts = new QLabel("&Scripts:", this);
    mainLayout->addWidget(lblScripts);

    scriptList = new QListWidget(this);
    scriptList->setSelectionMode(QAbstractItemView::SingleSelection);
    scriptList->setAccessibleName("Scripts");
    scriptList->setAccessibleDescription("Delete removes the selected script. F5 restarts it.");
    lblScripts->setBuddy(scriptList);
    mainLayout->addWidget(scriptList, 1);

    QLabel* lblKeys = new QLabel("In the script list, Delete removes the selected script and F5 restarts it.", this);
    lblKeys->setWordWrap(true);
    mainLayout->addWidget(lblKeys);

    QShortcut* scRemove = new QShortcut(QKeySequence(QKeySequence::Delete), scriptList);
    scRemove->setContext(Qt::WidgetShortcut);
    connect(scRemove, &QShortcut::activated, this, &LuaConsoleDialog::onRemoveScript);

    QShortcut* scRestart = new QShortcut(QKeySequence(Qt::Key_F5), scriptList);
    scRestart->setContext(Qt::WidgetShortcut);
    connect(scRestart, &QShortcut::activated, this, &LuaConsoleDialog::onRestartScript);

    QHBoxLayout* scriptButtons = new QHBoxLayout();

    btnLoad = new QPushButton("&Load script...", this);
    btnStop = new QPushButton("S&top", this);
    btnRestart = new QPushButton("&Restart", this);
    btnRemove = new QPushButton("Re&move", this);
    cbAutoload = new QCheckBox("Load at start&up", this);
    cbAutoload->setAccessibleDescription("Load the selected script automatically when this emulator instance starts.");
    btnStopAll = new QPushButton("Stop &all", this);

    scriptButtons->addWidget(btnLoad);
    scriptButtons->addWidget(btnStop);
    scriptButtons->addWidget(btnRestart);
    scriptButtons->addWidget(btnRemove);
    scriptButtons->addWidget(cbAutoload);
    scriptButtons->addStretch();
    scriptButtons->addWidget(btnStopAll);
    mainLayout->addLayout(scriptButtons);

    // Output area and its actions.

    QLabel* lblOutput = new QLabel("&Output:", this);
    mainLayout->addWidget(lblOutput);

    output = new QPlainTextEdit(this);
    output->setReadOnly(true);
    // Keyboard selection gives the read-only area a caret, so a screen reader
    // can move through it line by line.
    output->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    output->setTabChangesFocus(true);
    output->setUndoRedoEnabled(false);
    output->setMaximumBlockCount(kMaxOutputLines);
    // No wrapping, so each visual line is one real line of output.
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setAccessibleName("Output");
    lblOutput->setBuddy(output);
    mainLayout->addWidget(output, 2);

    QHBoxLayout* outputButtons = new QHBoxLayout();

    btnClear = new QPushButton("&Clear output", this);
    btnCopy = new QPushButton("Co&py all", this);
    btnClose = new QPushButton("Clos&e", this);

    outputButtons->addWidget(btnClear);
    outputButtons->addWidget(btnCopy);
    outputButtons->addStretch();
    outputButtons->addWidget(btnClose);
    mainLayout->addLayout(outputButtons);

    // No button may act on Enter by default, so Enter in the list or the
    // output never triggers something unexpected.
    for (QPushButton* btn : {btnLoad, btnStop, btnRestart, btnRemove, btnStopAll, btnClear, btnCopy, btnClose})
    {
        btn->setAutoDefault(false);
        btn->setDefault(false);
    }

    setTabOrder(scriptList, btnLoad);
    setTabOrder(btnLoad, btnStop);
    setTabOrder(btnStop, btnRestart);
    setTabOrder(btnRestart, btnRemove);
    setTabOrder(btnRemove, cbAutoload);
    setTabOrder(cbAutoload, btnStopAll);
    setTabOrder(btnStopAll, output);
    setTabOrder(output, btnClear);
    setTabOrder(btnClear, btnCopy);
    setTabOrder(btnCopy, btnClose);

    connect(scriptList, &QListWidget::currentItemChanged, this, &LuaConsoleDialog::onSelectionChanged);
    connect(btnLoad, &QPushButton::clicked, this, &LuaConsoleDialog::onLoadScript);
    connect(btnStop, &QPushButton::clicked, this, &LuaConsoleDialog::onStopScript);
    connect(btnRestart, &QPushButton::clicked, this, &LuaConsoleDialog::onRestartScript);
    connect(btnRemove, &QPushButton::clicked, this, &LuaConsoleDialog::onRemoveScript);
    connect(cbAutoload, &QCheckBox::clicked, this, &LuaConsoleDialog::onAutoloadClicked);
    connect(btnStopAll, &QPushButton::clicked, this, &LuaConsoleDialog::onStopAll);
    connect(btnClear, &QPushButton::clicked, this, &LuaConsoleDialog::onClearOutput);
    connect(btnCopy, &QPushButton::clicked, this, &LuaConsoleDialog::onCopyOutput);
    connect(btnClose, &QPushButton::clicked, this, &LuaConsoleDialog::close);
}

void LuaConsoleDialog::done(int r)
{
    closing = true;
    QDialog::done(r);
}


QListWidgetItem* LuaConsoleDialog::findItem(int scriptID)
{
    for (int i = 0; i < scriptList->count(); i++)
    {
        QListWidgetItem* item = scriptList->item(i);
        if (item->data(Qt::UserRole).toInt() == scriptID)
            return item;
    }

    return nullptr;
}

int LuaConsoleDialog::selectedScriptID()
{
    QListWidgetItem* item = scriptList->currentItem();
    if (!item)
        return -1;

    return item->data(Qt::UserRole).toInt();
}

bool LuaConsoleDialog::isAutoload(const QString& path)
{
    for (const QString& p : autoloadPaths)
    {
        if (samePath(p, path))
            return true;
    }

    return false;
}

QString LuaConsoleDialog::scriptName(int scriptID)
{
    if (scriptID < 0)
        return "Lua";

    auto it = scriptPaths.constFind(scriptID);
    if (it == scriptPaths.constEnd())
        return QString("script %1").arg(scriptID);

    return QFileInfo(it.value()).fileName();
}

QString LuaConsoleDialog::itemText(int scriptID)
{
    QString text = scriptName(scriptID) + ", " + scriptStateText(scriptStates.value(scriptID, -1));

    if (isAutoload(scriptPaths.value(scriptID)))
        text += ", loads at startup";

    return text;
}

void LuaConsoleDialog::refreshItem(QListWidgetItem* item)
{
    int id = item->data(Qt::UserRole).toInt();
    item->setText(itemText(id));
    item->setToolTip(QDir::toNativeSeparators(scriptPaths.value(id)));
}

void LuaConsoleDialog::syncScriptList()
{
    // Update the list in place rather than rebuilding it, so the item a screen
    // reader is sitting on keeps its position and selection where possible.

    const QList<LuaScriptInfo> scripts = luaManager->scripts();
    autoloadPaths = luaManager->autoloadScripts();

    int keepID = selectedScriptID();
    int oldRow = scriptList->currentRow();

    for (int i = 0; i < scripts.size(); i++)
    {
        const LuaScriptInfo& info = scripts[i];
        scriptPaths[info.id] = info.path;
        scriptStates[info.id] = info.state;

        QListWidgetItem* item = nullptr;
        for (int j = i; j < scriptList->count(); j++)
        {
            if (scriptList->item(j)->data(Qt::UserRole).toInt() != info.id)
                continue;

            item = scriptList->item(j);
            if (j != i)
            {
                scriptList->takeItem(j);
                scriptList->insertItem(i, item);
            }
            break;
        }

        if (!item)
        {
            item = new QListWidgetItem();
            item->setData(Qt::UserRole, info.id);
            scriptList->insertItem(i, item);
        }

        refreshItem(item);
    }

    while (scriptList->count() > scripts.size())
        delete scriptList->takeItem(scriptList->count() - 1);

    QListWidgetItem* target = nullptr;
    if (pendingSelectID >= 0)
    {
        target = findItem(pendingSelectID);
        if (target)
            pendingSelectID = -1;
    }
    if (!target && keepID >= 0)
        target = findItem(keepID);
    if (!target && scriptList->count() > 0)
        target = scriptList->item(qBound(0, oldRow, scriptList->count() - 1));

    if (target && scriptList->currentItem() != target)
        scriptList->setCurrentItem(target);
}

void LuaConsoleDialog::updateButtons()
{
    bool hasSelection = selectedScriptID() >= 0;

    // If the control that has focus is about to be disabled, put focus on the
    // list first so it lands somewhere predictable instead of jumping ahead.
    if (!hasSelection)
    {
        QWidget* focused = focusWidget();
        if (focused == btnStop || focused == btnRestart || focused == btnRemove || focused == cbAutoload)
            scriptList->setFocus();
    }

    btnStop->setEnabled(hasSelection);
    btnRestart->setEnabled(hasSelection);
    btnRemove->setEnabled(hasSelection);
    cbAutoload->setEnabled(hasSelection);

    // setChecked does not emit clicked, so this never writes the config.
    cbAutoload->setChecked(hasSelection && isAutoload(scriptPaths.value(selectedScriptID())));
}


void LuaConsoleDialog::appendMessage(int scriptID, int kind, const QString& text)
{
    QString body = text;
    if (body.endsWith('\n'))
        body.chop(1);

    // Errors get a heading line of their own and the text below it exactly as
    // the manager sent it, so the whole traceback can be read line by line.
    QString entry;
    if (kind == luaMessage_Error)
        entry = QString("Error in %1:\n%2").arg(scriptName(scriptID), body);
    else
        entry = QString("%1: %2").arg(scriptName(scriptID), body);

    // Only follow new output if the caret is at the very end. Otherwise leave
    // the caret, the selection and the scroll position exactly where they are.
    QTextCursor userCursor = output->textCursor();
    bool follow = !userCursor.hasSelection() && userCursor.atEnd();
    QScrollBar* vbar = output->verticalScrollBar();
    int scrollPos = vbar->value();

    QTextCursor ins(output->document());
    ins.movePosition(QTextCursor::End);
    if (!output->document()->isEmpty())
        ins.insertBlock();
    ins.insertText(entry);

    if (follow)
        output->moveCursor(QTextCursor::End);
    else
        vbar->setValue(scrollPos);
}

void LuaConsoleDialog::announce(const QString& text)
{
    // Speaks a short notice through the screen reader without moving focus.
    // Only done while this console is the active window.
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (!isActiveWindow())
        return;

    QAccessibleAnnouncementEvent event(this, text);
    QAccessible::updateAccessibility(&event);
#else
    Q_UNUSED(text);
#endif
}


void LuaConsoleDialog::onConsoleMessage(int scriptID, int kind, QString text)
{
    appendMessage(scriptID, kind, text);
}

void LuaConsoleDialog::onScriptStateChanged(int scriptID, int state)
{
    int oldState = scriptStates.value(scriptID, -1);
    scriptStates[scriptID] = state;

    QListWidgetItem* item = findItem(scriptID);
    if (item)
        refreshItem(item);

    if (state == luaScript_Errored && oldState != luaScript_Errored)
        announce(scriptName(scriptID) + " stopped by an error");

    updateButtons();
}

void LuaConsoleDialog::onScriptListChanged()
{
    syncScriptList();
    updateButtons();
}

void LuaConsoleDialog::onSelectionChanged()
{
    updateButtons();
}

void LuaConsoleDialog::onLoadScript()
{
    if (lastScriptDir.isEmpty())
        lastScriptDir = emuDirectory;

    QStringList files = QFileDialog::getOpenFileNames(this,
                                                      "Load Lua script",
                                                      lastScriptDir,
                                                      "Lua scripts (*.lua);;All files (*)");
    if (files.isEmpty())
        return;

    lastScriptDir = QFileInfo(files.last()).absolutePath();

    for (const QString& path : files)
    {
        int id = luaManager->loadScript(path);
        if (id < 0)
            continue;

        // Remember the path now so messages that arrive before the list
        // update are still labelled with the right file name.
        scriptPaths[id] = path;
        scriptStates[id] = luaScript_Loading;
        pendingSelectID = id;
    }

    syncScriptList();
    updateButtons();
}

void LuaConsoleDialog::onStopScript()
{
    int id = selectedScriptID();
    if (id < 0)
        return;

    luaManager->stopScript(id);
}

void LuaConsoleDialog::onRestartScript()
{
    int id = selectedScriptID();
    if (id < 0)
        return;

    luaManager->restartScript(id);
}

void LuaConsoleDialog::onRemoveScript()
{
    int id = selectedScriptID();
    if (id < 0)
        return;

    // The item goes away when the manager reports the list change.
    luaManager->removeScript(id);
}

void LuaConsoleDialog::onStopAll()
{
    luaManager->stopAll();
}

void LuaConsoleDialog::onAutoloadClicked(bool checked)
{
    int id = selectedScriptID();
    if (id < 0)
        return;

    QString path = scriptPaths.value(id);
    if (path.isEmpty())
        return;

    QStringList paths = luaManager->autoloadScripts();
    for (int i = paths.size() - 1; i >= 0; i--)
    {
        if (samePath(paths[i], path))
            paths.removeAt(i);
    }
    if (checked)
        paths.append(path);

    luaManager->setAutoloadScripts(paths);
    autoloadPaths = paths;

    for (int i = 0; i < scriptList->count(); i++)
        refreshItem(scriptList->item(i));

    updateButtons();
}

void LuaConsoleDialog::onClearOutput()
{
    output->clear();
    luaManager->clearBacklog();
    announce("Output cleared");
}

void LuaConsoleDialog::onCopyOutput()
{
    QGuiApplication::clipboard()->setText(output->toPlainText());
    announce("Output copied to the clipboard");
}
