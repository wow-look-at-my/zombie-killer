#!/bin/sh
# Test the kernel module: build it, spawn zombies, load the module (which
# sweeps on init), and verify no zombies remain. Needs root for insmod.
set -eu
DIR=$(dirname "$0")

echo "== building module =="
make -C "$DIR"

echo "== building zombie spawner =="
cc -O2 -Wall -o /tmp/zk-makezombies "$DIR/../test/makezombies.c"

echo "== spawning zombies =="
/tmp/zk-makezombies 20 &
SPAWN=$!
sleep 1

BEFORE=$(ps -eo stat | awk '$1 ~ /^Z/' | wc -l)
echo "zombies before: $BEFORE"

echo "== loading module (sweeps on insmod) =="
sudo insmod "$DIR/zombie_killer.ko" || { echo "insmod failed"; exit 1; }
sleep 1

AFTER=$(ps -eo stat | awk '$1 ~ /^Z/' | wc -l)
echo "zombies after: $AFTER"
ps -eo pid,ppid,stat,comm | awk '$3 ~ /^Z/ {print}' || true

echo "== unloading module =="
sudo rmmod zombie_killer || true

kill "$SPAWN" 2>/dev/null || true

if [ "$AFTER" -eq 0 ]; then
	echo "PASS: no zombies remain on the process list"
	exit 0
else
	echo "FAIL: $AFTER zombies remain"
	exit 1
fi