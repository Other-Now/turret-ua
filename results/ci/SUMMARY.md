# CI results: GitHub Actions run 39601194884 (ubuntu-24.04)

All 5 jobs green. Raw files are next to this one.

| Job | Result |
|---|---|
| build-test | unit_tests **69/69** (300-seed chaos: 300/300 runs connected at hangup, 0 illegal transitions, 0 leaks); gui_tests **2/2** offscreen |
| tsan (clang) | 3 runs x 69 tests, **ThreadSanitizer warnings: 0** |
| asan-ubsan | 69/69, 0 reports |
| fuzz (clang) | 3 saved regressions replayed clean; `fuzz_sip_parser` **4,592,448** execs / 150 s, `fuzz_ua` **1,475,509** execs / 150 s, 0 findings |
| interop | SIPp **6/6** + Asterisk 20 **6/6** (details below); real `tcpdump -i lo` capture: **14,399 frames, 0 malformed/expert-error** |

## SIPp (sip-tester 3.7.2)

```
PASS uac_load  calls=2000 rate=200cps sipp_ok=2000 sipp_failed=0 turret_connected=2000 illegal=0 wall=10.5s
PASS uas_basic
PASS uas_hold  sendonly/sendrecv offers verified by SIPp; RTP echoed 175/176, 0 lost, max jitter 0.63 ms
PASS uas_491   491 ACKed, retry after 3031 ms (RFC 3261 14.1 window for the Call-ID owner: 2100-4000)
PASS uac_refer 202 + NOTIFY "100 Trying" + NOTIFY "200 OK" verified by SIPp; carol connected
PASS tshark    0 malformed/error frames in all app-written captures
```

## Asterisk 20.6 (apt), PJSIP, direct_media=no

- **SIP:**
  - Alice answered 2 digest challenges (REGISTER, INVITE), and Bob and Carol 1
    each (REGISTER).
  - Alice -> Asterisk -> Bob connected, then hold, then resume.
  - REFER to Carol: Asterisk's NOTIFY 2xx came back and Alice BYE'd.
  - Carol connected. 0 illegal transitions.
- **Media through the PBX:**

| Leg | Received | Played | Late | Re-anchors / SSRCs | Max jitter |
|---|---:|---:|---:|---:|---:|
| alice | 299 | 295 | 0 | 2 / 2 | 0.70 ms |
| bob | 521 | 510 | 0 | 4 / 3 | 1.02 ms |
| carol | 219 | 216 | 0 | 0 / 1 | 0.63 ms |

  Asterisk changes SSRC across hold/re-bridge on a clean runner too (2-3
  sources per leg). "lost: 100" on alice/bob is the RFC 3550 sequence gap
  Asterisk leaves on the original SSRC; it isn't network loss.

## Live impairment (alice -> impair-relay -> bob, 20 s, 1% loss, jb 60 ms)

| sigma ms | relay dropped | RFC 3550 lost | J ms | max J ms | late | played |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 9 | 9 | 0.81 | 0.90 | 0 | 988 |
| 10 | 9 | 9 | 8.32 | 11.01 | 0 | 987 |
| 25 | 9 | 8 | 21.97 | 26.60 | 3 | 984 |

## Jitter-buffer sweep

`jb_sweep.md` (deterministic; identical to the local run):
- sd 25 ms Gaussian: under 1% late needs 60 ms = 3.5x mean J.
- Wi-Fi spikes (2% of packets delayed 75-150 ms): 140 ms = 17.8x J.
