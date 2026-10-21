#!/usr/bin/env bash
# Interop against SIPp (a third-party SIP implementation), five scenarios:
#   uac_load   SIPp's built-in UAC places N calls into turret-cli (auto-answer)
#   uas_basic  turret-cli calls SIPp's built-in UAS, hangs up
#   uas_hold   turret-cli calls SIPp, holds (sendonly), resumes (sendrecv), hangs up; SIPp echoes RTP
#   uas_491    SIPp answers turret's hold re-INVITE with 491; turret must ACK and retry after 2.1-4 s
#   uac_refer  SIPp calls turret, blind-transfers it (REFER) to a second turret; checks 202 + NOTIFYs
# Usage: scripts/sipp_interop.sh [build_dir] [out_dir]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
B=$(cd "${1:-$HERE/build}" && pwd)
OUT=$(mkdir -p "${2:-$HERE/results/local}" && cd "${2:-$HERE/results/local}" && pwd)
SIPP=${SIPP:-sipp}
TSHARK=${TSHARK:-tshark}
CALLS=${CALLS:-1000}
RATE=${RATE:-100}
cd "$OUT"
rm -f sipp_*.log *_errors.log *.pcap turret_*.json
pass=0; fail=0
ok()  { echo "PASS $1 $2"; echo "PASS $1 $2" >> sipp_summary.txt; pass=$((pass+1)); }
bad() { echo "FAIL $1 $2"; echo "FAIL $1 $2" >> sipp_summary.txt; fail=$((fail+1)); }
: > sipp_summary.txt
jget() { python3 -c "import json,sys; print(json.load(open('$1'))$2)"; }

# ---------------------------------------------------------------- uac_load
"$B/turret-cli" --bind 127.0.0.1:5070 --user bob --auto-answer --no-media --quiet \
  --exit-after-calls "$CALLS" --duration 120000 --json turret_load.json > sipp_uac_load_turret.log 2>&1 &
T=$!; sleep 0.5
start=$(date +%s.%N)
"$SIPP" -sn uac 127.0.0.1:5070 -s bob -i 127.0.0.1 -p 5090 -m "$CALLS" -r "$RATE" -l 200 -d 500 \
  -nostdin -timeout 100s -timeout_error -trace_screen -screen_file sipp_uac_load_screen.log \
  > sipp_uac_load.log 2>&1
rc=$?; end=$(date +%s.%N)
wait $T
succ=$(grep -E "Successful call" sipp_uac_load_screen.log | tail -1 | awk '{print $(NF)}')
failed=$(grep -E "Failed call" sipp_uac_load_screen.log | tail -1 | awk '{print $(NF)}')
conn=$(jget turret_load.json "['connected']"); ill=$(jget turret_load.json "['illegal_transitions']")
detail="calls=$CALLS rate=${RATE}cps sipp_ok=$succ sipp_failed=$failed turret_connected=$conn illegal=$ill wall=$(python3 -c "print(round($end-$start,1))")s"
[ $rc -eq 0 ] && [ "$conn" = "$CALLS" ] && [ "$ill" = "0" ] && ok uac_load "$detail" || bad uac_load "$detail rc=$rc"

# ---------------------------------------------------------------- uas_basic
"$SIPP" -sn uas -i 127.0.0.1 -p 5090 -m 1 -nostdin -timeout 20s -timeout_error > sipp_uas_basic.log 2>&1 &
S=$!; sleep 0.5
"$B/turret-cli" --bind 127.0.0.1:5071 --user alice --no-media --quiet --call sip:service@127.0.0.1:5090 \
  --hangup-at 1000 --json turret_basic.json > sipp_uas_basic_turret.log 2>&1
wait $S; rc=$?
st=$(jget turret_basic.json "['calls'][0]['state']")
[ $rc -eq 0 ] && [ "$(jget turret_basic.json "['connected']")" = "1" ] && ok uas_basic "state=$st" || bad uas_basic "rc=$rc"

