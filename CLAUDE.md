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
- Never block a DBus call or ACP request in a nested event loop (other sessions' requests would nest inside it): askUserQuestion uses a delayed DBus reply, terminal/wait_for_exit a callback
- Each chat keeps its own provider; `ACP/activeProvider` is only the default for new chats
- Chats connect as soon as they are created (`ChatWidget::startSession()`); there is no Connect button. A failed or lost connection shows a Reconnect button in the chat (resumes the last connected session if any), and switching provider starts a new session
- Session titles come from the agent's `session_info_update.title`; renaming (header label double-click or rename button) sends `session/set_title` when the agent advertises `sessionCapabilities.title`, otherwise the title is kept locally. The title is shown in the chat header, the tab (`windowTitle`, elided) and the side panel
- Provider logos (`AgentIcons`, owned by `SettingsStore`): explicit icon, else the ACP registry icon for the provider's `registryId` (fetched from cdn.agentclientprotocol.com, cached in `~/.cache/kate-code/acp-registry`, rendered with `currentColor` replaced by the palette text colour), else `code-context`. Kate reads a widget tab's icon only when adding the tab, so `ChatWidget::updateKateTabIcon()` finds the tab through a temporary unique title and sets the icon itself
- Resuming lives in the side panel (`SessionPanel`, filled by `KateCodeView::refreshResumableSessions()`): transcripts of the current project (or of all directories) with a user prompt, minus open sessions. The list is refreshed on view changes, on the project plugin's `projectFileNameChanged`/`projectMapChanged` (connected by name; it has no public header), when the panel is shown and by its refresh button; each refresh re-syncs `SessionStore`'s QSettings so other Kate instances' changes show up. Title, provider, first prompt and "replaced by" (context-injection restores start a new agent session) are kept per session in `SessionStore`; transcripts are only parsed for older, untracked sessions, since injected context embeds earlier transcripts. Resuming opens a new chat (`createChat(false)` + `ChatWidget::resumeSavedSession()`). Deleting (`KateCodeView::deleteSavedSession()`, panel context menu or Delete key) removes the transcript, summary and stored entry of the session and of the sessions it replaced; with the user's consent it also runs the agent headlessly (`ACPSessionDeleter`: `initialize`, then `session/delete` if `sessionCapabilities.delete` is advertised)
- Kate keeps the enabled-plugin list in the session file (`kate/anonymous.katesession`, `[Kate Plugins]`), not in `katerc`
- K_PLUGIN_CLASS_WITH_JSON macro requires katecode.json in same directory
- Plugin views are children of MainWindow and cleaned up automatically

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
