# turret-ua study sheet

This is the 1-2 day prep sheet: the protocol, the design decisions, the bugs and
their numbers, and the questions a SIP/telephony interviewer is likely to ask.
Read sections 1-4 first. Sections 5-6 are your stories.

---

## 1. SIP in five minutes

SIP (RFC 3261) is a text protocol, HTTP-like, usually over UDP port 5060. It
sets up sessions; it carries no audio. Audio goes over RTP, on ports negotiated
in an SDP body.

There are three layers. Keep them separate in your head, because interviewers
probe exactly this:

| Layer | Lifetime | Identified by | In this repo |
|---|---|---|---|
| Transaction | one request + its responses | top Via `branch` + CSeq method | `src/sip/transaction.*` |
| Dialog | the whole call | Call-ID + local tag + remote tag | `UserAgent::Call` (dialog fields) |
| Session | the media | SDP offer/answer | `src/sip/sdp.*`, `src/rtp/*` |

- **Transaction.** Retransmissions over UDP, matching responses to requests,
  absorbing duplicates. The `z9hG4bK` prefix on a branch is the RFC 3261
  "magic cookie".
- **Dialog.** From/To tags, CSeq ordering, the remote target from Contact.
- **INVITE is special.** It takes three messages (INVITE, 200, ACK) because a
  human may take seconds to answer. Its transaction is a different state machine
  from every other request.

### Message anatomy

```
INVITE sip:bob@10.0.0.2:5060 SIP/2.0                 <- request line
Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK...;rport   <- where to send responses + txn id
From: <sip:alice@10.0.0.1>;tag=a1                    <- caller + caller's dialog tag
To: <sip:bob@10.0.0.2>                               <- callee (tag added by callee's response)
Call-ID: x@10.0.0.1                                  <- dialog id part 1
CSeq: 1 INVITE                                       <- ordering within the dialog
Contact: <sip:alice@10.0.0.1:5060>                   <- where to send in-dialog requests
Content-Type: application/sdp
Content-Length: 142
                                                     <- blank line, then the SDP body
```

Mandatory headers are Via, From, To, Call-ID, CSeq and Max-Forwards. The parser
rejects a message missing any of the first five.

---

## 2. The four transaction state machines (RFC 3261 section 17)

T1 = 500 ms (RTT estimate), T2 = 4 s, T4 = 5 s. 64*T1 = 32 s is the
"give up" time everywhere.

| Machine | States | Timers |
|---|---|---|
| INVITE client (ICT) | Calling -> Proceeding -> Completed -> Terminated | **A** retransmit at T1, doubling with no cap, only in Calling. **B** = 64*T1 timeout, only in Calling. **D** = 32 s wait in Completed to absorb retransmitted finals. |
| non-INVITE client (NICT) | Trying -> Proceeding -> Completed -> Terminated | **E** retransmit, doubling up to T2; in Proceeding, every T2. **F** = 64*T1 timeout. **K** = T4 wait. |
| INVITE server (IST) | Proceeding -> Completed -> Confirmed -> Terminated | Sends 100 Trying at once. **G** retransmits a non-2xx final until the ACK. **H** = 64*T1 waiting for the ACK. **I** = T4. |
| non-INVITE server (NIST) | Trying -> Proceeding -> Completed -> Terminated | **J** = 64*T1 replaying the final response to retransmits. |

The 2xx-to-INVITE subtlety is asked about a lot:

- **Who sends the ACK.** The ICT terminates on a 2xx and hands it to the UA
  core. The **core** ACKs it, as a new transaction with a new branch. For a
  non-2xx final, the **transaction** builds the ACK with the same branch.
- **Who retransmits the 2xx.** The **UA core** on the server side, at T1, 2T1,
  ... capped at T2, until the ACK arrives or 64*T1 passes. If the ACK never
  comes, send BYE.
- **Why.** A 2xx can fork through proxies and create several dialogs. Each
  dialog needs its own end-to-end ACK, so it can't be hop-by-hop
  transaction state.

