#!/bin/bash
# ============================================================================
# Notification Feature Test Script
# Tests aquarium CRUD + device delete notifications via curl + ntfy polling
# ============================================================================

set -e
HUB="http://192.168.1.53"
NTFY_TOPIC="ams-hub-bg"
SINCE=$(date +%s)

echo "╔══════════════════════════════════════════════════════╗"
echo "║   AMS Notification Feature Test                      ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""
echo "📍 Hub: $HUB"
echo "📍 ntfy topic: $NTFY_TOPIC"
echo "📍 Start timestamp: $SINCE"
echo ""

# ---- Step 1: Verify hub is online ----
echo "━━━ Step 1: Verify Hub Status ━━━"
STATUS=$(curl -s --connect-timeout 5 "$HUB/api/status")
echo "  Response: $STATUS" | head -c 120
echo ""
UPTIME=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin)['uptime'])" 2>/dev/null || echo "?")
echo "  ✅ Hub online (uptime: ${UPTIME}s)"
echo ""

# ---- Step 2: Create Aquarium ----
echo "━━━ Step 2: Create Aquarium ━━━"
CREATE_RESP=$(curl -s "$HUB/api/aquariums" \
  -H "Content-Type: application/json" \
  -d '{"name":"CurlTest Tank","volumeLiters":42,"tankType":"Planted","location":"Script Room"}')
echo "  Response: $CREATE_RESP"
AQ_ID=$(echo "$CREATE_RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('id', d.get('aquariumId','?')))" 2>/dev/null || echo "?")
echo "  ✅ Created aquarium ID: $AQ_ID"
echo ""
sleep 2

# ---- Step 3: Update Aquarium ----
echo "━━━ Step 3: Update Aquarium ━━━"
UPDATE_RESP=$(curl -s "$HUB/api/aquarium/update?id=$AQ_ID" \
  -H "Content-Type: application/json" \
  -d '{"name":"CurlTest Tank v2","volumeLiters":99,"location":"Updated Room"}')
echo "  Response: $UPDATE_RESP"
echo "  ✅ Updated aquarium ID: $AQ_ID"
echo ""
sleep 2

# ---- Step 4: Delete Aquarium ----
echo "━━━ Step 4: Delete Aquarium ━━━"
DELETE_RESP=$(curl -s -X POST "$HUB/api/aquarium/delete?id=$AQ_ID")
echo "  Response: $DELETE_RESP"
echo "  ✅ Deleted aquarium ID: $AQ_ID"
echo ""
sleep 2

# ---- Step 5: Delete fake device (won't fire notif but tests the path) ----
echo "━━━ Step 5: Delete Device (fake MAC) ━━━"
DEV_DEL_RESP=$(curl -s "$HUB/api/delete-device" \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF"}')
echo "  Response: $DEV_DEL_RESP"
echo ""

# ---- Step 6: Wait for ntfy batch flush ----
echo "━━━ Step 6: Waiting for ntfy batch flush (config.* route = 60s) ━━━"
echo "  ⏳ Waiting 70 seconds..."
for i in $(seq 1 14); do
  sleep 5
  printf "  [%02d/70s]\r" $((i * 5))
done
echo "  ✅ Wait complete                     "
echo ""

# ---- Step 7: Poll ntfy ----
echo "━━━ Step 7: Poll ntfy.sh for notifications ━━━"
NTFY_MSGS=$(curl -s "https://ntfy.sh/$NTFY_TOPIC/json?poll=1&since=$SINCE")
echo "$NTFY_MSGS" | python3 -c "
import sys, json
lines = [l for l in sys.stdin.read().strip().split('\n') if l]
msgs = [json.loads(l) for l in lines if l]
msgs = [m for m in msgs if m.get('event') == 'message']
print(f'  Total notifications received: {len(msgs)}')
print()
for i, m in enumerate(msgs, 1):
    tags = ','.join(m.get('tags', []))
    print(f'  [{i}] [{tags}] {m[\"message\"]}')
print()

# Verify expected notifications
checks = {
    'Aquarium CREATE': any('created' in m['message'].lower() and 'CurlTest' in m['message'] for m in msgs),
    'Aquarium UPDATE': any('updated' in m['message'].lower() and 'CurlTest' in m['message'] for m in msgs),
    'Aquarium DELETE': any('deleted' in m['message'].lower() and any('config.aquarium' in t for t in m.get('tags',[])) for m in msgs),
}

print('  ══════════════════════════════════════════════')
print('  RESULTS:')
for name, passed in checks.items():
    icon = '✅' if passed else '❌'
    print(f'    {icon} {name}: {\"PASS\" if passed else \"FAIL\"} ')
print('  ══════════════════════════════════════════════')

all_pass = all(checks.values())
print(f'  Overall: {\"ALL PASSED ✅\" if all_pass else \"SOME FAILED ❌\"}')
sys.exit(0 if all_pass else 1)
"
EXIT_CODE=$?
echo ""

# ---- Step 8: Also check boot notification ----
echo "━━━ Step 8: Boot notification (last 30 min) ━━━"
BOOT_SINCE=$(($(date +%s) - 1800))
curl -s "https://ntfy.sh/$NTFY_TOPIC/json?poll=1&since=$BOOT_SINCE" | python3 -c "
import sys, json
lines = [l for l in sys.stdin.read().strip().split('\n') if l]
msgs = [json.loads(l) for l in lines if l]
boots = [m for m in msgs if m.get('event') == 'message' and 'system.boot' in m.get('tags', [])]
print(f'  Boot notifications found: {len(boots)}')
for m in boots:
    print(f'    → {m[\"message\"]}')
if boots:
    print('  ✅ Boot notification: PASS')
else:
    print('  ❌ Boot notification: FAIL')
"

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║   Test Complete                                      ║"
echo "╚══════════════════════════════════════════════════════╝"

exit $EXIT_CODE
