#!/usr/bin/env bash
# Real PBX interop: three turret-cli phones register to Asterisk 20 with digest
# auth, alice calls bob THROUGH Asterisk (INVITE is challenged too), holds,
# resumes, then blind-transfers bob to carol (REFER handled by Asterisk's
# res_pjsip_refer). RTP is relayed by Asterisk (direct_media=no).
#
#   scripts/asterisk_interop.sh <build_dir> <out_dir>
# Env: ASTERISK=/usr/sbin/asterisk, AST_MODDIR=/usr/lib/x86_64-linux-gnu/asterisk/modules
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
B=$(cd "${1:-$HERE/build}" && pwd)
OUT=$(mkdir -p "${2:-$HERE/results/local}" && cd "${2:-$HERE/results/local}" && pwd)
AST=${ASTERISK:-/usr/sbin/asterisk}
MODDIR=${AST_MODDIR:-/usr/lib/x86_64-linux-gnu/asterisk/modules}
DATADIR=${AST_DATADIR:-/usr/share/asterisk}
RUN=$(mktemp -d)
mkdir -p "$RUN"/{etc,run,log,lib,spool,db,keys}
cp "$HERE"/asterisk/*.conf "$RUN/etc/"
cat > "$RUN/etc/asterisk.conf" <<EOF
[directories]
astetcdir => $RUN/etc
astmoddir => $MODDIR
astvarlibdir => $RUN/lib
astdbdir => $RUN/db
astkeydir => $RUN/keys
astdatadir => $DATADIR
astagidir => $RUN/lib/agi
astspooldir => $RUN/spool
astrundir => $RUN/run
astlogdir => $RUN/log
astsbindir => $(dirname "$AST")
[options]
nocolor = yes
EOF
cat > "$RUN/etc/logger.conf" <<EOF
[logfiles]
messages => notice,warning,error,verbose
EOF
cat > "$RUN/etc/rtp.conf" <<EOF
[general]
rtpstart=20000
rtpend=20100
EOF
echo "[default]" > "$RUN/etc/musiconhold.conf"

cd "$OUT"
rm -f ast_*.log ast_*.json ast_*.pcap
"$AST" -C "$RUN/etc/asterisk.conf" -f -vvv > ast_asterisk.log 2>&1 &
A=$!
for i in $(seq 50); do "$AST" -C "$RUN/etc/asterisk.conf" -rx "core waitfullybooted" >/dev/null 2>&1 && break; sleep 0.2; done
"$AST" -C "$RUN/etc/asterisk.conf" -rx "pjsip set logger on" > /dev/null 2>&1

REG=sip:127.0.0.1:5060
"$B/turret-cli" --bind 127.0.0.1:5072 --user carol --registrar $REG --password carol-pw --auto-answer --answer-delay 300 \
  --rtp-base 46000 --duration 12000 --pcap ast_carol.pcap --json ast_carol.json > ast_carol.log 2>&1 &
C=$!
"$B/turret-cli" --bind 127.0.0.1:5071 --user bob --registrar $REG --password bob-pw --auto-answer --answer-delay 300 \
  --rtp-base 45000 --duration 12000 --pcap ast_bob.pcap --json ast_bob.json > ast_bob.log 2>&1 &
Bp=$!
sleep 1
"$B/turret-cli" --bind 127.0.0.1:5070 --user alice --registrar $REG --password alice-pw \
  --call sip:bob@127.0.0.1:5060 --rtp-base 44000 --hold-at 2000 --resume-at 4000 \
  --transfer-at 6000 --transfer-to sip:carol@127.0.0.1:5060 --duration 11000 \
  --pcap ast_alice.pcap --json ast_alice.json > ast_alice.log 2>&1
wait $C $Bp
"$AST" -C "$RUN/etc/asterisk.conf" -rx "core stop now" > /dev/null 2>&1
wait $A 2>/dev/null
cp "$RUN/log/messages" ast_messages.log 2>/dev/null

python3 - <<'EOF'
import json
def j(n): return json.load(open(f"ast_{n}.json"))
a, b, c = j("alice"), j("bob"), j("carol")
ok = True
def check(cond, what):
    global ok
    print(("PASS " if cond else "FAIL ") + what); ok &= cond
check(a["auth_retries"] >= 2, f"alice digest auth answered {a['auth_retries']} challenges (REGISTER + INVITE)")
check(a["connected"] == 1, "alice -> Asterisk -> bob connected")
check(a["transfers_done"] == 1, "alice's REFER succeeded (NOTIFY 2xx from Asterisk)")
check(c["calls_in"] == 1 and c["connected"] == 1, "carol received the transferred call")
check(a["illegal_transitions"] + b["illegal_transitions"] + c["illegal_transitions"] == 0, "0 illegal transitions")
r = a["calls"][0].get("rtp", {})
check(r.get("received", 0) > 50, f"alice got RTP relayed by Asterisk: {r}")
open("ast_summary.txt", "w").write(json.dumps({"alice": a, "bob": b, "carol": c}, indent=1))
raise SystemExit(0 if ok else 1)
EOF
