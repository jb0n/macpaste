#!/usr/bin/env bash
# -t click-through: the first click on a background window also clicks through
# to the content (links, buttons) instead of only activating it
#
# VM and remote-desktop windows take both -s and -n. The guest already does its
# own copy-on-select and middle-click paste, so macpaste has to stay out of the
# way completely. -s alone isn't enough: paste() raises the window and posts its
# focus click before the skip check, and that stray left click lands in the guest
# and collapses the
# selection the guest is about to paste. -s also matters more than it looks --
# the synthesized cmd reaches the guest as Super, so without it a middle click
# fires Super+V at the desktop environment.
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
