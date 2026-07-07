#!/bin/sh
set -u

CLOCK="${CLOCK_BIN:-../../bin/clock}"
FAILED=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

timeout 9 "$CLOCK" -W5 -wWEATHER text > "$TMPDIR/out" 2>/dev/null

# collect each tick block (timestamp + content line + blank line separator)
block=0
while IFS= read -r line; do
  if echo "$line" | grep -qE '^[0-9]{2}:[0-9]{2}:[0-9]{2}$'; then
    block=$((block + 1)); content=""
  elif [ -z "$content" ] && [ -n "$line" ]; then
    content="$line"
    if [ "$block" -le 2 ]; then
      : # accept dash or anything
    elif [ "$block" -le 4 ]; then
      if [ "$content" = "-" ]; then
        echo "  FAIL: block $block should have weather (dash unexpected)"
        FAILED=1
      else
        echo "$content" | grep -qiE '(partly|sunny|cloud|rain|clear|snow|fog|thunder|patchy|mist|overcast|drizzle|light|freezing|heavy)' \
          || { echo "  FAIL: no weather desc at block $block"; FAILED=1; }
        echo "$content" | grep -qE '[+-]?[0-9]+°' \
          || { echo "  FAIL: no temp at block $block"; FAILED=1; }
        echo "$content" | grep -qE '\[[+-]?[0-9]+°[CF]\.\.[+-]?[0-9]+°[CF]\]' \
          || { echo "  FAIL: no min/max at block $block"; FAILED=1; }
      fi
    elif [ "$block" -eq 5 ]; then
      : # accept dash or weather (transition)
    elif [ "$block" -eq 6 ]; then
      if [ "$content" != "-" ]; then
        echo "  FAIL: block 6 expected dash (refresh blank), got: $content"
        FAILED=1
      fi
    elif [ "$block" -le 7 ]; then
      : # accept dash or anything (transition window)
    else
      # block 8+ should have valid weather again
      if [ "$content" = "-" ]; then
        echo "  FAIL: block $block should have weather (dash unexpected)"
        FAILED=1
      else
        echo "$content" | grep -qiE '(partly|sunny|cloud|rain|clear|snow|fog|thunder|patchy|mist|overcast|drizzle|light|freezing|heavy)' \
          || { echo "  FAIL: no weather desc at block $block"; FAILED=1; }
        echo "$content" | grep -qE '[+-]?[0-9]+°' \
          || { echo "  FAIL: no temp at block $block"; FAILED=1; }
        echo "$content" | grep -qE '\[[+-]?[0-9]+°[CF]\.\.[+-]?[0-9]+°[CF]\]' \
          || { echo "  FAIL: no min/max at block $block"; FAILED=1; }
      fi
    fi
  fi
done < "$TMPDIR/out"

if [ "$block" -lt 8 ]; then
  echo "  FAIL: only $block blocks captured, need at least 8 for 9-second run"
  FAILED=1
fi

if [ $FAILED -ne 0 ]; then
  echo "  FAIL: weather-refresh"
  exit 1
fi
echo "  PASS: weather-refresh"
