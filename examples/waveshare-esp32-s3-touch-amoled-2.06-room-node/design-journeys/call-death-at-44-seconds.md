# The Design Journey: Why Every Call Died at 44 Seconds

*A case study from the Waveshare AMOLED room node — Fredrik and OpenClaw,
two days, six theories, one experiment that cracked it, and three priority
numbers that fixed it.*

This is the story of how the ESP32-S3 room node went from "every voice call
dies at ~44 seconds, every time" to a clean 169-second call with zero errors —
told as it actually happened, wrong turns included, because the wrong turns
are the curriculum.

---

## The system

An ESP32-S3 with a round AMOLED display acts as a room node: it listens for a
wake word, opens a WebRTC voice call to the gateway (a VM on a tailnet), and
streams audio both ways. Three transports coexist during a call:

| Transport | Carries | Path |
|---|---|---|
| WebRTC over TURN | the audio itself | public internet |
| WireGuard tunnel | tailscale traffic (control, peer data) | LAN-direct or DERP relay |
| DERP relay (TLS) | tunnel fallback + peer discovery | public internet |
| WebSocket (TLS) | node ↔ gateway control channel | via the tunnel |

The node runs a small RTOS with ~35 tasks on two cores. The audio pipeline
(AFE, Opus encode/decode, render) runs at high priority by design — audio
glitches are audible, network delays are usually not.

## The symptom

Every call died the same way, at the same age — 28 to 50 seconds in:

```text
W (124457) ml_coord: STUN primary failed after 3 retries, trying fallback
E (125427) ml_derp: TLS write failed: -0x0050      ← relay connection RST
... ~30 seconds of ICE binding keepalive failures ...
→ gateway declares the media peer dead → call torn down
```

The victim's audio cut mid-sentence: a spoken countdown from 20 died at "9, 8"
(48 seconds of rendered audio — the numbers were recoverable from the render
counter, which froze at the cut and never advanced again).

Idle was *always* clean. Only calls died.

## The theories that died first

What makes this debugging story useful is how many plausible theories died
against evidence. Each death narrowed the space.

**1. "The relay keepalive frame is wrong."** The DERP keepalive was being
sent as `NOTE_PREFERRED (0x07)` — a preference hint, not a keepalive.
Switching to `KEEP_ALIVE (0x06)` was protocol-correct (verified against the
upstream `derp.go`) — but the resets continued. *Lesson: protocol correctness
is necessary, not sufficient.*

**2. "The board never answers the server's PING."** The PING handler only
replied when the frame carried a payload. This looked like the smoking gun
until we read the authoritative protocol: the server's PING carries an
8-byte payload, so the old handler was already answering real pings. The fix
stayed (unconditional replies are more robust) but it was not the cause.
*Lesson: verify the protocol from source, not from the failure shape.*

**3. "The network router's DoS protection trips under call load."** A
~23-second cadence of STUN failures looked like a periodic protection window.
But the correlation collapsed when we discovered our control measurement — a
"LAN ping" — was aimed at a host on a different subnet we had no route to.
The evidence was void, not merely inconclusive. *Lesson: a measurement you
haven't validated measures nothing.*

**4. "The board tears down its own WireGuard state."** This one was *real*
but insufficient: peer updates from the coordination map unconditionally reset
the WG slot index and wiped the discovered direct endpoint. Fixing it
(`keypair unusable: 0`, endpoints preserved) cured tunnel flapping — and the
calls still died. *Lesson: a real bug is not necessarily THE bug.*

**5. "DNS dies with the tunnel."** The board's only DNS server was a
forwarder on the gateway, reachable solely through the tunnel — and that
forwarder had been silently dead for ~13 hours (no supervisor, no logging).
Real, fixed (1.1.1.1 as a lane-independent backup, the forwarder under a
KeepAlive agent), DNS failures went from 22+/boot to zero — and the calls
still died. *Lesson: two real bugs can hide a third.*

After five dead theories, we had a board that was protocol-correct, DNS-solid,
churn-free — and calls that still died at 44 seconds.

## The experiment that cracked it

The breakthrough was one instrument, run by Fredrik — the human half of
this collaboration — not by the agent: **a continuous ping from the gateway
to the board's tailscale IP, during a call.**

```text
call starts     → board STOPS answering the ping, immediately
during call     → TURN audio keeps flowing perfectly
call dies       → ping recovers
```

This single observation split the problem space in half:

- The **radio** was fine — audio kept flowing.
- The **WireGuard tunnel path** went dark the *instant* the call started.

What differs between those two paths? Not the radio. The *tasks*. The audio
pipeline and the tunnel's network tasks are separate FreeRTOS tasks — and if
the CPU-scheduling of those tasks is what fails, everything observed so far
follows: idle clean (audio tasks dormant), STUN unanswered (its task starved),
DERP stalls past the relay server's 2-second write deadline (RST), and the
ping recovering exactly at call end (the DSP shutting down frees the CPU).

