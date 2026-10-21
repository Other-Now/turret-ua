#!/usr/bin/env bash
# Live media under impairment: alice -> impair-relay -> bob. Bob advertises the
# relay's port in his SDP (--sdp-port), so alice's RTP crosses the relay, which
# adds delay + |N(0, sigma)| jitter and random loss (userspace netem).
# For each sigma, reports what bob's RFC 3550 receiver measured vs what was injected.
#   scripts/impair_run.sh <build_dir> <out_dir> [seconds]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
B=$(cd "${1:-$HERE/build}" && pwd)
OUT=$(mkdir -p "${2:-$HERE/results/local}" && cd "${2:-$HERE/results/local}" && pwd)
SECS=${3:-20}
LOSS=${LOSS:-0.01}
cd "$OUT"
echo "sigma_ms,loss_injected,relay_dropped,rx,lost_rfc3550,jitter_ms,max_jitter_ms,jb_depth,jb_late,jb_concealed,jb_played" > impair.csv
for SIGMA in 0 10 25; do
  "$B/impair-relay" --listen 127.0.0.1:47000 --to 127.0.0.1:41000 --delay 20 --jitter $SIGMA --loss $LOSS \
    --seed 7 --duration $(( SECS * 1000 + 4000 )) > relay_$SIGMA.log 2>&1 &
  R=$!
  "$B/turret-cli" --bind 127.0.0.1:5070 --user bob --auto-answer --rtp-base 41000 --sdp-port 47000 --jb 60 \
    --exit-after-calls 1 --json impair_bob_$SIGMA.json --quiet > impair_bob_$SIGMA.log 2>&1 &
  Bp=$!
  sleep 0.3
  "$B/turret-cli" --bind 127.0.0.1:5080 --user alice --rtp-base 42000 --call sip:bob@127.0.0.1:5070 \
    --hangup-at $(( SECS * 1000 )) --quiet --json impair_alice_$SIGMA.json > impair_alice_$SIGMA.log 2>&1
  wait $Bp; wait $R
  python3 - "$SIGMA" "$LOSS" <<'EOF' >> impair.csv
import json, sys
sigma, loss = sys.argv[1], sys.argv[2]
r = json.load(open(f"impair_bob_{sigma}.json"))["calls"][0]["rtp"]
relay = json.loads(open(f"relay_{sigma}.log").read().strip().splitlines()[-1])
print(f"{sigma},{loss},{relay['dropped']},{r['received']},{r['lost']},{r['jitter_ms']:.2f},{r['max_jitter_ms']:.2f},"
      f"{r['jb_depth_ms']},{r['jb_late']},{r['jb_concealed']},{r['jb_played']}")
EOF
done
column -s, -t impair.csv
