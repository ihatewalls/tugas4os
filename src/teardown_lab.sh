#!/usr/bin/env bash
# melepas Media B
set -e

readonly IMG="$(dirname "$0")/lab_disk.img"
readonly MNT=/mnt/lab

# unmount kalau masih termount 
mountpoint -q "$MNT" && sudo umount "$MNT"

# lepas semua loop device milik image ini 
sudo losetup --associated "$IMG" --noheadings --output NAME 2>/dev/null \
    | xargs -r sudo losetup --detach

# hapus image
rm -f "$IMG"

echo "Media B dilepas."