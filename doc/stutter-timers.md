# Buffered stutter timers

The timers are compiled in but **recording is off by default**. They work in
Release without VTune. No timer event writes to the game log or disk while
recording. A bounded 16384-event memory buffer uses a non-blocking try-lock:
contention/full-buffer events are dropped and counted rather than delaying a
game/worker thread. Capture is opt-in and still has measurement overhead.

## Quick capture

In the game console, before the measured route:

```text
stutter_profile start
```

After the route, outside the measured portion:

```text
stutter_profile dump
```

`dump` stops recording and writes `stutter_profile_<SessionUtcNs>.csv` under the
engine's `$logs$` directory. The game log prints its full path and the dropped
event count. Opening the console/dumping can stall; exclude those boundaries
from PresentMon. No automatic disk writes or automatic capture through user.ltx
are performed. The capture is lost on process exit unless explicitly dumped.

Other commands:

```text
stutter_profile stop
stutter_profile status
stutter_profile clear
stutter_profile mask 15
stutter_profile threshold relcase 2
stutter_profile threshold spawn 1
```

`1`/`on` alias `start`; `0`/`off` alias `stop`. `start` starts a fresh session and
discards the previous buffer. `clear` works only while stopped. Scopes crossing
stop/reset boundaries are omitted, so a new capture cannot receive old events.

## Categories, masks and default thresholds

| Category | Mask bit | Threshold, ms |
|---|---:|---:|
| spawn | 1 | 1 |
| sound | 2 | 1 |
| relcase | 4 | 2 |
| wait | 8 | 2 |
| phase | 16 | 5 |
| frame | 32 | 50 |
| task | 64 | 2 |

Default mask **63** includes everything except per-task execution. Mask **15**
selects just spawn/sound/relcase/wait. Mask **127** also measures task execution,
to correlate a long Wait with the task's execution interval and worker ID.
Change thresholds with `threshold CATEGORY MS`; zero records every completed
scope of that category and can fill the buffer quickly. Masks/thresholds are
not persisted by cfg_save; configure them before each capture as needed.

## Instrumented work

- `cl_Process_Spawn`, full `g_sv_Spawn`, `Objects.Create/Load`, the full virtual
  `IGameObject::net_Spawn` call, base `CGameObject::net_Spawn`, script `net_Spawn`.
- `CSoundRender_Source::LoadWave` with the sound filename.
- `net_Relcase.receiver_batch`: all destroyed-object notifications delivered to
  one receiver. Subject is receiver ID; Related is the number of calls in the
  batch; Detail is its section. Default threshold is **2 ms**. It deliberately
  avoids a timer for every receiver/destroyed-object pair.
- `net_Relcase.all_receivers` catches the total receiver phase even when every
  individual batch is below threshold. Sound/HUD/callback cleanup is not part
  of this particular aggregate.
- `TaskManager::Wait`: Subject is the task address, Worker is the scheduler
  worker ID (Main = 0). It measures wall duration, not CPU time or OS wait state.
- Optional `TaskManager::ExecuteTask`: same task-address convention. Addresses
  can be reused; correlate them by time as well, not by address alone.
- FrameMove, ProcessGameEvents, DoRender, Calculate, Render, render_lights and
  ProcessFrame provide enclosing phases. ProcessFrame includes its frame limiter.

The old synchronous SoundProfileLite LoadWave timer is replaced by the buffered
timer. The allocating DEBUG net_Relcase statistics/logger are disabled by default
behind the separate legacy build flag described below.

## CSV interpretation

Durations/StartUs/EndUs are **microseconds**, from a monotonic clock. StartUs and
EndUs are relative to this capture's anchor. SessionUtcNs is the UTC system-clock
anchor in nanoseconds since the Unix epoch, read once at capture start. Clock
conversion to a PresentMon date-time capture must still be checked explicitly.

EventId and ParentId describe nesting **on the same thread**. A parent can be
missing because its category was disabled or its duration was below threshold.
Do not sum overlapping parent/child durations. Task addresses connect Wait/Task
events across threads; no parent-child relationship across threads is invented.
FrameBegin/FrameEnd are game Device frame numbers, not PresentMon row numbers or
the previous VTune ITT instance numbers. Worker is blank when unknown; TID is the
native Windows thread ID on Windows. Name is bounded to 63 bytes, Detail to 95
bytes. Details are snapshotted before callbacks can invalidate them.

## Full compile-time removal

Visual Studio/MSBuild (rebuild affected modules consistently):

```text
/p:XrayStutterTimers=0
```

With this option off, timer macros evaluate no arguments, create no scope
objects and call no clock. To restore the separate old DEBUG relcase logger:
MSBuild `/p:XrayLegacyRelcaseProfile=1`. It has its own overhead and is independent of
the new runtime capture switch.
