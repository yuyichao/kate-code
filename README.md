# Kate Code

A plugin for the Kate text editor that integrates ACP-compatible coding agents directly into the editor's interface. It provides an interactive chat panel where you can work with Claude Code, Vibe, or any other configured ACP provider without leaving your KDE development environment.

![Kate Code Screenshot](Screenshot.png)

## Features

### Chat Interface
- **Real-time Streaming**: Messages stream in as the selected agent responds, with markdown formatting via marked.js
- **Syntax Highlighting**: Language-aware code highlighting via highlight.js supporting 50+ languages
- **Theme Integration**: Automatically adapts to your Kate/KDE color scheme (dark/light)
- **Copy Buttons**: One-click clipboard copy on all code blocks

### Tool Visualization
See what the active agent is doing with inline tool call displays:
- **Bash**: Command preview in header, full command and output in expandable details
- **Edit**: Unified diffs with syntax-highlighted code showing exactly what changed
- **Write**: Syntax-highlighted file content with copy button
- **Task/TaskOutput**: Sub-agent status with badges for agent type, background execution, and resume state
- **Read/Glob/Grep**: File operation summaries

### Task Management
- Collapsible task list showing multi-step operation progress
- Real-time status: pending (○), in-progress (⟳), completed (✓)
- Progress counter: "Tasks (3/5)" format
- State persists across sessions via localStorage

### Permission System
- Inline permission dialogs for tool approvals
- Options: Always Allow, Allow Once, Reject
- Clear display of tool inputs and available actions

### KDE Integration
- Automatic color scheme extraction from `~/.config/kdeglobals`
- Kate theme colors applied to highlight.js syntax highlighting
- Seamless side panel integration
- Context-aware: passes the current file, selection, and project root to the active agent
- Image paste support: paste images from clipboard directly into chat messages

### Architecture
- **ACP Protocol**: JSON-RPC 2.0 over stdin/stdout with the selected ACP provider process
- **Qt WebChannel**: Bidirectional C++/JavaScript bridge for real-time UI updates
- **Web UI**: HTML/CSS/JS rendered in Qt WebEngineView for rich formatting

## Prerequisites

### System Requirements
- KDE Plasma / Kate text editor
- Qt 6 (Core, DBus, Widgets, WebEngineWidgets, WebChannel, Network, Svg)
- KDE Frameworks 6 (KF6):
  - KTextEditor
  - KI18n
  - KCoreAddons
  - KXmlGui
  - SyntaxHighlighting
  - Pty
- CMake 3.16 or higher
- C++17 compatible compiler
- Optional, for remote folders (`kate-remote-fs`): libfuse 3.12 or newer (`fuse3`), and `sshfs` at runtime

### Runtime Dependencies
- At least one ACP-compatible agent executable installed and available in `PATH`
- Kate Code seeds editable provider entries for Claude Code (`claude-agent-acp`) and Vibe (`vibe-acp`), and supports custom ACP providers

## Installation

### Install an ACP provider

