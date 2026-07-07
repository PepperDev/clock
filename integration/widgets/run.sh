#!/bin/sh
set -u

CLOCK="${CLOCK_BIN:-../../bin/clock}"
FAILED=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

check_widget()
{
  w="$1"
  timeout 5 "$CLOCK" -o -w "$w" text > "$TMPDIR/${w}.out" 2>/dev/null
  rc=$?
  [ $rc -eq 0 ] && [ -s "$TMPDIR/${w}.out" ]
}

fail()
{
  echo "  FAIL: $*"
  FAILED=1
}

check_net()
{
  o="$1"
  lines=$(echo "$o" | wc -l)
  cur=1

  echo "$o" | sed -n "${cur}p" | grep -qE '^[0-9]{2}:[0-9]{2}:[0-9]{2}$' \
    || { fail "NET: line 1 is not a timestamp"; return 1; }
  cur=$((cur+1))

  [ $cur -le "$lines" ] || { fail "NET: missing NIC line"; return 1; }
  echo "$o" | sed -n "${cur}p" | grep -qE '^[a-zA-Z0-9._:-]+ [↓↑][0-9.]+[KMGb]?↑[0-9.]+[KMGb]?( ▂▄▆█ -[0-9]+dBm)?$' \
    || { fail "NET: line 2 not a valid NIC line"; return 1; }
  cur=$((cur+1))

  [ $cur -le "$lines" ] || { fail "NET: missing IP line"; return 1; }
  nxt=$(echo "$o" | sed -n "${cur}p")
  if echo "$nxt" | grep -qE '^SSID '; then
    echo "$nxt" | grep -qE '^SSID [^ ]+ [0-9.]+/[0-9.]+[KMG]?$' \
      || { fail "NET: SSID line malformed"; return 1; }
    cur=$((cur+1))
    [ $cur -le "$lines" ] || { fail "NET: missing IP line"; return 1; }
    nxt=$(echo "$o" | sed -n "${cur}p")
  fi

  echo "$nxt" | grep -qE '^IP [0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$' \
    || { fail "NET: IP IPv4 line not found"; return 1; }
  cur=$((cur+1))

  [ $cur -le "$lines" ] || { fail "NET: missing WAN line"; return 1; }
  nxt=$(echo "$o" | sed -n "${cur}p")
  if echo "$nxt" | grep -q '^IP6 '; then
    echo "$nxt" | grep -qE '^IP6 [0-9a-fA-F:]+$' \
      || { fail "NET: IP6 invalid format"; return 1; }
    cur=$((cur+1))
    [ $cur -le "$lines" ] || { fail "NET: missing WAN line"; return 1; }
    nxt=$(echo "$o" | sed -n "${cur}p")
  fi

  echo "$nxt" | grep -qE '^WAN [0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$' \
    || { fail "NET: WAN IPv4 not found"; return 1; }
  cur=$((cur+1))

  if [ $cur -le "$lines" ]; then
    nxt=$(echo "$o" | sed -n "${cur}p")
    if echo "$nxt" | grep -q '^WAN6 '; then
      echo "$nxt" | grep -qE '^WAN6 [0-9a-fA-F:]+$' \
        || { fail "NET: WAN6 invalid format"; return 1; }
      cur=$((cur+1))
    fi
  fi
  return 0
}

# ---- NET ----
if check_widget NET; then
  o=$(cat "$TMPDIR/NET.out")
  check_net "$o" && echo "  PASS: NET"
else
  fail "NET: timed out or empty output"
fi

check_weather()
{
  o="$1"
  echo "$o" | sed -n '1p' | grep -qE '^[0-9]{2}:[0-9]{2}:[0-9]{2}$' \
    || { fail "WEATHER: line 1 is not a timestamp"; return 1; }
  wthr=$(echo "$o" | sed -n '2p')
  echo "$wthr" | grep -qiE '(partly|sunny|cloud|rain|clear|snow|fog|thunder|patchy|mist|overcast|drizzle|light|freezing|heavy)' \
    || { fail "WEATHER: description not found"; return 1; }
  echo "$wthr" | grep -qE '[+-]?[0-9]+°[CF]' \
    || { fail "WEATHER: current temperature not found"; return 1; }
  echo "$wthr" | grep -qE '\[[+-]?[0-9]+°[CF]\.\.[+-]?[0-9]+°[CF]\]' \
    || { fail "WEATHER: min/max temperatures not found"; return 1; }
  return 0
}

