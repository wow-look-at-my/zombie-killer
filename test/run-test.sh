#!/bin/sh
# Test harness: spawn zombies, show them, kill them with gozer, show none.
set -u

Z=20
echo "== spawning $Z zombies =="
./makezombies "$Z" &
SPAWNER=$!
sleep 1

echo "== zombies before =="
ps -eo pid,ppid,stat,comm | awk '$3 ~ /^Z/ {print}'
BEFORE=$(ps -eo stat | awk '$1 ~ /^Z/' | wc -l)
echo "zombie count before: $BEFORE"

echo "== running gozer =="
./gozer
RC=$?
echo "gozer exit code: $RC"

echo "== zombies after =="
AFTER=$(ps -eo stat | awk '$1 ~ /^Z/' | wc -l)
ps -eo pid,ppid,stat,comm | awk '$3 ~ /^Z/ {print}'
echo "zombie count after: $AFTER"

wait $SPAWNER 2>/dev/null

if [ "$AFTER" -eq 0 ]; then
	echo "PASS: no zombies remain"
	exit 0
else
	echo "FAIL: $AFTER zombies remain"
	exit 1
fi
