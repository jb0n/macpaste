#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

make macpaste
mkdir -p "$HOME/bin"
cp macpaste "$HOME/bin/"

mkdir -p "$HOME/Library/LaunchAgents"

args=""
for a in "$@"; do
    args="$args\n\t\t<string>$a</string>"
done

cat > "$HOME/Library/LaunchAgents/local.macpaste.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
	<dict>
		<key>Label</key>
		<string>local.macpaste</string>
		<key>ProgramArguments</key>
		<array>
			<string>$HOME/bin/macpaste</string>
			$(printf '%b' "$args")
		</array>
		<key>RunAtLoad</key>
		<true/>
		<key>KeepAlive</key>
		<true/>
	</dict>
</plist>
PLIST

echo ""
echo "Installed $HOME/Library/LaunchAgents/local.macpaste.plist"
echo ""
echo "    Grant Accessibility permission to $HOME/bin/macpaste"
echo "    (System Settings -> Privacy & Security -> Accessibility)"
echo ""
echo "    Load it now with:"
echo "    launchctl bootstrap gui/\$(id -u) $HOME/Library/LaunchAgents/local.macpaste.plist"