# ---- WEATHER ----
if check_widget WEATHER; then
  o=$(cat "$TMPDIR/WEATHER.out")
  check_weather "$o" && echo "  PASS: WEATHER"
else
  fail "WEATHER: timed out or empty output"
fi

# ---- CPU ----
if check_widget CPU; then
  o=$(cat "$TMPDIR/CPU.out")
  echo "$o" | grep -qE '[0-9]+%' || fail "CPU: no percentage found"
  echo "  PASS: CPU"
else
  fail "CPU: timed out or empty output"
fi

# ---- MEM ----
if check_widget MEM; then
  o=$(cat "$TMPDIR/MEM.out")
  echo "$o" | grep -qE '[0-9]+%' || fail "MEM: no percentage found"
  echo "  PASS: MEM"
else
  fail "MEM: timed out or empty output"
fi

# ---- STO ----
if check_widget STO; then
  o=$(cat "$TMPDIR/STO.out")
  # at least one device and mount
  echo "$o" | grep -qE '^\s*/' || fail "STO: no device line (starting with /)"
  devs=$(echo "$o" | grep -cE '^\s*/')
  echo "$o" | grep -qE '[0-9]+%' || fail "STO: no usage percentage"
  [ "$devs" -ge 1 ] || fail "STO: less than one device"
  echo "  PASS: STO"
else
  fail "STO: timed out or empty output"
fi

# ---- BAT ----
if check_widget BAT; then
  o=$(cat "$TMPDIR/BAT.out")
  echo "$o" | grep -qE '[0-9]+%' || fail "BAT: no percentage found"
  echo "  PASS: BAT"
else
  echo "  SKIP: BAT (no battery)"
fi

# ---- UP ----
if check_widget UP; then
  o=$(cat "$TMPDIR/UP.out")
  echo "$o" | grep -qE '(up|day|hour|minute|[0-9])' || fail "UP: no uptime info"
  echo "  PASS: UP"
else
  fail "UP: timed out or empty output"
fi

# ---- FAN ----
if check_widget FAN; then
  o=$(cat "$TMPDIR/FAN.out")
  # check for FAN line and optional temp line
  echo "$o" | grep -qE 'FAN' || fail "FAN: no FAN label found"
  echo "  PASS: FAN"
else
  echo "  SKIP: FAN (no fan controller)"
fi

# ---- GPU ----
if check_widget GPU; then
  o=$(cat "$TMPDIR/GPU.out")
  echo "$o" | grep -qE '[0-9]+%' || fail "GPU: no percentage found"
  echo "  PASS: GPU"
else
  echo "  SKIP: GPU (no GPU)"
fi

# ---- date/time ----
if check_widget ""; then
  o=$(cat "$TMPDIR/.out")
  t=$(echo "$o" | grep -oE '[0-9]{2}:[0-9]{2}:[0-9]{2}' | head -1)
  if [ -n "$t" ]; then
    now=$(date +%s)
    h=$(echo "$t" | cut -d: -f1)
    m=$(echo "$t" | cut -d: -f2)
    s=$(echo "$t" | cut -d: -f3)
    tsec=$(( (${h#0} * 3600) + (${m#0} * 60) + (${s#0}) ))
    nsec=$(( ( $(date +%H | sed 's/^0//') * 3600) + ( $(date +%M | sed 's/^0//') * 60) + $(date +%S | sed 's/^0//') ))
    diff=$((tsec - nsec)); [ "$diff" -lt 0 ] && diff=$((-diff))
    [ "$diff" -le 1 ] || fail "date/time: off by $diff seconds (accept ≤1)"
  fi
  echo "  PASS: date/time"
else
  fail "date/time: timed out or empty output"
fi

exit $FAILED