`tests/test_transaction.cpp` asserts exact schedules with gmock and a fake
clock. For example, Timer A sends at 0, 500, 1500, 3500, 7500, 15500 and
31500 ms, then Timer B fires at 32 s.

**Our Timer C:** RFC 3261 gives an ICT in Proceeding no timeout. The TU is
supposed to CANCEL. We add a 180 s guard so a missing final can't leak a
transaction. That's non-standard for a UA (Timer C is a proxy timer), and it's
documented as a deliberate choice.

---

## 3. SDP offer/answer and hold (RFC 3264)

```
m=audio 40002 RTP/AVP 0        <- port + payload type 0 = PCMU (G.711 mu-law, 8 kHz)
c=IN IP4 10.0.0.1              <- where to send RTP
a=sendrecv | sendonly | recvonly | inactive
```

- **Hold.** Send a re-INVITE with `a=sendonly`. The peer answers `a=recvonly`.
  The holder keeps sending (music on hold) and the held party stops.
- **Resume.** Send `a=sendrecv`.
- **Old-style hold** (RFC 2543): `c=0.0.0.0`. The parser treats it as sendonly.
- **The answer's direction** is the mirror of the offer, intersected with what
  you want. If both sides hold, you get `inactive`. That's
  `sdp::answer_dir`, and the truth table is in `tests/test_sdp.cpp`.

---

## 4. Architecture and threading (the "race conditions" part of the JD)

```
  Qt GUI thread                      SIP/media thread (core::EventLoop)
  -------------                      ----------------------------------
  QML button
    -> CallModel::hold(id)  --post-->  UserAgent::hold(id)
                                       UDP socket readable -> TransactionLayer -> UserAgent
                                       timers (A/B/E/F/G/H/..., 2xx retransmit, RTP 20 ms)
  CallModel::apply(info)  <--QueuedConnection--  on_call_event(info)
    -> dataChanged -> QML delegate
```

- **One thread owns all call state.** Network input, timer expiries and user
  commands are serialised onto one `EventLoop` thread. Two events for the same
  call can never interleave inside a handler, so `UserAgent` has no locks at
  all. That's deliberate, not an omission.
- **The GUI never touches the UA.** `CallModel` posts closures to the loop, and
  the loop posts `CallInfo` copies back with
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. The UI can't block
  call handling, and call handling can't block the UI.
- **Races still exist, but they're on the wire, not in memory.** Two messages
  cross in the network (CANCEL vs 200, BYE vs re-INVITE, re-INVITE vs
  re-INVITE). The state machine has to make the right decision for every
  arrival order. That's what `tests/test_races.cpp` covers.
- **Checked state transitions.** `set_state()` checks every transition against
  a table (`transition_allowed`). An illegal one is counted, not applied, and
  every test asserts the count is 0. Terminated -> anything is illegal, so
  "a late 200 resurrected the call" bugs become a failing assertion.
- **TSan scope.** `tests/test_threaded.cpp` drives two UAs on two loop threads
  from a third "UI" thread over real UDP. Under clang TSan: 3 runs x 69 tests,
  0 warnings (CI run 39601194884; also clean locally with clang 20). Note that GCC 13's TSan runtime crashed in its own allocator on
  this WSL kernel (it needs `setarch -R` for the ASLR layout, then SEGVs). The
  same tests are clean under ASan+UBSan and clang TSan, so it's a toolchain
  issue, not a race. CI runs clang TSan.
- **Scaling.** One loop handles this load: SIPp pushed 2,000 calls at 200 cps
  with 0 failures in CI (500 at 50 cps on the loaded laptop). The next step is N loops with calls sharded by
  hash(Call-ID), still one owner per call, still no locks in the state machine.

---

## 5. The race tests: what each one proves

