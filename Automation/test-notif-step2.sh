#!/bin/bash
# Step 2: Poll ntfy.sh for notifications after the batch window has flushed
NTFY_TOPIC="ams-hub-bg"
SINCE=$(cat /tmp/notif_test_since.txt 2>/dev/null || echo "$(( $(date +%s) - 120 ))")

echo "╔══════════════════════════════════════════════════════╗"
echo "║   ntfy.sh Notification Verification                  ║"
echo "╚══════════════════════════════════════════════════════╝"
echo "📍 Polling since: $SINCE"
echo ""

# Poll recent messages
echo "━━━ Notifications since CRUD operations ━━━"
curl -s "https://ntfy.sh/$NTFY_TOPIC/json?poll=1&since=$SINCE" | python3 -c "
import sys, json

lines = [l for l in sys.stdin.read().strip().split('\n') if l]
msgs = [json.loads(l) for l in lines if l]
msgs = [m for m in msgs if m.get('event') == 'message']

print(f'  Total notifications: {len(msgs)}')
print()
for i, m in enumerate(msgs, 1):
    tags = ','.join(m.get('tags', []))
    print(f'  [{i}] [{tags}] {m[\"message\"]}')
print()

# Verify expected notifications
checks = {
    'Aquarium CREATE': any('created' in m['message'].lower() and 'CurlTest' in m['message'] for m in msgs),
    'Aquarium UPDATE': any('updated' in m['message'].lower() and 'CurlTest' in m['message'] for m in msgs),
    'Aquarium DELETE': any('deleted' in m['message'].lower() for m in msgs),
    'Device DELETE':   any('deleted' in m['message'].lower() and ('device' in m['message'].lower() or 'AA:BB' in m['message']) for m in msgs),
}

print('  ══════════════════════════════════════════════')
print('  RESULTS:')
for name, passed in checks.items():
    icon = '✅' if passed else '❌'
    print(f'    {icon} {name}: {\"PASS\" if passed else \"FAIL\"} ')
print('  ══════════════════════════════════════════════')

all_pass = all(checks.values())
failed = [k for k,v in checks.items() if not v]
passed_count = sum(1 for v in checks.values() if v)
print(f'  Score: {passed_count}/{len(checks)} passed')
if not all_pass:
    print(f'  Note: Device DELETE may not fire if MAC AA:BB:CC:DD:EE:FF')
    print(f'        was not in devices.json (expected behavior)')
"

echo ""
echo "━━━ Boot notifications (last 30 min) ━━━"
BOOT_SINCE=$(($(date +%s) - 1800))
curl -s "https://ntfy.sh/$NTFY_TOPIC/json?poll=1&since=$BOOT_SINCE" | python3 -c "
import sys, json
lines = [l for l in sys.stdin.read().strip().split('\n') if l]
msgs = [json.loads(l) for l in lines if l]
boots = [m for m in msgs if m.get('event') == 'message' and 'system.boot' in m.get('tags', [])]
print(f'  Boot notifications: {len(boots)}')
for m in boots:
    print(f'    → {m[\"message\"]}')
if boots:
    print('  ✅ Boot notification: PASS')
else:
    print('  ❌ Boot notification: FAIL (hub may have booted >30min ago)')
"
echo ""
