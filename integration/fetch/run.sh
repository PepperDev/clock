#!/bin/sh
set -u

CLOCK="${CLOCK_BIN:-../../bin/clock}"
FAILED=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

fail()
{
  echo "  FAIL: $*"
  FAILED=1
}

validate_fetch_output()
{
  out="$1"
  label="$2"
  lines=$(echo "$out" | wc -l)

  echo "$out" | sed -n '1p' | grep -qE '^[0-9]{2}:[0-9]{2}:[0-9]{2}$' \
    || { fail "$label: line 1 is not a timestamp"; return; }

  echo "$out" | sed -n '2p' | grep -qE '^[a-zA-Z0-9._:-]+ [↓↑][0-9.]+[KMGb]?↑[0-9.]+[KMGb]?( [▂▄▆█]+ -[0-9]+dBm)?$' \
    || { fail "$label: line 2 not a valid NIC line"; return; }

  cur=3
  nxt=$(echo "$out" | sed -n "${cur}p")

  if echo "$nxt" | grep -qE '^SSID '; then
    echo "$nxt" | grep -qE '^SSID [^ ]+ [0-9.]+/[0-9.]+[KMG]?$' \
      || { fail "$label: line $cur SSID line malformed"; return; }
    cur=$((cur + 1))
  fi

  [ $cur -le "$lines" ] || { fail "$label: missing IP line"; return; }
  echo "$out" | sed -n "${cur}p" | grep -qE '^IP [0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$' \
    || { fail "$label: line $cur not a valid IP line"; return; }
  cur=$((cur + 1))

  [ $cur -le "$lines" ] || { fail "$label: missing WAN line"; return; }
  nxt=$(echo "$out" | sed -n "${cur}p")
  if echo "$nxt" | grep -qE '^IP6 '; then
    cur=$((cur + 1))
  fi

  [ $cur -le "$lines" ] || { fail "$label: missing WAN line"; return; }
  nxt=$(echo "$out" | sed -n "${cur}p")
  if echo "$nxt" | grep -qE '^WAN6 '; then
    cur=$((cur + 1))
  fi

  [ $cur -le "$lines" ] || { fail "$label: missing WAN line"; return; }
  echo "$out" | sed -n "${cur}p" | grep -qE '^WAN [0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$' \
    || { fail "$label: line $cur not a valid WAN line"; return; }
  cur=$((cur + 1))

  [ $cur -le "$lines" ] || { fail "$label: missing weather line"; return; }
  wthr=$(echo "$out" | sed -n "${cur}p")
  echo "$wthr" | grep -qiE '(partly|sunny|cloud|rain|clear|snow|fog|thunder|patchy|mist|overcast|drizzle|light|freezing|heavy)' \
    || { fail "$label: weather description not found"; return; }
  echo "$wthr" | grep -qE '[+-]?[0-9]+°[CF]' \
    || { fail "$label: weather current temperature not found"; return; }
  echo "$wthr" | grep -qE '\[[+-]?[0-9]+°[CF]\.\.[+-]?[0-9]+°[CF]\]' \
    || { fail "$label: weather min/max temperatures not found"; return; }
}

echo "  FETCH once mode"
ONCE_OUT=$("$CLOCK" -o -w NET,WEATHER text 2>/dev/null)
rc=$?
[ $rc -eq 0 ] && [ -n "$ONCE_OUT" ] || fail "once mode: exit $rc or empty"
validate_fetch_output "$ONCE_OUT" "once"

  echo "  FETCH continuous mode"
	LAST=$(python3 -c "import pty; pty.spawn(['timeout', '15', '$CLOCK', '-w', 'NET,WEATHER', 'text'])" | tr -d '\r' | awk -v RS= '{last=$0} END{print last}')
echo "$LAST" | grep -q '^[0-9]\{2\}:[0-9]\{2\}:[0-9]\{2\}$' || { fail "continuous mode: missing weather line"; }
validate_fetch_output "$LAST" "continuous"

exit $FAILED
