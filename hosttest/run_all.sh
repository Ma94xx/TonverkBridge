#!/bin/sh
# Baut und startet alle Host-Tests + prüft die Parser-Auswahl für echte Geräte-Dumps.
#   ./run_all.sh            Tests ausführen
#   ./run_all.sh --update   erwartete Fixture-Ausgaben neu schreiben (danach Diff prüfen!)
set -e
cd "$(dirname "$0")"
CF="-O1 -Wall -Werror -I. -I../app/src/main/cpp"
fail=0
for t in test_iso_fill test_capture test_stress test_stream_eps test_generic; do
    gcc $CF $t.c -o $t -lm -lpthread
    if ./$t > /tmp/hosttest_$t.log 2>&1; then echo "PASS $t"; else echo "FAIL $t"; cat /tmp/hosttest_$t.log; fail=1; fi
done
gcc $CF parse_dump.c -o parse_dump
HEX=$(mktemp -d)
for f in fixtures/lsusb/*.txt; do python3 -I lsusb2bin.py "$f" 2>/dev/null > "$HEX/$(basename "$f" .txt).hex"; done
if ./test_generic "$HEX"/*.hex fixtures/raw/*.txt > /tmp/hosttest_fixtures.log 2>&1; then echo "PASS test_generic (echte Geräte-Deskriptoren)"
else echo "FAIL test_generic (echte Geräte-Deskriptoren)"; cat /tmp/hosttest_fixtures.log; fail=1; fi
rm -rf "$HEX"
for f in fixtures/lsusb/*.txt fixtures/raw/*.txt; do
    b=$(basename "$f" .txt)
    case "$f" in
        */raw/*) got=$(./parse_dump < "$f") || true ;;
        *)       got=$(python3 -I lsusb2bin.py "$f" 2>/dev/null | ./parse_dump) || true ;;
    esac
    exp="fixtures/expected/$b.txt"
    if [ "$1" = "--update" ]; then mkdir -p fixtures/expected; printf '%s\n' "$got" > "$exp"; echo "UPD  $b"; continue; fi
    if [ -f "$exp" ] && [ "$got" = "$(cat "$exp")" ]; then echo "PASS fixture $b"
    else echo "FAIL fixture $b"; printf 'got:\n%s\n' "$got"; [ -f "$exp" ] && printf 'expected:\n%s\n' "$(cat "$exp")"; fail=1; fi
done
[ $fail = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
