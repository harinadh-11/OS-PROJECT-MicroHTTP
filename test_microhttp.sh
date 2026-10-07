#!/bin/bash

BASE="http://127.0.0.1:8080"
PASS=0
FAIL=0

check() {
    local name="$1"
    local expected="$2"
    local actual="$3"

    if echo "$actual" | grep -q "$expected"; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== MicroHTTP Correctness Test Suite ==="
echo

# 1. Normal GET
response=$(curl.exe -s -i "$BASE/" 2>/dev/null)
check "GET / returns 200" "HTTP/1.1 200 OK" "$response"

# 2. Missing file
response=$(curl.exe -s -i "$BASE/file-that-does-not-exist.html" 2>/dev/null)
check "Missing file returns 404" "HTTP/1.1 404 Not Found" "$response"

# 3. Directory traversal
response=$(curl.exe --path-as-is -s -i "$BASE/%2e%2e/etc/passwd" 2>/dev/null)
check "Directory traversal is blocked" "404 Not Found" "$response"

# 4. Concurrent requests
success=$(seq 1 20 | xargs -P 20 -I {} curl -s -o /dev/null -w "%{http_code}\n" "$BASE/" 2>/dev/null | grep -c "^200$")

if [ "$success" -eq 20 ]; then
    echo "PASS: 20 concurrent requests returned 200"
    PASS=$((PASS + 1))
else
    echo "FAIL: Concurrent requests ($success/20 returned 200)"
    FAIL=$((FAIL + 1))
fi

# 5. Malformed HTTP request
malformed=$(python3 - <<'PY'
import socket

try:
    s = socket.create_connection(("127.0.0.1", 8080), timeout=5)
    s.sendall(b"THIS IS NOT HTTP\r\n\r\n")
    data = s.recv(1024)
    s.close()
    print(data.decode("latin1", errors="replace"))
except Exception as e:
    print("ERROR:", e)
PY
)

check "Malformed request returns 400" "HTTP/1.1 400 Bad Request" "$malformed"

echo
echo "=== Test Summary ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ "$FAIL" -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "SOME TESTS FAILED"
    exit 1
fi