Install the ACP-compatible agent you want to use and ensure its executable is available in `PATH`. For example, follow the [claude-agent-acp installation instructions](https://github.com/agentclientprotocol/claude-agent-acp) for Claude Code (the installed binary is named `claude-agent-acp`).

Verify the executable, for example:
```bash
which claude-agent-acp
```

### Option 1: Install from Package (Recommended)

#### Arch Linux
The `kate-code-git` package is maintained in the archlinuxcn package
repositories; its PKGBUILD is tracked there rather than in this repository.

### Option 2: Build from Source

#### Install Build Dependencies

**Arch Linux / Manjaro:**
```bash
sudo pacman -S cmake extra-cmake-modules qt6-webengine qt6-webchannel qt6-declarative qt6-svg \
  ktexteditor ki18n kcoreaddons kxmlgui syntax-highlighting kpty kparts
```

**Debian / Ubuntu (26.04 or newer):**
```bash
sudo apt install g++ cmake extra-cmake-modules \
  qt6-base-dev qt6-webengine-dev qt6-webchannel-dev qt6-declarative-dev qt6-svg-dev \
  libkf6texteditor-dev libkf6i18n-dev libkf6coreaddons-dev libkf6xmlgui-dev \
  libkf6syntaxhighlighting-dev libkf6pty-dev libkf6parts-dev
```

**Fedora:**
```bash
sudo dnf install cmake extra-cmake-modules gcc-c++ qt6-qtwebengine-devel qt6-qtsvg-devel \
  kf6-ktexteditor-devel kf6-ki18n-devel kf6-kcoreaddons-devel \
  kf6-kxmlgui-devel kf6-syntax-highlighting-devel
```

**openSUSE Tumbleweed:**
```bash
sudo zypper install cmake kf6-extra-cmake-modules gcc-c++ \
  qt6-base-devel qt6-webchannel-devel qt6-webenginewidgets-devel qt6-svg-devel \
  kf6-ktexteditor-devel kf6-ki18n-devel kf6-kcoreaddons-devel \
  kf6-kxmlgui-devel kf6-syntax-highlighting-devel kf6-kpty-devel
```

#### Build and Install

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Install to system (requires sudo) - Arch/Fedora/most distros need --prefix /usr
sudo cmake --install build --prefix /usr

# Or install to user directory
cmake --install build --prefix ~/.local
```

The plugin will be installed to:
- Library: `/usr/lib/qt6/plugins/kf6/ktexteditor/katecode.so`
- Metadata: `/usr/lib/qt6/plugins/kf6/ktexteditor/katecode.json`
- UI Resource: `/usr/share/kate/plugins/katecode/katecodeui.rc`

### Enable in Kate

1. Restart Kate completely (close all windows)
2. Open Kate
3. Go to **Settings** → **Configure Kate** → **Plugins**
4. Find and enable **Kate Code** (and **Remote Projects** for remote folders, see below)
5. Click **OK**

## Usage

### Starting a Session

1. Click **New Session** in the Kate Code side panel (or use a quick action) to open a chat tab
2. The chat connects to the default ACP provider right away, using the current project's directory as its working directory
3. Choosing another provider from the chat's provider dropdown starts a new session with it

### Resuming a Session

Below the open chats, the side panel lists the previous sessions of the current project (the project of the active document), most recent first, with their title (or first prompt) and logo of the agent they ran with. Double-click one to resume it in a new chat tab, with the same agent. The list follows the current project and is reloaded when the panel is shown; the refresh button reloads it on demand (e.g. after another Kate instance added or deleted sessions). The folder button lists the sessions of all directories instead; the history button opens the session browser, which shows each session's summary.

To delete a previous session, right-click it and choose **Delete Session…** (or select it and press **Delete**). This removes its transcript and summary from Kate Code, together with any earlier sessions it was restored from. If its agent supports deleting sessions (ACP `session/delete`, e.g. Claude Code), the confirmation also offers to delete it from the agent's own history, so it no longer shows up there (e.g. in `claude --resume`) either.

### Sending Messages

- Type your message in the input field at the bottom
- Press **Enter** to send (Shift+Enter for newline)
- The agent's response will stream in real time with markdown formatting
- Messages sent while the agent is working are sent right away when the agent queues prompts itself (Claude Code takes them into the running turn at its next step, without interrupting it); otherwise they are sent when the turn ends
- Type `/` to pick one of the agent's slash commands: your most used commands come first, and the list narrows (alphabetically) as you type
- Slash commands (e.g. `/advisor`, `/remote-control`; those the agent lists, so a message starting with a path like `/tmp` is not one) are sent exactly as typed, without the current file or added context (which stay for the next message), and wait for a running turn to end
- When the agent edits files through Kate, it never opens, closes or switches tabs: a file that is open is changed in its buffer (so the change can be undone there) and saved, other files are changed on disk. A file with unsaved changes in Kate that was also changed on disk is left alone and the agent is told to ask you

### Agent Mode

Use the mode dropdown next to the input field to select an operating mode exposed by the active ACP provider. Available modes and their behavior depend on that provider; common examples include default and plan modes.

### Code Blocks

When the agent shows code in responses:
- **Syntax highlighting** is applied automatically based on the language
- Click the **📋 copy button** in the top-right corner of any code block to copy to clipboard
- Highlighting theme automatically matches your KDE color scheme (light/dark)

### Context Awareness

The plugin automatically provides context to the active agent:
- **Current File**: If you have a file open, the agent knows which file you're working on
- **Selection**: If you have text selected, it is passed to the agent as context
- **Project Root**: Automatically detected from:
  - Kate's project plugin (if available)
  - VCS markers (`.git`, `.hg`, `.svn`)
  - Build files (`CMakeLists.txt`, `package.json`, `Cargo.toml`, etc.)
  - IDE markers (`.idea`, `.vscode`, `.kate-project`)

### Tool Calls

When the agent uses tools, they appear inline in the conversation:
- **Collapsed view**: Shows tool name and key info (command for Bash, filename for file ops)
- **Expanded view**: Click to see full command/input and complete output
- **Status indicators**: Visual feedback for pending, running, completed, or failed operations

### Task List

- Appears at the bottom of the chat when the agent is working on multi-step tasks
- Click the header to collapse/expand (state persists across sessions)
- Shows progress counter: "Tasks (3/5)" means 3 completed out of 5 total
- **Icons**:
  - ○ Pending task
  - ⟳ Currently working (animated)
  - ✓ Completed task

### Permissions

When the agent needs approval to run certain tools:
- A permission dialog appears inline in the chat
- Shows the tool name, input details, and available options
- Options typically include: "Always Allow", "Allow", "Reject"
- Click an option to respond

### Session Summarization & Resume (Experimental)

Kate Code includes optional session summarization and context re-seeding:

- **Configurable Summary Agent**: Use the current live agent or select any configured ACP provider as the summary agent
- **Background Summaries**: A provider other than “Current agent” runs briefly in the background against the saved transcript
- **Context Resume**: A new session can be seeded from a stored summary; abandoned raw sessions can be summarized before resuming
- **Local Storage**: Summaries are stored under `~/.kate-code/summaries/`

Enable summaries and choose the summary agent under **Settings → Configure Kate → Kate Code → Advanced**. Kate Code does not store a separate API key or use KWallet for summaries; each ACP provider uses its own authentication and configuration.

This feature is experimental and behavior may change in future releases.

## Configuration

### ACP Providers

Manage providers under **Settings → Configure Kate → Kate Code → General**. Providers can be added, edited, removed, and reordered. Each entry can configure its executable, command-line options, MCP configuration, ACP session settings, and resume capability. The seeded Claude Code and Vibe entries are ordinary editable providers.

Each provider's logo is shown on its chat tabs, in the provider dropdown and in the session list. It is the provider's explicit icon (a theme icon name or an image file) if set, otherwise the icon of its entry in the [ACP agent registry](https://agentclientprotocol.com/get-started/registry) (the "ACP registry id", e.g. `claude-acp`; the seeded providers have one), otherwise a generic icon. Registry icons are downloaded from `cdn.agentclientprotocol.com` and cached under `~/.cache/kate-code/acp-registry`; Kate Code only contacts the registry for providers with a registry id and no cached (or a week-old) icon.

### Color Scheme

The plugin automatically adapts to your KDE color scheme by reading `~/.config/kdeglobals`. If you change your KDE theme, restart Kate to pick up the new colors.

## Remote Folders (kate-remote-fs, experimental)

### Remote Projects plugin

Enable **Remote Projects** (and Kate's **Project** plugin) in Kate's plugin
settings. **File → Open Remote Folder…** shows KDE's folder dialog in its
Network view (`remote:/`): open a network folder (add one there for hosts
you use often) or type `sftp://host/` in the location bar, and browse to the
folder (the dialog browses through KIO; the chosen folder is then opened
through `kate-remote-fs`, as a project). **File → Open Recent Remote Folder** lists
the folders opened before. The folder is opened through
`kate-remote-fs` below, at its local path (e.g.
`~/.kate-remote/sftp/host/path`), so Kate's own features (search, git,
terminal, Kate Code) work on it. The whole remote file system (`host:/`)
cannot be opened as a project: Kate would list every file on it.

