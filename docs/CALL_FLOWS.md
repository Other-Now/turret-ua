# Call flows (captured from the tests)

Every ladder below is printed by the real `UserAgent`s talking over the
deterministic `SimNetwork` (20 ms one-way latency, shared fake clock). Get them
yourself with:

```
TU_LADDER=1 ./build/unit_tests --gtest_filter='Races.*'
```

The SIP ladders from live runs (two `turret-cli` processes, SIPp, Asterisk) were
also checked with `tshark`: 0 malformed or expert-error frames across all captures.

---

## 1. CANCEL crosses 200 OK (`Races.CancelCrossesTwoHundredOk`)

Bob answers at the same instant Alice hangs up. Bob's 200 and Alice's CANCEL are
both on the wire at once.

```
alice -> bob    INVITE
bob   -> alice  100 INVITE
bob   -> alice  180 INVITE
bob   -> alice  200 INVITE      <- bob answered...
alice -> bob    CANCEL          <- ...while alice hung up
alice -> bob    ACK             <- alice got a 200 for a call she cancelled: ACK it
alice -> bob    BYE             <- ...and immediately BYE it (RFC 3261 15)
bob   -> alice  200 CANCEL      <- CANCEL after a final response "has no effect" (9.2): 200, no 487
bob   -> alice  200 BYE
```

What the code does:
- Alice's `hangup()` moves her to `Terminated` immediately. When the 200 arrives
  on a terminated call she sends ACK and then BYE.
- Bob already sent 200, so the CANCEL gets 200 but no 487.

Both sides end `Terminated`. `cancel_lost_race` is 1, and Bob's ACK stopped his
2xx retransmit timer.

## 2. BYE crosses re-INVITE (`Races.ByeCrossesReInvite`)

Alice puts Bob on hold at the same instant Bob hangs up.

```
alice -> bob    INVITE (hold, a=sendonly)
bob   -> alice  BYE
bob   -> alice  100 INVITE
bob   -> alice  481 INVITE      <- bob's dialog is gone: Call/Transaction Does Not Exist
alice -> bob    200 BYE
alice -> bob    ACK             <- ACK for the 481 (built by the INVITE client transaction)
```

Alice's call ends with reason "remote hangup", not "dialog gone": the BYE won,
and the later 481 is ignored on a terminated call. Nothing resurrects her call,
and 70 s later both sides have 0 live transactions.

## 3. Re-INVITE glare (`Races.ReInviteGlareResolvesWith491AndBackoff`)

Both sides press HOLD at the same time.

```
alice -> bob    INVITE (hold)
bob   -> alice  INVITE (hold)
bob   -> alice  491 INVITE      <- each side has its own re-INVITE pending
alice -> bob    491 INVITE
alice -> bob    ACK             <- ACKs for the 491s
bob   -> alice  ACK
bob   -> alice  INVITE          <- bob (not Call-ID owner) retries after 0-2.0 s
alice -> bob    200 INVITE
bob   -> alice  ACK
alice -> bob    INVITE          <- alice (Call-ID owner) retries after 2.1-4.0 s
bob   -> alice  200 INVITE
alice -> bob    ACK
```

The RFC 3261 14.1 back-off windows do not overlap, so a second collision is
impossible. `GlareConvergesForEverySeed` checks that for 200 RNG seeds: exactly
two 491s each time, and both ends finish `OnHold`. Against SIPp
(`sipp/uas_491.xml`), the measured retry gap was 3,047 ms and 3,202 ms in two
local runs, inside the 2.1-4.0 s window.

## 4. REFER while on hold (`Races.ReferWhileOnHold`)

Alice holds Bob, then blind-transfers him to Carol.

```
alice -> bob    INVITE (hold)  / 100 / 200 / ACK
alice -> bob    REFER  Refer-To: <sip:carol@...>
bob   -> alice  202 REFER
bob   -> alice  NOTIFY  (sipfrag "SIP/2.0 100 Trying", Subscription-State: active)
bob   -> carol  INVITE
alice -> bob    200 NOTIFY
carol -> bob    100 / 180 / 200 INVITE
bob   -> carol  ACK
bob   -> alice  NOTIFY  (sipfrag "SIP/2.0 200 OK", Subscription-State: terminated)
alice -> bob    200 NOTIFY
alice -> bob    BYE             <- transferor drops out after the success NOTIFY
bob   -> alice  200 BYE
```

The variant `ReferRacesItsOwnHoldReInvite` sends the REFER before the hold's
200 has come back. Alice goes Connected -> Transferring -> Terminated, and the
late hold answer only updates the state she would fall back to if the transfer
failed. `TransferToBusyTargetFallsBackToHold` covers the failure path: Carol
declines, the NOTIFY carries 603, and Alice returns to OnHold.

## 5. Hang up right after a re-INVITE whose 200 is lost (`Races.HangupRightAfterReInviteWithLost200`)

This is the regression for the bug the random-loss chaos test found (seed 7).

```
alice -> bob    INVITE (hold)
alice -> bob    BYE             <- alice hangs up immediately
bob   -> alice  100 INVITE      <- alice's re-INVITE transaction is now in Proceeding
bob   -> alice  200 INVITE  (lost)
bob   -> alice  200 BYE
bob   -> alice  200 INVITE      <- bob keeps retransmitting the 2xx after the BYE (13.3.1.4)
alice -> bob    ACK             <- alice's INVITE client transaction finally completes
```

Before the fix, Bob stopped retransmitting the 2xx when the BYE arrived.
RFC 3261 gives an INVITE client transaction in Proceeding no timeout, so Alice's
transaction leaked forever.

## Live: hold / resume between two processes (tshark)

```
0.000  :5080 -> :5070  INVITE         1 INVITE
0.001  :5070 -> :5080  100 / 180 / 200
0.002  :5080 -> :5070  ACK            1 ACK
2.003  :5080 -> :5070  INVITE         2 INVITE   (a=sendonly)
2.003  :5070 -> :5080  100 / 200                 (a=recvonly)
2.003  :5080 -> :5070  ACK            2 ACK
4.002  :5080 -> :5070  INVITE         3 INVITE   (a=sendrecv)
4.002  :5070 -> :5080  100 / 200
4.002  :5080 -> :5070  ACK            3 ACK
6.002  :5080 -> :5070  BYE            4 BYE
6.004  :5070 -> :5080  200            4 BYE
```

Wireshark's RTP analysis of the same capture: 0 lost on both streams, and a max
jitter of 5.517 ms on the Bob->Alice stream. turret-ua's own RFC 3550 receiver
reported 5.511 ms for that stream.