| Test | Collision | Correct behaviour |
|---|---|---|
| `CancelCrossesTwoHundredOk` | caller CANCELs as callee answers | callee: 200 to CANCEL, no 487. Caller: ACK the 200, then BYE |
| `CancelBeforeAnyProvisionalIsDeferred` | hang up before any 1xx | RFC 3261 9.1: no CANCEL before a provisional. Send it when the 1xx arrives |
| `ByeCrossesReInvite` | hold vs hangup | the side that hung up answers 481. The other side terminates on the BYE and ignores the 481 |
| `ReInviteGlareResolvesWith491AndBackoff` | both hold at once | both answer 491. Call-ID owner retries in 2.1-4 s, the other in 0-2 s |
| `GlareConvergesForEverySeed` | same, 200 seeds | always exactly two 491s, both end OnHold |
| `ReferWhileOnHold` / `ReferRacesItsOwnHoldReInvite` | transfer during or right after hold | 202, NOTIFY 100, new INVITE, NOTIFY 200, transferor BYEs |
| `TransferToBusyTargetFallsBackToHold` | target declines | NOTIFY 603 -> transferor back to OnHold, call intact |
| `LostAckIsRecoveredBy2xxRetransmission` | ACK lost | 2xx retransmitted, ACK resent from the stray-response path |
| `NoAckAtAllEndsCallAfter64T1` | every ACK lost | callee BYEs at exactly 32 s ("no ACK") |
| `HangupRightAfterReInviteWithLost200` | chaos seed 7 (bug 1 below) | 2xx keeps retransmitting after BYE |
| `RandomLossChaos` | 300 seeds, 5-30% loss, random hold/resume | 0 illegal transitions. Everything ends Terminated with 0 live transactions |

---

## 6. Bugs found and their measured numbers (your stories)

### Bug 1: a BYE stopped the 2xx retransmission, and the peer's transaction leaked forever

- **Found by:** `RandomLossChaos`, seed 7.
- **Sequence:** Alice sends a re-INVITE and hangs up at once. Bob's 100 reaches
  her, so her ICT is in Proceeding. Bob's 200 is lost. Then Bob gets the BYE and
  `finish()` stopped retransmitting the 2xx.
- **Effect:** the ICT in Proceeding has no Timer B, so Alice's transaction lived
  forever. That's one leaked transaction per occurrence, for the life of the
  process.
- **Fix:** RFC 3261 13.3.1.4 says retransmit the 2xx until the ACK or 64*T1, and
  a BYE doesn't change that. Plus the 180 s Timer C guard as a second layer.
- **Regression test:** `HangupRightAfterReInviteWithLost200`.

### Bug 2: hold/resume killed the audio for the rest of the call

Live two-process call: hold at 2 s, resume at 4 s, hang up at 6 s.

| Alice's receive side | played | late (discarded) | concealed | max RFC 3550 jitter |
|---|---:|---:|---:|---:|
| before | 101 | 100 | 196 | 125.3 ms |
| after | 198 of 201 | 0 | 10 | 5.5 ms |

There were two causes:
1. **Sender.** The RTP timestamp only advanced per packet sent, so a 2 s pause
   vanished from the media clock. RFC 3550 5.1 says the timestamp is the
   sampling clock and keeps running. That also produced the 125 ms jitter spike,
   because transit time jumped by 2 s.
2. **Receiver.** The jitter buffer kept stepping through sequence numbers during
   the silence. It ended 100 frames ahead of the resumed stream, so every later
   packet was "late".

Fixes:
- The sender puts frames on a wall-clock 20 ms grid and sets the marker bit on
  the first packet after a pause.
- The jitter buffer pauses after 10 dry frames (200 ms) and re-anchors on the
  next packet.

### Bug 3: Asterisk switched SSRC mid-call, and my jitter buffer assumed one stream

- **Observed in the capture:** during Alice's hold, Asterisk 20.6 kept sending
  RTP on a new SSRC, even though its 200 OK SDP said `recvonly`. On resume it
  went back to the old SSRC, with one stray packet carrying the other stream's
  sequence number.