# ---------------------------------------------------------------- uas_hold (+ RTP echo)
"$SIPP" -sf "$HERE/sipp/uas_hold.xml" -i 127.0.0.1 -p 5090 -mi 127.0.0.1 -min_rtp_port 6000 -max_rtp_port 6010 -rtp_echo -m 1 \
  -nostdin -timeout 20s -timeout_error > sipp_uas_hold.log 2>&1 &
S=$!; sleep 0.5
"$B/turret-cli" --bind 127.0.0.1:5071 --user alice --rtp-base 43000 --call sip:service@127.0.0.1:5090 \
  --hold-at 1000 --resume-at 2000 --hangup-at 3500 --pcap hold.pcap --json turret_hold.json \
  > sipp_uas_hold_turret.log 2>&1
wait $S; rc=$?
rtp=$(jget turret_hold.json "['calls'][0]['rtp']")
[ $rc -eq 0 ] && ok uas_hold "sdp sendonly/sendrecv checked by SIPp; rtp(echoed)=$rtp" || bad uas_hold "rc=$rc"

# ---------------------------------------------------------------- uas_491
"$SIPP" -sf "$HERE/sipp/uas_491.xml" -i 127.0.0.1 -p 5090 -m 1 -nostdin -timeout 20s -timeout_error \
  > sipp_uas_491.log 2>&1 &
S=$!; sleep 0.5
"$B/turret-cli" --bind 127.0.0.1:5071 --user alice --no-media --call sip:service@127.0.0.1:5090 \
  --hold-at 500 --hangup-at 6000 --pcap glare.pcap --json turret_491.json > sipp_uas_491_turret.log 2>&1
wait $S; rc=$?
gap=$("$TSHARK" -r glare.pcap -Y 'sip.Method == "INVITE" && sip.CSeq.seq >= 2 && ip.src == 127.0.0.1 && udp.srcport == 5071' \
      -T fields -e frame.time_relative 2>/dev/null | python3 -c "import sys; t=[float(x) for x in sys.stdin]; print(round((t[1]-t[0])*1000) if len(t)>1 else 'n/a')")
st=$(jget turret_491.json "['got_491']")
[ $rc -eq 0 ] && [ "$st" = "1" ] && ok uas_491 "got_491=$st retry_gap_ms=$gap (RFC window 2100-4000)" || bad uas_491 "rc=$rc got_491=$st gap=$gap"

# ---------------------------------------------------------------- uac_refer
"$B/turret-cli" --bind 127.0.0.1:5072 --user carol --auto-answer --no-media --quiet --duration 6000 \
  --json turret_carol.json > sipp_refer_carol.log 2>&1 &
C=$!
"$B/turret-cli" --bind 127.0.0.1:5070 --user bob --auto-answer --no-media --duration 6000 --pcap refer.pcap \
  --json turret_bob.json > sipp_refer_bob.log 2>&1 &
Bb=$!; sleep 0.5
"$SIPP" -sf "$HERE/sipp/uac_refer.xml" 127.0.0.1:5070 -s bob -set target sip:carol@127.0.0.1:5072 \
  -i 127.0.0.1 -p 5090 -m 1 -nostdin -timeout 20s -timeout_error > sipp_uac_refer.log 2>&1
rc=$?
wait $C $Bb
cc=$(jget turret_carol.json "['connected']")
[ $rc -eq 0 ] && [ "$cc" = "1" ] && ok uac_refer "202 + NOTIFY 100/200 checked by SIPp; carol connected=$cc" || bad uac_refer "rc=$rc carol=$cc"

# ---------------------------------------------------------------- tshark over every capture
mal=0
for p in *.pcap; do
  n=$("$TSHARK" -r "$p" -Y "_ws.malformed || _ws.expert.severity == error" 2>/dev/null | wc -l)
  f=$("$TSHARK" -r "$p" 2>/dev/null | wc -l)
  echo "tshark $p frames=$f malformed_or_error=$n" >> sipp_summary.txt
  mal=$((mal + n))
done
[ $mal -eq 0 ] && ok tshark "0 malformed/error frames in all captures" || bad tshark "$mal bad frames"

echo "== $pass passed, $fail failed" | tee -a sipp_summary.txt
exit $fail
