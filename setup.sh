#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

LABEL="local.macpaste"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/macpaste.log"
TARGET="$HOME/bin/macpaste"

# Arguments are pasted into the plist as XML text, so an app name containing
# & or < produces a document launchd quietly refuses to load.
xml_escape() {
    local s=$1
    s=${s//&/&amp;}
    s=${s//</&lt;}
    s=${s//>/&gt;}
    printf '%s' "$s"
}

make macpaste

# Stop any agent that is already up. It is running the previous binary with the
# previous arguments, and leaving it there makes a re-run of this script look
# like it did nothing at all.
if launchctl print "gui/$(id -u)/$LABEL" >/dev/null 2>&1; then
    echo "Stopping the running agent so it picks up this build"
    launchctl bootout "gui/$(id -u)/$LABEL" || true
fi

mkdir -p "$HOME/bin" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
cp macpaste "$TARGET"

# Real tabs and newlines, rather than \t\n handed to printf '%b'. That %b also
# expanded any backslash escape inside an app name, so -s 'Foo\tBar' reached
# launchd with a literal tab in it and then matched nothing.
args=""
for a in "$@"; do
    args+=$'\n\t\t\t<string>'"$(xml_escape "$a")"'</string>'
done

# Write, validate, then move into place, so a rejected plist never replaces a
# working one.
tmp_plist="$(mktemp "${TMPDIR:-/tmp}/macpaste.plist.XXXXXX")"
trap 'rm -f "$tmp_plist"' EXIT

cat > "$tmp_plist" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
	<dict>
		<key>Label</key>
		<string>$(xml_escape "$LABEL")</string>
		<key>ProgramArguments</key>
		<array>
			<string>$(xml_escape "$TARGET")</string>$args
		</array>
		<key>RunAtLoad</key>
		<true/>
		<key>KeepAlive</key>
		<true/>
		<key>ThrottleInterval</key>
		<integer>30</integer>
		<key>StandardOutPath</key>
		<string>$(xml_escape "$LOG")</string>
		<key>StandardErrorPath</key>
		<string>$(xml_escape "$LOG")</string>
	</dict>
</plist>
PLIST_EOF

if ! plutil -lint "$tmp_plist" >/dev/null; then
    echo "Generated plist is not valid, leaving any existing one alone:" >&2
    plutil -lint "$tmp_plist" >&2 || true
    exit 1
fi
mv "$tmp_plist" "$PLIST"
trap - EXIT

echo ""
echo "Installed $PLIST"
echo "Output goes to $LOG"
echo ""
echo "    Grant Accessibility permission to $TARGET"
echo "    (System Settings -> Privacy & Security -> Accessibility)"
echo ""
echo "    This just replaced the binary at that path. Accessibility permission"
echo "    follows the binary, so an entry approved for an earlier build may need"
echo "    to be removed and re-added before macpaste will start."
echo ""
echo "    Start it with:"
echo "    launchctl bootstrap gui/\$(id -u) $PLIST"
echo ""
echo "    KeepAlive is on, so if it exits for lack of permission launchd retries"
echo "    every 30s. $LOG will say so."