Kate's session restore brings remote projects and files back: the plugin
starts `kate-remote-fs` before Kate restores the session's files and reopens
remote projects that could not be restored before it ran. If a host cannot
be reached when Kate starts, its projects stay in the session and come back
at the next start where it is reachable (its files open empty meanwhile), and
the start may wait once for the ssh connect timeout. A host that could not be
connected to when opening a folder is not remembered. Browsing in the dialog
uses KIO's own sftp connection, so a host that asks for a password asks
again when the folder is opened. (Tested with the daemon started through
DBus activation; not yet with the installed systemd unit at login.)

### kate-remote-fs

`kate-remote-fs` is a small FUSE daemon (built when libfuse 3.12+ is found)
that makes remote folders available at stable local paths that mirror their
URL:

```
~/.kate-remote/sftp/myhost/home/me/project   <->   sftp://myhost/home/me/project
~/.kate-remote/sftp/me@host:2222/srv         <->   sftp://me@host:2222/srv
```

Only registered hosts exist under `~/.kate-remote/sftp/` (host names and
`user@host:port` as for `ssh`, including `~/.ssh/config` aliases). The first
access below a registered host connects to it with `sshfs`; idle hosts are
disconnected and reconnect on the next access, and a host that cannot be
reached fails fast for a while instead of every access waiting for a
timeout. Because the paths stay the same across reboots, Kate's recent files,
session restore and projects are meant to work with remote folders as with
local ones (not yet tried with Kate itself). Listing
`~/.kate-remote/sftp` shows only connected hosts, so tools walking your home
directory do not connect anywhere.

