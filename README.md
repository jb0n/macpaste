# MacPaste - The Missing Mouse Paste Feature for Mac OSX

## Overview
This simulates the middle mouse button copy/paste found in Unix/Linux X11 window managers.

If you first highlight arbitrary text or visual elements, you can then middle click in the same or another window to paste the elements. Unlike X11, this program will alter your clipboard. Perhaps a future version could manage its own buffer like X11.

#### How?
This program assumes that the key combinations Cmd+C/Cmd+V are mapped as copy and paste in your applications. If they are not, then this will not work, because the program simply posts the following events: 

1. Cmd+C down & up (copies your selected text or objects) whenever your left mouse button releases after a drag of more than 5 pixels or after a double-click.
   This allows copying text that is drag highlighted, or double-clicked to highlight words or lines.
2. Left Mouse Button down & up (position mouse cursor for paste insertion) on middle click.
3. Cmd+V down & up after tiny delay following middle click.

Only the middle mouse button (button 2) pastes; side/back buttons (button 3+) do nothing. Micro-drags of less than 5 pixels do not trigger a copy.

If your mouse is left-handed, or you remapped the keystrokes, then just edit the C program and recompile.

## Permissions
macpaste needs **Accessibility** permission only (System Settings > Privacy & Security > Accessibility). This is the same permission the event tap already requires. **Screen Recording permission is not needed** - window detection uses the Accessibility API (AXUIElement), not screen capture.

If Accessibility permission is denied, macpaste prints a clear message and exits non-zero instead of crashing.

## Usage
Run the executable in the background from your shell command-line interface, or run it as a "Login Item" at startup (System Preferences > Users & Groups > Login Items > + > Navigate to file).

To install as a LaunchAgent that starts at login and restarts automatically, run `./setup.sh`. Pass any extra arguments you want the agent to use, e.g. `./setup.sh -c -s "Screen Sharing"`.

## Options
-s "App Name" to skip the handling of that application

-c Uses Ctrl instead of Cmd

-n "App Name" Don't focus window before pasting by simulating left click. This was the default behavior, but causes browsers to do weird things trying to open tabs by middle clicking.

-v Verbose mode. Logs some extra info.

App names for -s/-n are matched **case-insensitively** against each application's display name (resolved from its bundle Info.plist, e.g. "VirtualBox VM" matches the VM window). If an app exposes no Accessibility elements, it simply isn't matched and pasting proceeds normally.

## Example setup (run.sh)

The author's daily config, showing `-s`/`-n` in practice. The comment next to each entry explains why that app is listed:

```bash
#!/usr/bin/env bash
# -v logs; -c uses Ctrl (not Cmd) as the copy/paste modifier
./macpaste -v -c \
    -s "VirtualBox VM"   # guest owns its clipboard; a local Cmd+V would fight the VM's
    -s "Screen Sharing"  # remote Mac handles its own paste; posting Cmd+V would double-fire
    -n "Screen Sharing"  # don't synth-click inside the remote session either
    -n "Google Chrome"   # synthetic left click can open tabs / mess with links on middle click
    -n "Slack"           # synth click can drop focus out of the composer or switch channels
    -s uTorrent          # nothing to paste into the torrent list
    -s iTerm2            # already pastes on middle click itself; would double-paste
    -s Terminal          # same reason as iTerm2
    -s Finder            # no text target; paste would beep or trigger rename oddness
    -s "Royal TSX"       # RDP/SSH client: remote side handles paste, like Screen Sharing
```

Run it from your shell, or pass the same flags to `./setup.sh` so the LaunchAgent uses them, e.g. `./setup.sh -c -s "Screen Sharing" -n "Google Chrome"`.

Note: `-s` and `-n` are independent — an app can appear in both lists (like Screen Sharing above), and names are matched case-insensitively, so `-s iTerm2` would also match `iterm2`.

## Building

	make macpaste

## Running

    ./macpaste &

## License
Public Domain 2016
