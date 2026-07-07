#!/bin/sh
set -u

CLOCK="${CLOCK_BIN:-../../bin/clock}"
FAILED=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

timeout 5 "$CLOCK" -w WEATHER text > "$TMPDIR/out" 2>/dev/null
cat "$TMPDIR/out"

block=0
while IFS= read -r line; do
  if echo "$line" | grep -qE '^[0-9]{2}:[0-9]{2}:[0-9]{2}$'; then
    block=$((block + 1)); content=""
  elif [ -z "$content" ] && [ -n "$line" ]; then
    content="$line"
    if [ "$content" != "-" ]; then
      echo "$content" | grep -qiE '(partly|sunny|cloud|rain|clear|snow|fog|thunder|patchy|mist|overcast|drizzle|light|freezing|heavy)' \
        || { echo "  FAIL: no weather desc at block $block: $content"; FAILED=1; }
      echo "$content" | grep -qE '[+-]?[0-9]+°' \
        || { echo "  FAIL: no temp at block $block: $content"; FAILED=1; }
      echo "$content" | grep -qE '\[[+-]?[0-9]+°[CF]\.\.[+-]?[0-9]+°[CF]\]' \
        || { echo "  FAIL: no min/max at block $block: $content"; FAILED=1; }
    fi
  elif [ -z "$content" ] && [ -z "$line" ]; then
    echo "  FAIL: empty content at block $block (dash gap bug)"; FAILED=1
  fi
done < "$TMPDIR/out"

if [ $FAILED -ne 0 ]; then echo "  FAIL: weather-dash"; exit 1; fi
echo "  PASS: weather-dash"
