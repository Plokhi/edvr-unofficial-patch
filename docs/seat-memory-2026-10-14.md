# Seat memory: the cockpit centre that nobody presses (2026-10-14)

The investigation is `reviews\boarding-recentre-2026-10-14.md` (gitignored; the evidence tables and the log reading are there). This doc is
the arc's status, the log lines the build writes, and the flight checklist.

## Status

- State: BUILT on `claude/seat-memory` (off `claude/flight-2026-10-14`), NOT FLOWN, not merged. Instruments and behaviour ship together;
  `advanced.cockpit_centre = game` is today's behaviour with the instruments still logging, so one install gives both halves of the A/B.
- **Temporary flight key** (AGENTS.md: list it, propose removing it when the arc closes):
  `advanced.cockpit_centre = keep | game`, default `keep`, VR profile only, live. Propose removing it, quoted, once the flight settles
  which behaviour wins: the end state is `keep` as the only behaviour and no key. Not needed in the flat allow-list
  (`src/common/flat_profile_keys.h`): the flat profile publishes no seat word, so the runtime reads game mode.
- Open hypotheses (reviews\boarding-recentre-2026-10-14.md section 1): **H1** the game's reset lands while the head is standing or turned
  (seen 4.4 s before to 4.2 s after the journal Embark); **H2** most embarks (34 of 46) have no reset at all, so the origin left from the
  on-foot spawn reset is used in the cockpit; **H3** an internal game recentre with no runtime call. Discriminator: `seat_meter` at
  `transition_2s` after a boarding in `game` mode (offset_cm, height_cm, yaw_deg far from 0 with or without a `seat_reset` near the flip).
- Assumptions the first flight tests: (a) the first tracked pose at launch may be a headset on its stand, not a worn head (position repeated
  to 2 cm over 92 launches), so the seat is captured at the splash only after the head has been still for a window, and the capture's
  statistics are logged either way; (b) the 10 s transition window; (c) that seated zero is the cockpit's eye point, so a normally seated
  head reads about 0/0/0 after a good alignment; (d) Status.json flags arrive about a second late.
- Known risk: a game reset aboard that is neither a player's recentre nor near a journal boundary (the 2026-10-14 logs have one 25 s after
  an embark, docked) is classified as the player's own and STORED as the new seat. It is taken from a seated head in practice, but the
  flight should look for `decision=honour_store` lines with `window=0` that no one pressed.
- Ruled out (review): STAGE as a seated centre (floor origin at the play-area centre; Sean's chair is 1.1 m and 28 degrees from LOCAL's
  origin on both runtimes); yaw-only or position-only handling as the primary fix; reading Elite's cockpit eye point as a prerequisite.
- Next flight: the checklist at the end of this doc.

## What the build does

State kept: the seat, a position and a heading in the runtime's natural LOCAL space, per headset key (`headset_key`, runtime plus system),
in `%LOCALAPPDATA%\EDVR\<leaf>-<store>\seat.txt` (`src\openxr\seat_store.h`; format `src\openxr\seat_memory.h`).

- Start (`centreAtStartup`): stored seat present and mode keep: the seat is the origin (the same tracked-pose wait as before; no head sample
  chooses the origin). Otherwise today's centre on the head.
- Splash (`requestIntroRecentre`): seat stored and already the origin: no recentre. Seat stored but not applied: applied. None stored: sample
  the head until it has been still for 0.4 s (30+ samples, 2 cm and 4 degrees), at most 4 s; then the seat is that mean, or LOCAL's own
  origin when the head is within 25 cm and 20 degrees of it; saved. Not still in 4 s: today's recentre on the head as it is.
- Boarding, a change of cockpit (Status.json seat kind: ship, fighter, SRV, taxi, multicrew), Explorer Cam closing: the seat becomes the
  origin if it is not already (a 10 s window opens at each).
- A game ResetSeatedZeroPose: game mode or unpublished: as before. On foot or in a menu: as before, nothing remembered. Aboard inside a
  window: the stored seat replaces the head pose (nothing stored: as before). Aboard outside a window: the player's own recentre,
  honoured and stored.
- The runtime recentres LOCAL (SteamVR dashboard, Pimax Play): the stored seat is carried into the new LOCAL with the event's
  `poseInPreviousSpace`, or forgotten when the event carries none, and put back when aboard.
- The word that crosses from d3d11.dll to openvr_api.dll is one 32-bit value (`src\common\seat_context.h`, `publishSeatContext` in
  frame_flag.h; the slot was padding, `reservedPoseReader0`, so the mapping version is unchanged). Status.json's seat kind is new in the
  journal watcher (`journalSeatKind`).

## Log lines (the runtime's `edvr_openxr_*.log`; none of them appear in a build without this code)

- `seat_memory,init=1,mode=keep|game|unpublished,key=,stored=,seat=x/y/z/yawdeg,file=,file_exists=,read_ok=,records=,skipped_lines=`
  once, at the first use. `mode=unpublished` means the graphics half has not written the word (treated as game).
