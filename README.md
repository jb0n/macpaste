# MacPaste - The Missing Mouse Paste Feature for Mac OSX

## TL;DR

macpaste brings X11-style middle-click copy/paste to macOS: highlight any text, then middle-click anywhere to paste it. The core utility was written in 2016 by **Remik Ziemlinski** (public domain); the original repo has since disappeared, and the code is preserved at [**lodestone/macpaste**](https://github.com/lodestone/macpaste), which this repo is a fork of. On top of that baseline, this fork adds:

- **Click-through (`-t`)** — the first click on a background window now clicks links and buttons instead of only activating the window. No more click-once-to-focus, click-again-to-act.
- **Per-app controls (`-s`, `-n`, `-x`)** — skip paste handling, skip raising and clicking the target window, or opt out of click-through for specific apps, matched case-insensitively by app name.
- **Works with VMs and remote desktops** — the copy/paste modifier is sent as a real key press rather than only a flag on the key event, so clients that track modifier state the way hardware reports it see Cmd+C instead of a literal `c`. Guest windows can also be skipped outright, which is usually what you want.
- **Hardened** — fixed crashes and spurious copies, dropped deprecated APIs, and switched to Accessibility-based window detection so only **Accessibility** permission is needed (no screen recording).
- **Convenience** — Ctrl instead of Cmd (`-c`), verbose logging (`-v`), and `./setup.sh` installs it as a LaunchAgent that starts at login.

## Overview
This simulates the middle mouse button copy/paste found in Unix/Linux X11 window managers.

If you first highlight arbitrary text or visual elements, you can then middle click in the same or another window to paste the elements. Unlike X11, this program will alter your clipboard. Perhaps a future version could manage its own buffer like X11.

#### How?
This program assumes that the key combinations Cmd+C/Cmd+V are mapped as copy and paste in your applications. If they are not, then this will not work, because the program simply posts the following events: 

1. Cmd+C down & up (copies your selected text or objects) whenever your left mouse button releases after a drag of more than 5 pixels or after a double-click.
   This allows copying text that is drag highlighted, or double-clicked to highlight words or lines.
2. An Accessibility raise of the window under the pointer, then Left Mouse Button down & up (position mouse cursor for paste insertion) on middle click.
3. Cmd+V down & up, 15 ms after that click, giving the app time to move its caret first.

The raise in step 2 uses the Accessibility API (`kAXRaiseAction` on the window, then frontmost on its app) rather than relying on the click to bring the window forward. A click aimed at a background window can be consumed by the activation itself, in which case the window comes forward but the caret never moves and the paste lands wherever the caret already was. Raising also names the *exact* window under the pointer; marking the app frontmost alone brings up whichever window that app considers main, which may not be the one you aimed at.

The modifier is sent as a real key press and release around the keystroke, not merely as a flag set on the key event. Native Mac apps accept either, but VM and remote-desktop clients rebuild modifier state from the modifier events real hardware emits; without them the guest receives a bare `c` or `v` and types the letter instead of copying or pasting. See [Virtual machines and remote desktops](#virtual-machines-and-remote-desktops) — those windows usually want to be skipped entirely anyway.

Synthetic mouse events are posted at the session event tap, which is the only tap location that actually delivers them to applications, and are tagged so macpaste ignores its own clicks when they arrive back through its tap.

Only the middle mouse button (button 2) pastes; side/back buttons (button 3+) do nothing. Micro-drags of less than 5 pixels do not trigger a copy.

If your mouse is left-handed, or you remapped the keystrokes, then just edit the C program and recompile.

#### Optional: Click-through (-t)

Normally macOS swallows the first click on a background window: the window activates, but the click never reaches the content (links, buttons), so you must click a second time. With `-t`, macpaste watches for left clicks on windows of non-frontmost apps, raises the window, and replays the click about 100 ms later, so the first click also acts on the content.

Notes:

- Both halves of the click are held back and replayed together. Releasing one without the other would leave the app running the tracking loop it starts on mouse-down, so it would read every later mouse move as a drag and select text as the pointer travelled.
- If the click turns into a drag (more than 5 pixels), the held-back press is released immediately at the point you pressed and the rest of the drag runs normally, so drags are not swallowed while macpaste waits for activation.
- Because the press is deferred, press-and-hold on a *background* window does nothing until you release it or start dragging. Ordinary clicking is unaffected.
- Clicks on the Dock and menu bar are never re-posted.
- Apps with their own click-through (Terminal, iTerm2) still receive exactly one completed click.
- Double-clicking text in a background window may select a word on the first click instead of just placing the cursor.
- `-x "App Name"` opts an app out of click-through entirely, e.g. `./macpaste -t -x "Google Chrome"`.
- The window is brought forward with the Accessibility API, not with a synthetic click, and it is the specific window under the pointer that is raised — not just whichever window its app considers main.
- macpaste waits for the app to actually become frontmost (polling up to ~0.5s), then re-checks that the same window still owns whatever is under the cursor. If a dialog appeared, or the window moved or closed, or a sibling window of the same app came forward over the point while waiting, the click is dropped rather than sent to whatever happens to be there now (`-v` logs this). If the app simply never reports itself frontmost, the click is replayed anyway rather than lost — that is just the ordinary activate-on-first-click behaviour.
- Whether an app is frontmost is read from that app directly. An app that doesn't report its frontmost state gets no click-through, and its clicks are passed through untouched rather than held back.

## Virtual machines and remote desktops

Give VM and remote-desktop windows **both `-s` and `-n`**:

    ./macpaste -t -s "VMware Fusion" -n "VMware Fusion"

The guest already does its own copy-on-select and middle-click paste, so macpaste only has to stay out of the way. `-s` alone is not enough, for two separate reasons:

- `-s` stops the copy and paste keystrokes, but the raise and focus click happen *before* the skip check, so a skipped app still receives a stray left click. Inside a guest that click collapses the very selection you were about to paste. `-n` suppresses it.
- The synthesized Cmd arrives in a Linux guest as **Super**, so a middle click fires Super+V at the desktop environment rather than pasting. `-s` stops that.

The names carried in the shipped `run.sh` are `VMware Fusion`, `VirtualBox VM`, `UTM`, `Screen Sharing` and `Royal TSX`. For any other app — or if one of those stops matching — run with `-v` and click the window: macpaste logs the name it resolved, and that is the string to pass.

## Permissions
macpaste needs **Accessibility** permission only (System Settings > Privacy & Security > Accessibility). This is the same permission the event tap already requires. **Screen Recording permission is not needed** - window detection uses the Accessibility API (AXUIElement), not screen capture.

If Accessibility permission is denied, macpaste prints a clear message and exits non-zero instead of crashing.

## Usage
Run the executable in the background from your shell command-line interface, or run it as a "Login Item" at startup (System Preferences > Users & Groups > Login Items > + > Navigate to file).

To install as a LaunchAgent that starts at login and restarts automatically, run `./setup.sh`. Pass any extra arguments you want the agent to use, e.g. `./setup.sh -t -s "Screen Sharing" -n "Screen Sharing"`. It builds macpaste, copies it to `~/bin/macpaste`, writes the plist, and prints the `launchctl bootstrap` line to start it. Agent output, including `-v` logging, goes to `~/Library/Logs/macpaste.log`.

Re-running `setup.sh` stops an already-loaded agent first, since it would otherwise keep running the previous binary and arguments. Note that it also replaces `~/bin/macpaste`: Accessibility permission follows the binary, so an entry approved for an earlier build may have to be removed and re-added before the agent will start.

## Options
-s "App Name" to skip the handling of that application

-c Uses Ctrl instead of Cmd

-t Enables click-through (see above): the first click on a background window also clicks through to the content instead of only activating the window. Off by default.

-x "App Name" Disables click-through for that application. Only meaningful with -t.

-n "App Name" Don't raise the window or simulate a left click on it before pasting. Focusing is the default behavior, but the click causes browsers to do weird things trying to open tabs by middle clicking.

-v Verbose mode. Logs some extra info.

-h Prints usage and exits. An unknown option or a missing option argument also prints usage and exits non-zero rather than starting up with a partly applied config.

App names for -s/-n/-x are matched **case-insensitively** against each application's display name (resolved from its bundle Info.plist, e.g. "VirtualBox VM" matches the VM window). If an app exposes no Accessibility elements, it simply isn't matched and pasting proceeds normally.

## Example setup (run.sh)

The author's daily config, showing `-t`/`-s`/`-n` in practice:

```bash
#!/usr/bin/env bash
# -v logs; -t click-through: the first click on a background window also
# clicks through to the content (links, buttons)
#
# The first five take both -s and -n: they are VM or remote-desktop windows
# whose guest handles copy/paste itself. See "Virtual machines and remote
# desktops" above for why -s alone leaves a stray click behind.
#
# Google Chrome, Slack   -n only: a synthetic left click can open a tab, or
#                        drop focus out of the composer.
# iTerm2, Terminal       -s: they already paste on middle click, so macpaste
#                        would paste a second time.
# Finder, uTorrent       -s: no text target to paste into.
./macpaste -v -t \
    -s "VMware Fusion" \
    -n "VMware Fusion" \
    -s "VirtualBox VM" \
    -n "VirtualBox VM" \
    -s "UTM" \
    -n "UTM" \
    -s "Screen Sharing" \
    -n "Screen Sharing" \
    -s "Royal TSX" \
    -n "Royal TSX" \
    -n "Google Chrome" \
    -n "Slack" \
    -s uTorrent \
    -s iTerm2 \
    -s Terminal \
    -s Finder
```

Note that `-c` is absent: Cmd is what native Mac apps expect, and the VM windows that would want Ctrl are skipped outright. `-c` is global, so there is no way to use Ctrl for a guest and Cmd for Slack in a single instance.

Run it from your shell, or pass the same flags to `./setup.sh` so the LaunchAgent uses them, e.g. `./setup.sh -t -s "Screen Sharing" -n "Google Chrome"`.

Note: `-s`, `-n` and `-x` are independent — an app can appear in multiple lists (like Screen Sharing above), and names are matched case-insensitively, so `-s iTerm2` would also match `iterm2`.

## Building

	make macpaste

## Running

    ./macpaste &

## License
Public Domain 2016