## The instrument that named it

Before changing anything, we added a measurement: every 5 seconds during a
call, the board logs its audio counters, the wifi RSSI, and the top-3 tasks
by CPU share (FreeRTOS runtime stats, diffed per tick). The next test call
produced:

```text
room_talk_cpu buffer_in=47% Adec=36% aenc_0=33%     ← 116% of the dual-core budget
room_talk_cpu buffer_in=48% Adec=35% aenc_0=30%
room_talk_wifi rssi=-55                             ← radio steady throughout
W: ml_derp: TLS write failed: -0x0050               ← the cut, same second
room_talk_cpu buffer_in=68% aenc_0=48% taskLVGL=19%  ← post-cut spin
```

The media pipeline (buffer_in, Adec, aenc_0) was consuming the entire
dual-core budget. The four microlink network tasks — `ml_net_io`,
`ml_derp_tx` (prio 5), `ml_coord`, `ml_wg_mgr` (prio 7) — sat *below* the
audio tasks (prio 10–15) and received effectively zero CPU for the whole
call. The starver was named.

## The fix — and the regression it caused first

The fix direction was obvious: raise the network tasks above the media tasks.
The first attempt — priority 12 — **broke the board's control websocket
entirely**: 50 consecutive TLS handshake timeouts, never reaching "session
ready". The cause: the websocket/TLS client tasks themselves run at priority
11. Putting the network tasks *above* the handshake preempted the very
connection that brings the node online.

The A/B was visible within one flash cycle:

```text
boot prio 5/7 :  3 connect fails → session ready
boot prio 12  : 50 connect fails → NO ready line at 250s
boot prio 11  :  2 connect fails → session ready     ← shipped
```

Priority 11 — above the media hogs (10), *level* with the TLS tasks
(time-sliced round-robin: both make progress), below the audio-realtime
floor (15) and the radio layers (23). The earlier field lesson applied here
too: *raising a polling task's priority makes it the hog — the ladder, not
the leap.*

## The verification

One test call, with the human's ping running alongside:

- **Ping replies stayed alive during the whole call** — the starved-tunnel
  symptom, gone.
- **169 seconds of continuous audio** (8,238 rendered frames), ending in a
  normal, gateway-initiated hangup.
- **Zero** STUN failures, **zero** relay resets, **zero** keepalive failures
  in the call window.
- The CPU ticks show the media pipeline still eating its share — and the
  network tasks getting theirs.

## The lessons, distilled

1. **Instrument before you fix.** Five plausible fixes shipped before the
   right one; each was real or protocol-correct, none was causal. The 5-second
   tick (audio + RSSI + CPU) found the cause in one call.
2. **A human with a ping is a scientific instrument.** Fredrik's decisive
   observation cost one command and split the hypothesis space in half —
   something a night of automated log-reading had not managed.
3. **Validate your measurements.** The longest-lived wrong theory survived on
   a ping to an unreachable host.
4. **Counters freeze; reads can mislead.** A render counter was once read as
   "success" when it actually recorded the *end* of audio. Check what a
   counter means before celebrating it.
5. **Real bugs can hide other bugs.** Dead forwarder, peer-state teardown,
   protocol issues — three genuine defects fixed along the way, none of them
   the one killing calls.
6. **The priority ladder, not the priority leap.** 5/7 → 12 broke the
   handshake; 5/7 → 11 fixed the starvation. Same-direction steps can have
   opposite outcomes; A/B your way up.
7. **Wrong turns are the curriculum.** Every disproven theory eliminated a
   whole class of causes and made the next instrument sharper.

## Artifacts

- The instrumentation: 5-second in-call ticks (audio counters, RSSI, top-3
  CPU) in `esp_openclaw_room_node.c`; named DISCO decrypt failures in
  `ml_wg_mgr.c`/`ml_coord.c`.
- The fix: `ML_TASK_{NET_IO,DERP_TX,COORD,WG_MGR}_PRIO = 11` in
  `microlink_internal.h`, with the evidence trail in the comment above it.
- The supporting fixes shipped on the journey: peer-update state preservation,
  DERP keepalive from the socket owner, DNS fallback resolvers, peer-churn
  suppression.
- Raw working journals: `memory/` (this document is the curated, sanitized
  telling; the journals contain live network details and stay local).

---

*Design Journey #1 — room-node voice calls: the 44-second death and the
priority fix. Further journeys in this series will follow as the room node
and its companions keep teaching us: each `DESIGN_JOURNEY*.md` in this
directory tells one story, start to finish, wrong turns included — because
we are not magicians, and the wrong turns are the curriculum.*