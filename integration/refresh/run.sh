#!/bin/sh
set -u

CLOCK="${CLOCK_BIN:-../../bin/clock}"
FAILED=0

check_refresh()
{
  label="$1"
  args="$2"
  total_sec="$3"

  echo "Testing $label (${total_sec}s run)..."

  TF=$(mktemp)
  STRF=$(mktemp)
  strace -e trace=connect -f -o "$STRF" \
    timeout "$total_sec" $CLOCK $args >"$TF" 2>&1

  v4=0; v6=0; lines=0
  [ -f "$STRF" ] && {
    v4=$(grep -c "sa_family=AF_INET," "$STRF" 2>/dev/null || true)
    v6=$(grep -c "sa_family=AF_INET6" "$STRF" 2>/dev/null || true)
  }
  [ -f "$TF" ] && lines=$(wc -l < "$TF" 2>/dev/null || true)
  v4=${v4:-0}; v6=${v6:-0}; lines=${lines:-0}
  rm -f "$TF" "$STRF"

  fail=
  if [ "$v4" -ne 2 ]; then
    echo "  FAIL: $label - IPv4 had $v4 connects (expected = 2), IPv6 $v6 connects"
    fail=1
  fi
  if [ "$v6" -ne 0 ] && [ "$v6" -ne 2 ]; then
    echo "  FAIL: $label - IPv6 had $v6 connects (expected = 2 when present), IPv4 $v4 connects"
    fail=1
  fi
  if [ -z "$fail" ]; then
    echo "  PASS: $label (IPv4 $v4, IPv6 $v6, $lines output lines)"
  else
    FAILED=1
  fi
}

check_refresh "--ip-refresh=5 -w NET" "--ip-refresh=5 -w NET text" 9
check_refresh "--weather-refresh=5 -w WEATHER" "--weather-refresh=5 -w WEATHER text" 9

exit $FAILED
