#!/usr/bin/env bash
# Simple smoke test: requires hub reachable at HUB_URL (default http://ams.local)
HUB_URL=${HUB_URL:-http://ams.local}
URL="$HUB_URL/aquarium/add-new-aquarium.html"

set -euo pipefail

echo "Fetching $URL..."
body=$(curl -fsS "$URL")

doctype_count=$(printf "%s" "$body" | grep -o "<!DOCTYPE html>" | wc -l)
if [ "$doctype_count" -ne 1 ]; then
  echo "ERROR: expected 1 <!DOCTYPE html> but found $doctype_count"
  exit 2
fi

if ! printf "%s" "$body" | grep -q '<aside class="sidebar"'; then
  echo "ERROR: sidebar markup not found"
  exit 3
fi

if printf "%s" "$body" | grep -q '<!DOCTYPE html>.*<!DOCTYPE html>'; then
  echo "ERROR: duplicate DOCTYPE detected"
  exit 4
fi

echo "OK: add-new-aquarium.html looks good (single DOCTYPE and sidebar present)"
exit 0