Hosts are registered over DBus (the upcoming Remote Projects plugin does this
when you open a remote folder); the daemon keeps the list in
`~/.local/state/kate-remote-fs/hosts` and is the only one writing it:
```bash
busctl --user call org.kde.KateRemoteFS /org/kde/KateRemoteFS org.kde.KateRemoteFS addHost ss sftp myhost
busctl --user call org.kde.KateRemoteFS /org/kde/KateRemoteFS org.kde.KateRemoteFS hosts
busctl --user call org.kde.KateRemoteFS /org/kde/KateRemoteFS org.kde.KateRemoteFS removeHost ss sftp myhost
```
Unregistered names do not exist on purpose: tools look for files such as
`HEAD` (git) or `Cargo.toml` in every parent directory, and those are valid
host names.

Start it with the user service (or let DBus start it on first use):
```bash
systemctl --user enable --now kate-remote-fs
```

Settings live in `~/.config/kate-remoterc` (restart the service after
changing them):
```ini
[General]
# Must be an empty directory owned by you
MountRoot=~/.kate-remote
# Seconds before an idle host is disconnected (0: never)
IdleTimeout=900
# Seconds before a failed host is tried again
RetryAfterFailure=30
# Seconds to connect (includes waiting for a password prompt)
MountTimeout=120
SshCommand=ssh
# Extra sshfs -o options, comma separated (e.g. ro)
SshfsOptions=
```

Changing `MountRoot` changes every remote path, so Kate's history of the old
paths no longer works. sshfs caches directory contents for 20 seconds, so a
change made on the remote by something else can take that long to show.
Commands that look at every file (`git status` on a large tree) are slow over
any network file system: about 15 s for 600 files with a cold cache, 1.5 s
warm. Passwords and unknown host keys are asked through
`SSH_ASKPASS` (`ksshaskpass` if installed); keys from your ssh agent work
without prompts. Connection errors are available over DBus
(`org.kde.KateRemoteFS`, `lastError`), and the daemon logs to the journal
(`journalctl --user -u kate-remote-fs`).

## Troubleshooting

### Plugin not showing in Kate
- Verify the plugin is installed: `ls /usr/lib/qt6/plugins/kf6/ktexteditor/katecode.so`
- Check Kate's plugin list: Settings → Configure Kate → Plugins
- Restart Kate completely (close all windows)

### Connection fails
- The chat shows the agent's error and a **Reconnect** button
- Verify the selected provider executable is installed and available in `PATH`
- Look for error messages in terminal when launching Kate from command line: `kate`

### Messages not displaying
- Open browser dev tools: The WebView uses Chromium, accessible on `localhost:9222`
- Check terminal output for JavaScript errors

### Build errors
- Ensure all KF6 dependencies are installed
- Check CMake output for missing packages
- Verify Qt 6 and KDE Frameworks 6 versions are compatible

## Development

### Project Structure

```
kate-code/
├── CMakeLists.txt              # Root build configuration
├── src/
│   ├── CMakeLists.txt          # Source build configuration
│   ├── katecode.json           # Plugin metadata
│   ├── katecode.qrc            # Qt resources (web assets)
│   ├── plugin/                 # Kate plugin integration
│   │   ├── KateCodePlugin.*    # Plugin factory
│   │   └── KateCodeView.*      # Per-window view, context providers
│   ├── acp/                    # ACP protocol implementation
│   │   ├── ACPModels.h         # Data structures
│   │   ├── ACPService.*        # QProcess subprocess management
│   │   └── ACPSession.*        # Protocol flow, message parsing
│   ├── ui/                     # User interface components
│   │   ├── ChatWidget.*        # Main container
│   │   ├── ChatWebView.*       # WebEngine view + JS bridge
│   │   ├── ChatInputWidget.*   # Multiline text input
│   │   └── PermissionDialog.*  # Permission approval UI
│   ├── util/                   # Utilities
│   │   └── KDEColorScheme.*    # KDE color extraction
│   └── web/                    # Web UI assets
│       ├── chat.html           # Chat page
│       ├── chat.css            # Styles
│       └── chat.js             # Message rendering, interactivity
```

### Building for Development

```bash
# Build without installing
cmake -B build
cmake --build build

# Install to user location (if supported)
cmake --install build --prefix ~/.local

# Rebuild after changes
cmake --build build
sudo cmake --install build
```

### Debug Logging

The plugin outputs debug information to stdout/stderr. Run Kate from terminal to see logs:
```bash
kate 2>&1 | grep -E "\[ACP|ChatWebView|ChatWidget\]"
```

## License

MIT License

## Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## Acknowledgments

- Built with Qt 6 and KDE Frameworks 6
- Supports ACP-compatible agents, with seeded providers for [claude-agent-acp](https://github.com/agentclientprotocol/claude-agent-acp) and Vibe
- Markdown rendering by [marked.js](https://marked.js.org/)
