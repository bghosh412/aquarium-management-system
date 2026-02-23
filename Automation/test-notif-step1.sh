#!/bin/bash
# Step 1: Fire all CRUD operations against the hub
set -e
HUB="http://192.168.1.53"

echo "$(date +%s)" > /tmp/notif_test_since.txt
echo "📍 Start timestamp: $(cat /tmp/notif_test_since.txt)"

echo "━━━ Create Aquarium ━━━"
curl -s "$HUB/api/aquariums" \
  -H "Content-Type: application/json" \
  -d '{"name":"CurlTest Tank","volumeLiters":42,"tankType":"Planted","location":"Script Room"}'
echo ""
sleep 1

# Get the ID of the just-created aquarium
AQ_ID=$(curl -s "$HUB/api/aquariums" | python3 -c "
import sys,json
aq = json.load(sys.stdin)['aquariums']
match = [a for a in aq if a['name'] == 'CurlTest Tank']
print(match[-1]['id'] if match else '?')
")
echo "  ID: $AQ_ID"

echo "━━━ Update Aquarium ━━━"
curl -s "$HUB/api/aquarium/update?id=$AQ_ID" \
  -H "Content-Type: application/json" \
  -d '{"name":"CurlTest Tank v2","volumeLiters":99}'
echo ""
sleep 1

echo "━━━ Delete Aquarium ━━━"
curl -s -X POST "$HUB/api/aquarium/delete?id=$AQ_ID"
echo ""
sleep 1

echo "━━━ Delete Device (fake MAC — tests the API path) ━━━"
curl -s "$HUB/api/delete-device" \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF"}'
echo ""

echo ""
echo "✅ All CRUD operations complete. Now run step2 after 70 seconds."
echo "   Saved timestamp to /tmp/notif_test_since.txt"