- `seat_context,published=,mode=,gameplay=,seated=known/val,on_foot=known/val,camera=,seat_kind=0x,transitions=,where=aboard|on_foot|unknown`
  at every change of the word (at most 300).
- `seat_event,why=boarded|seated|seated_at_start|cockpit_changed|camera_opened|camera_closed|journal_event|left_seat|unknown_state,window_opened=,apply_wanted=,no_seat=,where=,...`
- `seat_reset,n=,reason=seated_reset|launch_centre|intro_recentre|seat_transition,decision=,mode=,where=,window=,window_age_ms=,stored=,origin_was_seat=,caller_thread=,caller_rva=0x,head_local=x/y/z/yawdeg,from=,to=,moved_cm=,moved_deg=,after_offset_cm=,after_height_cm=,after_yaw_deg=,result=,ok=`
  for EVERY seated-origin change, game or ours. `decision`: `game_mode`, `pass_through`, `replace_with_seat`, `honour_no_seat`,
  `honour_store` (game calls); `start_seat`, `start_head`, `intro_head`, `intro_seat`, `capture_head`, `capture_snap_local`,
  `transition_seat` (ours). `caller_rva` is the game's return address for a game call (0 for ours).
- `seat_start,plan=todays_centre|apply_seat|capture_late,mode=,stored=` once, before the start's reset.
- `seat_intro,plan=skip_seat_in_place|apply_seat`; `seat_capture,state=begin|done|gave_up,...` (done: `n,span_ms,spread_cm,yaw_spread_deg,mean,dist_from_local_cm,heading_from_local_deg,snapped,seat`).
- `seat_meter,when=transition_2s|periodic,why=,where=,mode=,stored=,origin_was_seat=,offset_cm=,height_cm=,yaw_deg=,head_local=,origin=`
  (`head=untracked` when the headset is not tracked). 2 s after each window opens aboard and after each transition apply; every 30 s.
- `seat_store,action=load|save|forget,...`; `seat_local_changed,pose=valid|absent|overflow,...,action=compensate|forget_seat`.
- `reference_spaces,count=,types=LOCAL|VIEW|STAGE...,stage=`, `stage_bounds,result=,width_m=,height_m=`,
  `stage_in_local,result=0,position=x/y/z,heading_deg=` once at the first poll: answers whether STAGE exists and where the play-space
  centre sits against LOCAL and the seat.

## Flight checklist

Read each log with `python tools\edvr_log.py --target <target> --expect-build HEAD --version` first (right build), then grep `seat_`.
Pimax OpenXR first; SteamVR/OpenXR as a second launch if time allows (the key differs, so each has its own seat).

0. Install, check the build line. The first `seat_` line must be `seat_memory,init=1`; if there is none the code did not run.
1. Launch 1, `game`: `mode=game` in `seat_memory`, `seat_start,plan=todays_centre`, `seat_reset ... decision=intro_head`; baseline for steps 4 to 6.
2. Launch 2, `keep`, no seat file: `seat_start,plan=capture_late`, `seat_intro`/`seat_capture,state=begin` then `done`. Pass: `done` with
   `spread_cm` under 2 and the splash in front at eye height; `seat_store,action=save,ok=1`. Record `dist_from_local_cm` and `snapped`.
   Fail: `gave_up` (the stats name why), or the splash is off.
3. Launch 3, `keep`, seat stored: `seat_memory,init=1,...,stored=1`, `seat_start,plan=apply_seat`, `seat_reset ... decision=start_seat`,
   no `intro_head` line, `seat_intro,plan=skip_seat_in_place`; first `seat_meter` within 5 cm / 5 degrees when seated.
4. Board from foot by the ramp, looking sideways; then by the interface: `seat_event,why=boarded,apply_wanted=1`, `seat_reset ... decision=transition_seat`,
   any game reset in the window `decision=replace_with_seat`; `seat_meter,when=transition_2s` within 5 cm and 5 degrees (offset_cm, yaw_deg),
   height_cm within 5. Compare `game` mode's meter at the same step (H1 or H2: reset near the flip or not).
5. Ship change in the hangar and back in: `seat_event,why=boarded` or `seated`; same meter.
6. SRV launch and return if a planet fits (`why=cockpit_changed`; the `seat_kind` in `seat_context` goes 0x1 to 0x4 and back); if not flown, say so.
7. Explorer Cam in and out, on foot and aboard: aboard, `camera_opened` then `camera_closed`, meter unchanged; no `seat_reset` that moves the origin off the seat.
8. Manual: press the game's recentre aboard well after a window: `decision=honour_store`, `seat_store,action=save,why=manual_reset`; the next boarding uses it.
   Runtime recentre once: `seat_local_changed,...,action=compensate` (or `forget_seat` with `pose=absent`).
9. Chair moved 30 cm, relaunch: the meter reads the move; one manual recentre fixes it and is remembered.
Pass overall: every boarding, ship change, SRV and Explorer Cam return inside 5 cm and 5 degrees at `transition_2s` without a press, and no
`seat_reset` with `ok=0`. Also read `reference_spaces` and `stage_in_local` once and write the answer into this Status block.
