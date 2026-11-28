#!/usr/bin/env bash
# menyiapkan Media B
set -e

readonly IMG="$(dirname "$0")/lab_disk.img"
readonly MNT=/mnt/lab

# jika sudah termount
mountpoint -q "$MNT" && exit 0

# buat image kosong 64 MB
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none

# pasang image ke loop device kosong, terus simpan namanya
LOOP=$(sudo losetup --find --show "$IMG")

# buat filesystem ext4 di loop device
sudo mkfs.ext4 -q "$LOOP"

# mount ke /mnt/lab, lalu beri akses ke user biasa
sudo mkdir -p "$MNT"
sudo mount "$LOOP" "$MNT"
sudo chown "$USER" "$MNT"

echo "Media B: $MNT (dev: ${LOOP#/dev/})"   # ${LOOP#/dev/} membuang awalan /dev/