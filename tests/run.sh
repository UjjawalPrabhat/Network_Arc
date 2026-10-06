#!/bin/sh
# End-to-end tests for bserve + bcurl. Run from repo root: make test
set -u
cd "$(dirname "$0")/.."

PORT=${PORT:-19000}
FAKEPORT=$((PORT + 1))
TMP=$(mktemp -d)
LOG="$TMP/server.log"
pass=0 fail=0

./bserve ./www "$PORT" 2>"$LOG" &
SPID=$!
trap 'kill $SPID 2>/dev/null; wait $SPID 2>/dev/null; rm -rf "$TMP"' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do
    ./bcurl -I "localhost:$PORT/" >/dev/null 2>&1 && break
    sleep 0.2
done

check() {  # name, condition-result (0 = ok)
    if [ "$2" -eq 0 ]; then echo "  ok   $1"; pass=$((pass + 1));
    else echo "  FAIL $1"; fail=$((fail + 1)); fi
}

echo "bserve on :$PORT"

./bcurl "localhost:$PORT/index.html" >"$TMP/out" 2>/dev/null
rc=$?
cmp -s "$TMP/out" www/index.html; check "GET /index.html body matches, exit 0" $(( $? + rc ))

./bcurl "localhost:$PORT/" >"$TMP/out" 2>/dev/null
cmp -s "$TMP/out" www/index.html; check "GET / maps to index.html" $?

./bcurl "localhost:$PORT/docs/" >"$TMP/out" 2>/dev/null
cmp -s "$TMP/out" www/docs/index.html; check "GET /docs/ maps to docs/index.html" $?

./bcurl "localhost:$PORT/nope.html" >/dev/null 2>&1
[ $? -eq 1 ]; check "GET missing file -> exit 1 (404)" $?

./bcurl -v "localhost:$PORT/nope.html" 2>&1 >/dev/null | grep -q "status: 404"
check "missing file reports status 404" $?

./bcurl "localhost:$PORT/logo.png" >"$TMP/logo" 2>/dev/null
cmp -s "$TMP/logo" www/logo.png; check "binary file round-trips" $?

dd if=/dev/urandom of=www/.big.bin bs=1024 count=300 2>/dev/null
./bcurl "localhost:$PORT/.big.bin" >"$TMP/big" 2>/dev/null
cmp -s "$TMP/big" www/.big.bin; check "300 KiB file split across DATA frames" $?
nframes=$(./bcurl -v "localhost:$PORT/.big.bin" 2>&1 >/dev/null | grep -c "< DATA frame")
[ "$nframes" -eq 5 ]; check "  ... in 5 DATA frames of <=64 KiB ($nframes)" $?
rm -f www/.big.bin

./bcurl -I "localhost:$PORT/hello.txt" >"$TMP/out" 2>/dev/null
[ $? -eq 0 ] && [ ! -s "$TMP/out" ]; check "HEAD returns no body" $?

before=$(grep -c "accept" "$LOG")
./bcurl -v "localhost:$PORT/hello.txt" "localhost:$PORT/missing" "localhost:$PORT/index.html" \
    >"$TMP/out" 2>"$TMP/err"
rc=$?
sleep 0.2
after=$(grep -c "accept" "$LOG")
[ $((after - before)) -eq 1 ]; check "3 requests used exactly 1 connection" $?
[ "$(grep -c 'response complete' "$TMP/err")" -eq 3 ]; check "  ... and got 3 responses" $?
[ $rc -eq 1 ]; check "  ... worst exit code wins (404 -> 1)" $?

./bcurl "localhost:$PORT/../../../../etc/passwd" >"$TMP/out" 2>/dev/null
rc=$?
! grep -q "root:" "$TMP/out" && [ $rc -eq 1 ]; check "path traversal refused" $?

ln -sf /etc/hosts www/.escape 2>/dev/null
./bcurl -v "localhost:$PORT/.escape" 2>&1 >/dev/null | grep -q "status: 403"
check "symlink out of root -> 403" $?
rm -f www/.escape

expect() {  # scenario, expected output
    got=$(python3 tests/rawframes.py "$PORT" "$1" 2>&1)
    [ "$got" = "$2" ]; check "raw: $1 -> $2 (got: $got)" $?
}
expect unknown     "200"
expect malformed   "400 200"
expect badidx      "400 200"
expect stray-data  "400 200"
expect literal     "200"
expect pipeline    "200 404 200"
expect method      "405"
expect toobig      "400 closed"

python3 tests/rawframes.py "$FAKEPORT" fakeserver >"$TMP/fake" &
FPID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do grep -q ready "$TMP/fake" 2>/dev/null && break; sleep 0.1; done
out=$(./bcurl "localhost:$FAKEPORT/x" 2>/dev/null)
[ "$out" = "via fake server" ]; check "bcurl skips unknown frames from server" $?
wait $FPID 2>/dev/null

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
