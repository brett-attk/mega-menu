# MegaMenu (ncurses command menu)

`MegaMenu` is a terminal UI application built with `ncurses`. It reads menu entries from a config file and runs the selected shell command, showing command output in a lower pane.

## Features

- Config-driven command menu (JSON: `theme_index`, `groups[]`, `items[].label`, `items[].command`, `items[].group`, `items[].launch_detached`)
- Live command output panel
- Add, edit, and delete menu items from inside the UI
- Group manager popup (`g`) for adding/renaming/removing groups
- Grouped menu rendering in the top pane
- Theme cycling for color-capable terminals
- In-app sudo password prompt for commands that start with `sudo`
- Hidden `i` shortcut for a quick system info popup

## Linux dependencies

Install a C toolchain (`gcc`, `make`) and ncurses development headers.

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential libncurses-dev
```

Fedora/RHEL:

```bash
sudo dnf install -y gcc make ncurses-devel
```

Arch Linux:

```bash
sudo pacman -S --needed base-devel ncurses
```

Verify tools are available:

```bash
gcc --version
make --version
```

## Build

From the project root:

```bash
make
```

This produces the `megamenu` binary.

## Project layout

- `main.c`: ncurses UI, command execution flow, keyboard handling, and popups.
- `config.c`: JSON config parsing/writing and menu/group data management.
- `config.h`: shared menu/group data structures and config function declarations.
- `ui.c`: theme lifecycle and system info popup rendering.
- `ui.h`: shared UI constants and UI module function declarations.

## Run

Use the default config (`megamenu.config`):

```bash
make run
```

Or run directly with an explicit config path:

```bash
./megamenu /path/to/megamenu.config
```

## Quick start (Linux)

```bash
git clone <your-repo-url>
cd shellUI
make
./megamenu megamenu.config
```

## Config file format

Default config file: `megamenu.config`

Config format:

```json
{
  "theme_index": 0,
  "groups": ["Ungrouped"],
  "items": [
    {
      "label": "Menu label",
      "command": "command to run",
      "group": "Ungrouped",
      "launch_detached": false
    }
  ]
}
```

`launch_detached` behavior:
- `false` (default): run in-menu and stream output into the output pane
- `true`: launch detached from the menu process and immediately return control to the menu
  - If a graphical session is available, the app first tries to open the command in a new terminal window.
  - If no terminal window can be opened, it falls back to a background detached launch.

Example:

```json
{
  "theme_index": 2,
  "groups": ["Utilities", "Apps", "Ungrouped"],
  "items": [
    {
      "label": "Show date",
      "command": "date",
      "group": "Utilities",
      "launch_detached": false
    },
    {
      "label": "Open browser",
      "command": "firefox",
      "group": "Apps",
      "launch_detached": true
    }
  ]
}
```

Notes:

- JSON must include a top-level `items` array.
- `groups` is optional; if omitted, missing groups are auto-created and `Ungrouped` is ensured.
- `theme_index` is optional; defaults to `0` and saves the last selected color theme.
- Each item must include non-empty `label` and `command` strings.
- `items[].group` is optional; missing/unknown groups default to `Ungrouped`.
- `launch_detached` supports JSON booleans (`true` / `false`).

## Keyboard controls

- `Up` / `Down`: Move selection
- `Enter`: Run selected command
- `a`: Add a menu item
- `Space` (during Add/Edit popup checkbox): Toggle “Launch detached”
- `Left` / `Right` (during Add/Edit popup group field): Change selected group
- `e`: Edit selected item
- `d`: Delete selected item (with confirmation)
- `g`: Open group manager popup (add/rename/delete groups)
- `r` (inside group manager): Rename selected group
- `c`: Cycle UI theme
- `q`: Quit

## Hidden shortcut

- `i`: Open system info popup (`Enter` closes it). This key is intentionally not shown in the on-screen menu hint.

## Troubleshooting

- **`fatal error: ncurses.h: No such file or directory`**
  - Install ncurses development headers for your distro (`libncurses-dev`, `ncurses-devel`, or `ncurses`).

- **Linker errors for ncurses symbols (for example `undefined reference to initscr`)**
  - Rebuild with the provided `Makefile` (`make`) so the app links with `-lncurses`.

- **Terminal too small popup errors**
  - Resize terminal window larger, then retry. Popups for add/edit/group/system-info need more rows/columns than the base menu.

- **Sudo commands do not run**
  - Ensure the command starts with `sudo` and enter your password in the app prompt.
  - If no password is entered (or prompt canceled), the command is aborted by design.

- **Detached launch does not open a new terminal window**
  - The app falls back to background detached mode when no GUI/terminal emulator is available.
  - Install a terminal emulator (`x-terminal-emulator`, `gnome-terminal`, `konsole`, `xterm`, etc.) for new-window behavior.

- **Config parse/load errors**
  - Validate `megamenu.config` is valid JSON with a top-level `items` array.
  - Check each item includes non-empty `label` and `command`.

## Clean build artifacts

```bash
make clean
```