- **Effect:** the buffer treated the new sequence space as a +25,661 jump and
  kept those packets far ahead of the playout point. Because the queue was
  non-empty, it never paused, so it concealed every frame from then on.
- **Before:** 191 of 299 received packets played, 124 concealed.
- **After:** re-anchor on a new SSRC. A same-SSRC jump over 3000 is only
  believed when the next packet confirms it (RFC 3550 A.1 probation). Stats are
  per SSRC. Result: 295 of 299 played, 0 late.
- **About Asterisk's "lost: 106":** this is an RFC 3550 artifact. Asterisk
  advanced the original SSRC's sequence number while sending on the other one.
  Wireshark independently reports the same 105-106.

### Bug 4: one RTP packet made the jitter estimate 8,894,580 ms

- **Cause:** one marker-bit packet from Asterisk carried a timestamp 1,138,507,800
  ticks away (another leg's clock). J += |D|/16 turned that into 8.9 million ms.
- **Fix:** a marker packet re-baselines transit time instead of feeding J.
  That's the same rule Wireshark's RTP analysis uses.
- **After:** max jitter 42-50 ms on the loaded laptop.

### Bugs 5-7: the parser fuzzer's round-trip check found three bugs

The parser fuzzer doesn't just check "no crash". It also checks that whatever
`parse()` accepts, `serialize()` turns into something `parse()` accepts again,
and that serialising is a fixed point. That check found three bugs:

| # | After | Input | Problem |
|---|---|---|---|
| 5 | 54,781 execs | `Call-ID:` with the value on a folded continuation line | parsed as `" abc"`. In practice a dialog-lookup miss, so the peer gets a 481 |
| 6 | 189,166 execs | whitespace-only continuation line, plus raw NUL bytes in a value | trailing space; NUL accepted. NULs are now rejected (RFC 3261 grammar has no control characters; NUL truncates in C-string consumers) |
| 7 | 790,231 execs | exactly 128 headers | `serialize()` adds Content-Length, giving 129, so our own output failed our own parser. Fixed, and duplicate Content-Length is now rejected (two lengths make the body boundary ambiguous, the SIP version of request smuggling) |

All three inputs are in `fuzz/regressions/` and CI replays them. The UA-level
fuzzer (`fuzz_ua`) feeds datagram sequences into a whole UserAgent and checks
for no crash, no illegal transition, and nothing leaked after the timers run
out. 2.5M runs found nothing. After the three fixes, the parser fuzzer ran 6.8M
clean executions (10 minutes locally plus 150 s in CI).

### Jitter-buffer depth vs RFC 3550 J (bench/jb_sweep, deterministic)

| Network model | Smallest depth with < 1% late | as a multiple of mean J |
|---|---:|---:|
| Gaussian, sd 25 ms | 60 ms | 3.5x |
| Wi-Fi-like: sd 5 ms + 2% spikes of 75-150 ms | 140 ms | 17.8x |

**Takeaway:** J is a smoothed mean of |D|, so it barely sees rare spikes. A
"depth = k*J" adaptive rule tuned on Gaussian jitter under-buffers bursty
networks by about 5x. A real adaptive buffer needs a tail estimate (a
percentile of transit time), not J.

---

## 7. RTP and RTCP cheat sheet

- **RTP header (12 bytes):** V=2, P, X, CC, M (marker), PT, sequence (16-bit),
  timestamp (32-bit, 8 kHz for G.711), SSRC.
- **G.711 framing:** 20 ms = 160 samples = 160 bytes. 50 packets per second.
  64 kbit/s payload.
- **Loss (A.1):** extended max sequence (cycles << 16 | max_seq). Expected =
  ext_max - base + 1. Lost = expected - received. It can go negative with
  duplicates.
- **Jitter (A.8):**
  - D = (R_j - R_i) - (S_j - S_i), in timestamp units.
  - J += (|D| - J)/16.
  - Reported in RTCP RR in timestamp units; divide by 8 for ms.
- **RTCP RR** (PT 201) per source: fraction lost (8-bit fixed point), cumulative
  lost (24-bit signed), extended highest sequence, jitter, LSR, DLSR. Sent every
  5 s on the RTP port + 1.

---

## 8. Likely interview questions, with short answers

**Q: Why does INVITE need ACK and other requests don't?**
A: Because the final response can take seconds (a human answering), so the
server needs proof the 2xx arrived, and the 2xx is retransmitted end-to-end
until that proof comes. Non-INVITE finals come back immediately and ride the
transaction's retransmission instead.

**Q: What's the difference between a transaction and a dialog?**
A: A transaction is one request and its responses, matched by branch. A dialog
is the peer-to-peer relationship for the call, matched by Call-ID plus both
tags, and it outlives many transactions (INVITE, re-INVITEs, REFER, NOTIFY,
BYE).

**Q: CANCEL arrives after you sent 200 OK. What happens?**
A: You answer the CANCEL with 200, but it has no effect: no 487. The caller
must ACK your 200 and send BYE. (`CancelCrossesTwoHundredOk`.)

**Q: What's glare and how do you resolve it?**
A: Both sides send re-INVITEs at once. Each answers the other with 491. The side
that owns the Call-ID (the original caller) retries after 2.1-4 s, the other
after 0-2 s. The windows don't overlap, so they can't collide twice. (Tested
over 200 seeds, and against SIPp: 3.0-3.2 s measured.)

**Q: How do you make call handling thread-safe?**
A: Single ownership: one event-loop thread per group of calls owns the state,
and other threads post commands to it. There are no locks in the state machine.
Then I test the remaining real races, the ones on the wire, deterministically
with a fake network and clock, and run the multi-threaded tests under TSan.

**Q: How do you test timers without sleeping?**
A: Time is an interface (`core::Timers`). Tests use `FakeTimers::advance()`, so
a 32 s Timer B test runs in microseconds and checks exact retransmit
timestamps.

**Q: How does blind transfer work?**
A: The transferor sends REFER with Refer-To. The transferee answers 202, sends
NOTIFY with "SIP/2.0 100 Trying" in a message/sipfrag body, INVITEs the target,
then sends NOTIFY "200 OK" with Subscription-State terminated. The transferor
then BYEs the original call. Attended transfer adds a Replaces header
(RFC 3891); that's not implemented here.

**Q: Hold through Asterisk: what did you see?**
A: Asterisk is a B2BUA, so each phone has its own dialog with Asterisk.
Asterisk's media source changes when it re-bridges. In the capture it switched
SSRC during hold and back on resume, and one packet carried another leg's
timestamp. Both broke my first jitter buffer and jitter estimator (bugs 3-4).

**Q: What would you need for production?**
A: TCP/TLS and SRTP, Record-Route/route sets for proxies, DNS (RFC 3263), PRACK
(100rel), session timers (RFC 4028), attended transfer (Replaces), an adaptive
jitter buffer with PLC, real audio I/O, and sharding calls across loops.

---

## 9. What is NOT here (say so if asked)

- **Transport:** UDP only. No TCP, TLS or WebSocket transport, no SRTP or DTLS.
- **Routing:** no Record-Route or route sets. That's fine with a B2BUA like
  Asterisk, but it won't work through a record-routing proxy. IP literals only,
  no DNS.
- **SIP features:** no PRACK, session timers, attended transfer, forking
  handling, or offerless INVITE.
- **Media:**
  - A tone generator and a WAV dump, not a sound card. No Qt Multimedia or ALSA.
  - The jitter buffer is fixed-depth. There's no packet-loss concealment beyond
    silence.
- **Impairment** comes from a userspace relay (`tools/impair_relay.cpp`), not
  `tc netem`. netem needs root, and this WSL box has none.
- **Local timing numbers** (loopback jitter, call rates) were measured while
  another session was compiling LLVM (load average 5-8 on 4 cores). Quote the
  CI numbers for timing.
