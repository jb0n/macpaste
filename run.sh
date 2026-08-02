#!/usr/bin/env bash
# Add -t for click-through: the first click on a background window also clicks
# through to the content (links, buttons) instead of only activating it.
./macpaste -v -c \
    -s "VirtualBox VM" \
    -s "Screen Sharing" \
    -n "Screen Sharing" \
    -n "Google Chrome" \
    -n "Slack" \
    -s uTorrent \
    -s iTerm2 \
    -s Terminal \
    -s Finder \
    -s "Royal TSX"
