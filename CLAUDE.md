# Kate-Code Plugin

A Kate text editor plugin that wraps Claude Code using claude-agent-acp.

## Build Instructions

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Installation

Test install:
```bash
cmake --install build --prefix ~/.local
```

Then restart Kate and enable the plugin via:
Settings > Configure Kate > Plugins > Enable "Kate Code"

## Architecture

### Layer Structure
- **Plugin**: KateCodePlugin, KateCodeView - Kate integration
- **ACP**: ACPService, ACPSession - JSON-RPC 2.0 over stdin/stdout to claude-agent-acp
- **MCP**: MCPServer - Built-in MCP server executable for Kate editor tools
- **UI**: ChatWidget, ChatWebView, ChatInputWidget, PermissionDialog, SessionPanel
- **Util**: KDEColorScheme - reads ~/.config/kdeglobals

### Key Files
- `src/plugin/KateCodePlugin.{h,cpp}` - Plugin registration via K_PLUGIN_CLASS_WITH_JSON
- `src/plugin/KateCodeView.{h,cpp}` - Per-window view: opens chats as editor-area tabs, side panel session list, provides Kate context
- `src/acp/ACPService.{h,cpp}` - QProcess-based claude-agent-acp subprocess management
- `src/acp/ACPSession.{h,cpp}` - Protocol flow and session state management
- `src/acp/ACPSessionDeleter.{h,cpp}` - Deletes saved sessions on the agent's side (one-shot agent process)
- `src/acp/ACPModels.h` - Data structures (Message, ToolCall, TodoItem, etc.)
- `src/remotefs/RemoteLocation.{h,cpp}` - Static lib `kateremotelocation`: remote URL <-> `<MountRoot>/<scheme>/<authority>/<path>` mapping, ssh authority validation, symlink target rewriting; shared by kate-remote-fs and the plugins
- `src/remotefs/daemon/` - `kate-remote-fs` FUSE daemon (libfuse3 high-level API) with DBus interface `org.kde.KateRemoteFS`; systemd user unit + DBus activation file
- `src/remotefs/askpass/main.cpp` - `kate-remote-fs-askpass`: SSH_ASKPASS for the mounts, login passwords from kpasswdserver (KIO's password cache)
- `src/remotefs/autotests/` - QtTest unit test and `test_remotefs.py` integration test (with `kpasswdtool` to seed kpasswdserver) (ctest; runs under `dbus-run-session`, sandboxed `sftp-server` via bwrap as the remote)
- `src/remoteprojects/` - "Remote Projects" Kate plugin (`kateremoteprojects`): File > Open Remote Folder… (KDE's folder dialog via `QFileDialog::getExistingDirectoryUrl`, starting at `remote:/` (network folders), schemes sftp/remote/file; KIO browses, the result is handled here: a `remote:` link is resolved with `KIO::stat` → `UDS_TARGET_URL`) / Open Recent Remote Folder, opens `<MountRoot>/<scheme>/<authority>/<path>` as a project via the project plugin view's `openDirectoryOrProject(QDir)` slot
- `src/mcp/MCPServer.{h,cpp}` - MCP protocol handler (initialize, tools/list, tools/call)
- `src/mcp/main.cpp` - MCP server entry point (stdio JSON-RPC loop)
- `src/ui/ChatWidget.{h,cpp}` - Main chat interface with ACP integration (one per editor tab)
- `src/ui/SessionPanel.{h,cpp}` - Side panel listing the window's open chats with status and usage
- `src/ui/ChatWebView.{h,cpp}` - QWebEngineView for HTML/CSS/JS rendering
- `src/ui/ChatInputWidget.{h,cpp}` - Multiline text input with send button
- `src/ui/PermissionDialog.{h,cpp}` - Modal dialog for tool approval requests
- `src/config/SettingsStore.{h,cpp}` - QSettings + KWallet storage, ACPProvider management
- `src/config/KateCodeConfigPage.{h,cpp}` - Kate config page with provider table, diff colors, summaries
- `src/util/KDEColorScheme.{h,cpp}` - KDE color extraction from kdeglobals
- `src/web/chat.{html,css,js}` - Web assets for chat interface
- `src/katecode.qrc` - Qt resource file for web assets
- `src/katecode.json` - KPlugin metadata

## Lessons Learned

### Build System
- Must include `<KLocalizedString>` for i18n() function
- KF6 components needed: I18n, TextEditor, CoreAddons, XmlGui
- Qt6 components: Core, Widgets, WebEngineWidgets

### Plugin Structure
- Chats are editor-area widgets added with `KTextEditor::MainWindow::addWidget()` (Kate owns and deletes them; they can move between splits). Kate calls an invokable `bool shouldClose()` before closing such a tab
- The side panel (`createToolView()` with `KTextEditor::MainWindow::Left`) holds the session list
- Several chats can run agents at once. Each chat owns an `EditorSession` DBus endpoint (`/KateCode/Session/sN`, interface `org.kde.katecode.Editor`); its kate-mcp-server gets the path in `KATECODE_DBUS_PATH` (service name in `KATECODE_DBUS_SERVICE`), so questions and edit notifications reach that chat only. The shared `/KateCode/Editor` object only serves document tools
- Agent actions must not open, close or activate windows/tabs or move focus, nor show modal dialogs (a dialog during a DBus call nests an event loop and times the agent out). Document writes (`EditorDBusService::changeDocument()`, `ACPSession::handleFsWriteTextFile()`) change an open document's buffer and save it only when its file is unchanged on disk (`EditorDBusService::documentChangedOnDisk()`: Kate's `modifiedOnDisk` flag, tracked per document, or the git-blob digest of the file vs `Document::checksum()`, since Kate updates the checksum when it notices a change); an unmodified stale buffer is written on disk and reloaded, a modified one is refused. Files that are not open are written on disk (QSaveFile), never opened
- Prompts: a slash command (`ACPSession::isSlashCommand()`: a `/name` first word naming one of the agent's `available_commands_update` commands, or any `/name` before the agent sent its list; decided once, when sent) is sent alone as one text block (agents only recognise a command that is the whole prompt; editor context and the first-prompt `<system-reminder>` wait for the next ordinary prompt). While a turn runs, other messages are sent as further `session/prompt` requests when the agent advertises `agentCapabilities._meta.claudeCode.promptQueueing` (Claude Code folds them into the running turn at its next step; the turn ends when all have answered, `m_midTurnPromptIds`); otherwise, and for commands, they are queued until the turn ends. (`_session/steering` is not used: it aborts the current generation)
- Never block a DBus call or ACP request in a nested event loop (other sessions' requests would nest inside it): askUserQuestion uses a delayed DBus reply, terminal/wait_for_exit a callback
- Each chat keeps its own provider; `ACP/activeProvider` is only the default for new chats
- Chats connect as soon as they are created (`ChatWidget::startSession()`); there is no Connect button. A failed or lost connection shows a Reconnect button in the chat (resumes the last connected session if any), and switching provider starts a new session
- Session titles come from the agent's `session_info_update.title`; renaming (header label double-click or rename button) sends `session/set_title` when the agent advertises `sessionCapabilities.title`, otherwise the title is kept locally. The title is shown in the chat header, the tab (`windowTitle`, elided) and the side panel
- Provider logos (`AgentIcons`, owned by `SettingsStore`): explicit icon, else the ACP registry icon for the provider's `registryId` (fetched from cdn.agentclientprotocol.com, cached in `~/.cache/kate-code/acp-registry`, rendered with `currentColor` replaced by the palette text colour), else `code-context`. Kate reads a widget tab's icon only when adding the tab, so `ChatWidget::updateKateTabIcon()` finds the tab through a temporary unique title and sets the icon itself
- Resuming lives in the side panel (`SessionPanel`, filled by `KateCodeView::refreshResumableSessions()`): transcripts of the current project (or of all directories) with a user prompt, minus open sessions. The list is refreshed on view changes, on the project plugin's `projectFileNameChanged`/`projectMapChanged` (connected by name; it has no public header), when the panel is shown and by its refresh button; each refresh re-syncs `SessionStore`'s QSettings so other Kate instances' changes show up. Title, provider, first prompt and "replaced by" (context-injection restores start a new agent session) are kept per session in `SessionStore`; transcripts are only parsed for older, untracked sessions, since injected context embeds earlier transcripts. Resuming opens a new chat (`createChat(false)` + `ChatWidget::resumeSavedSession()`). Deleting (`KateCodeView::deleteSavedSession()`, panel context menu or Delete key) removes the transcript, summary and stored entry of the session and of the sessions it replaced; with the user's consent it also runs the agent headlessly (`ACPSessionDeleter`: `initialize`, then `session/delete` if `sessionCapabilities.delete` is advertised)
- Kate keeps the enabled-plugin list in the session file (`kate/anonymous.katesession`, `[Kate Plugins]`), not in `katerc`
- K_PLUGIN_CLASS_WITH_JSON macro requires katecode.json in same directory
- Plugin views are children of MainWindow and cleaned up automatically

### kate-remote-fs
- Paths mirror URLs under a stable, configurable root (`kate-remoterc` `[General] MountRoot`, default `~/.kate-remote`), so Kate's history (recents, session restore, projects; Kate resolves symlinks, so these must be real paths, not symlinks to the per-host mounts) survives reboots. kio-fuse's mount directory is random per run
- Backends: `sftp` hosts are `sshfs`; the KIO schemes (`KateRemote::kioSchemes()`, `[General] KioSchemes`, read once per process) go through kio-fuse: DBus `org.kde.KIOFuse.VFS.mountUrl("<scheme>://<authority>/")` (blocking call from the FUSE thread, may wait for a KIO login dialog) returns its per-run path, kept in `Mount::backendPath`; "unmounting" only closes our root fd (kio-fuse has no unmount); a dead kio-fuse gives `ENOTCONN` → remount (DBus activation restarts it). KIO authorities may have `;` in the user (smb `DOMAIN;user`). Tests drive KIO's `fish` worker through a fake `ssh` on the PATH given to bus-activated services
- Each sftp host is an `sshfs host:/` mount in `$XDG_RUNTIME_DIR/kate-remote-fs/<scheme>/<authority>`, mounted on first access (per-host state machine, waiters share one attempt, failures cached for `RetryAfterFailure`) and unmounted when idle with no open files. Operations use `*at()` calls on a held root fd of the live mount (checked `FUSE_SUPER_MAGIC`), never the plain directory; `ENOTCONN` (sshfs died) remounts and retries once (mount generation counter, not `fstat`, which may answer from cache)
- Only registered hosts exist (DBus `addHost`/`removeHost`/`hosts`, signal `hostsChanged`; kept in `~/.local/state/kate-remote-fs/hosts`, read at startup and written only by the daemon, so there is a single writer; `kate-remoterc` is user settings the daemon never writes): tools stat `HEAD` (git), `Cargo.toml`, `setup.py`... in every parent directory, and these are valid host names. Names are also validated strictly (`parseSshAuthority()`: no leading `-`/`.`); listing `sftp/` shows only connected hosts; creating/removing a host directory itself is refused without connecting
- sshfs runs without `reconnect`: it then exits when the connection drops, the next access gets `ENOTCONN` and remounts here, with failures cached (with `reconnect` every access to a vanished host waits for the connect timeout). Directory cache stays at sshfs's default 20 s (a cold cache is a round trip per file)
- Single instance via a lock file (`$XDG_RUNTIME_DIR/kate-remote-fs/lock`); the DBus name is taken only after mounting, so DBus activation / systemd `Type=dbus` readiness means the paths work
- Lookups in one directory must not wait for a connecting host: request `FUSE_CAP_PARALLEL_DIROPS`, raise the worker thread cap (libfuse default 10)
- Symlinks: sshfs runs with `no_contain_symlinks` when supported (Arch patches sshfs to reject absolute/`..` links); `hostSymlinkTarget()` rewrites absolute targets and `..` above the remote root relative to the host directory, so links never resolve to local files
- sshfs drops ssh's stderr when daemonizing: the daemon passes `ssh_command=<SshCommand> -E <backend>/logs/<scheme>_<authority>.log` and reports that log; sshfs (without `-f`) exits only once mounted
- Helpers run without our fds (FUSE device, root fds: `CloseFileDescriptors`), with signals unblocked (the daemon blocks SIGTERM/INT/HUP for its signalfd) and in a new session (ssh asks through `SSH_ASKPASS`)
- Only ever unmount (`fusermount3 -u`) and `rmdir` backend dirs, never delete recursively: the mounts expose remote (and in tests local) files
- Qt logs to journald when stderr is not a terminal; tests set `QT_FORCE_STDERR_LOGGING=1`
- Passwords (`KdePasswords`, default on): sshfs's ssh gets `SSH_ASKPASS=kate-remote-fs-askpass` (`SSH_ASKPASS_REQUIRE=force`), with `KATE_REMOTE_AUTHORITY`, `KATE_REMOTE_FALLBACK_ASKPASS` (previous SSH_ASKPASS or ksshaskpass) and a per-attempt state file. Login password prompts (`isLoginPasswordPrompt()`) are answered from kpasswdserver: KIO's sftp worker files passwords under `sftp://[user@]host[:port]` (no port when 22; no user when the URL had none, the typed username only in the entry), so the helper checks both keys and only takes an entry for the user ssh logs in as (URL user, else from the prompt). A second prompt in the same attempt means the password was wrong: the entry is removed (`removeAuthInfo`) before `queryAuthInfo()` shows KDE's dialog — kpasswdserver answers a query whose client sequence number is older than the cached entry's (any new client) with that entry, no dialog ("auto retry"). Dialog answers are cached by kpasswdserver itself: bound to the asking window (KIO in Kate: until the window closes), 10 s for window 0 unless "Remember password" (then KWallet / session). Lookups (`findAuthInfoItem`) do not filter by window. Other prompts exec the fallback
- Tests with kpasswdserver: services DBus activates (kiod6, kwalletd6) get the bus daemon's environment (the user's real config), not the test's: run `dbus-update-activation-environment` with the test's XDG dirs first, disable KWallet (`kwalletrc` `[Wallet] Enabled=false`, else it waits for a wallet prompt), and stop the activated services afterwards (they outlive `dbus-run-session`)

### Remote Projects plugin
- Kate loads plugins (constructor, then `readSessionConfig`) before restoring the session's documents (`KateSessionManager::loadSession`), so the plugin's constructor makes sure kate-remote-fs runs (blocking DBus call, which activates it; the daemon takes its name only once mounted). It only blocks when the daemon is running or activatable
- The project plugin (load order −800) restores session projects before this plugin loads and drops those whose directory does not exist yet. The plugin saves the open remote projects (project view property `allProjects`) in its own session group and reopens them with the project plugin's invokable `projectForDir(QDir, bool)` (checking the path in a background thread)
- Session projects whose host is unreachable at startup stay pending (`m_pendingProjects`) and are written to the session again, so an offline start does not forget them; with no project view at save time the last read list is kept
- A folder dialog result without a user (not recents) gets the username KIO logged in with, from kpasswdserver (`checkAuthInfo` on `kioAuthUrl()`, in a worker thread since the client waits in an event loop), so a username typed into KIO's login dialog is used for the mount too
- Opening: the dialog's URL (sftp://, or a local path below the mount root) → `KateRemote::urlForInput()` (unit-tested; refuses the remote root, which Kate would index recursively as a non-VCS project; on refusal a warning box and the dialog again) → `hosts` → `addHost` → background `QFileInfo::isDir()` (connects) → `openDirectoryOrProject(QDir)`; on failure `lastError` is shown (Output view) and a host registered only for that attempt is removed again
- Recent remote folders are URLs in `katerc` `[Remote Projects] Recent` (Kate's own recents hold the local paths)
- Testing in a real Kate: KTextEditor does not save documents below `QDir::tempPath()` in sessions, so a harness in /tmp must set `TMPDIR` elsewhere; `Application::openUrl()` opens a document without a view (not saved in the session), use `MainWindow::openUrl()`. Offscreen Kate shows Qt's own file dialog; `QT_QPA_PLATFORMTHEME=kde` gives KDE's (`KDEPlatformFileDialog`, drive its `KFileWidget`: `setUrl()`, `slotOk()`), with a hidden `QFileDialog` behind it

### ACP Protocol
- JSON-RPC 2.0 over stdin/stdout with newline-delimited JSON
- Flow: `initialize` (protocolVersion: 1) → `session/new` (cwd, mcpServers) → `session/prompt`
- Streaming updates via `session/update` notifications with sessionUpdate types:
  - `agent_message_start`, `agent_message_chunk`, `agent_message_end`
  - `tool_call`, `tool_call_update`
  - `plan` (todos)
- Permission requests via `session/request_permission` with requestId that requires response

### ACP Provider Configuration
- Providers are defined by `ACPProvider` struct: id, description, executable, options, builtin flag
- Two built-in providers (Claude Code, Vibe/Mistral) are hardcoded and cannot be deleted
- Custom providers are stored in QSettings via `beginWriteArray("ACP/customProviders")`
- Active provider selection persisted as `ACP/activeProvider` string id
- Provider selector is a QComboBox in the ChatWidget header (disabled while connected)
- Unavailable executables shown grayed with "(not found)" suffix
- Settings page shows a QTableWidget for managing custom providers (add/edit/remove)
- Old `ACPBackend` enum settings are auto-migrated on first launch

### Web Interface
- QWebEngineView loads HTML from Qt resources (qrc:/katecode/web/chat.html)
- JavaScript functions exposed: `addMessage()`, `updateMessage()`, `finishMessage()`, `addToolCall()`, `updateToolCall()`
- C++ calls JavaScript via `page()->runJavaScript()` with proper string escaping
- CSS uses CSS variables for theming (ready for KDE color injection in Phase 4)
- Responsive layout with message bubbles, tool call display, and streaming indicator
