# Mini Space Shooter

A small pixel-art space shooter for Linux, written in C11 and C++17. The game uses fixed-capacity pools and its own online learners to adapt enemy movement and tactics. Raw difficulty has hard limits; later levels add tactical variation instead of unlimited speed or bullet counts.

The canonical repository is [github.com/yunusemrejr/minispaceshooter](https://github.com/yunusemrejr/minispaceshooter). We will keep that repository updated as future local changes are completed. See [CONTRIBUTING.md](CONTRIBUTING.md) for the commit, verification, and push workflow.

```bash
./run.sh
```

## Build and run

Build requirements: `build-essential`, `libx11-dev`, and `libxext-dev`.
On a Wayland desktop the game uses the optional **SDL2 runtime** (`libsdl2-2.0-0`) for native windows. SDL development headers are not required. On X11, or when native Wayland is unavailable, it uses the existing Xlib renderer. Audio loads ALSA/PulseAudio dynamically and can run silently.

```bash
./run.sh test         # 28 headless regression tests
./run.sh shots        # screenshots of every screen, no display required
./run.sh strict       # separate build with warnings treated as errors
./run.sh sanitize     # AddressSanitizer, UBSan and float-cast checks
./run.sh audio-test   # real playback, music/mute controls and audio shutdown
./run.sh window-test  # real window, input, resize and close test; no save changes
```

Both `make strict` and `make sanitize` use separate build directories, preserving the normal game binary. In environments where LeakSanitizer cannot inspect processes (including this agent sandbox), use:

```bash
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 make sanitize
```

This retains address and undefined-behavior checks; it does **not** test leaks.

## If an icon appears but no window opens

The default launcher now chooses native Wayland when `WAYLAND_DISPLAY` is set. To select a backend explicitly:

```bash
MSS_VIDEO_BACKEND=wayland ./run.sh
MSS_VIDEO_BACKEND=x11 ./run.sh
```

The reported startup failure was reproduced on GNOME/Wayland: the game, a minimal Xlib probe, and `xmessage` all remained **unmapped / Iconic**. Changing pixel transport or requesting normal window state did not resolve that desktop's X11 behavior. The native Wayland path bypasses it. No desktop settings are changed.

Useful diagnostics:

```bash
./run.sh --debug --frames 180
MSS_VIDEO_BACKEND=x11 ./run.sh --verify-present 120
MSS_NO_SHM=1 MSS_VIDEO_BACKEND=x11 ./run.sh --verify-present 120
```

`--verify-present` returns 0 for matching X11 pixels, 1 for a mismatch or an unmapped window, and 2 when pixel read-back is unavailable. An invisible X11 window is no longer described as a successful render or merely a read-back limitation. Native Wayland does not expose desktop pixel read-back through this API.

## Controls

| Key | Action |
| --- | --- |
| Arrows / WASD | Move |
| Space / Z / J | Fire |
| Esc / P | Pause |
| Enter / Space | Confirm menus |
| 1 / 2 / 3 / 4 (or numpad) | Buy Scout / Wing / Cruiser / Titan |
| M | Mute all audio |
| N | Toggle background music |
| `[` / `]` | Volume |
| F1 | Learning and difficulty overlay |
| `-` / `=` or F2 / F3 | Integer pixel scale |

Start with three hearts. Every fifth level repairs one heart, up to three. Death ends the run and resets the next run to level 1. Restarting or returning to the title from pause records the run first. Test/autoplay/frame-limited/headless modes never modify the real save.

## Your allied fleet

Every point earned from enemy kills (including allied kills) and level bonuses also adds one credit to your **run wallet**. Spending credits never lowers your score or medal progress. Press a number key once to buy a ship; holding the key does not repeatedly spend credits.

| Key | Ally | Size | Credits | Hull | Weapon | Reload |
| --- | --- | --- | ---: | ---: | --- | ---: |
| 1 | Scout | Small | 600 | 2 | One 1-damage bolt | 0.95 s |
| 2 | Wing | Medium | 1,600 | 5 | Two 1-damage bolts | 0.85 s |
| 3 | Cruiser | Big | 3,600 | 10 | Two 2-damage bolts | 0.75 s |
| 4 | Titan | Heavy | 7,200 | 18 | Three 3-damage bolts | 0.65 s |

Up to four allies can fight at once, in any combination. They aim at enemies, escort your ship, evade bullets and intercept incoming fire. Green bars show remaining hull. Enemy bullets and collisions damage allies; destroyed ships free their slot for another purchase. A full fleet or insufficient funds costs nothing. Allies do not damage you or one another, and their kills do not inflate your personal shooting accuracy. Credits and ships reset when a new run starts.

The fleet has its own shared **12-input, 12-hidden, 4-action neural policy** (208 parameters). It samples guard, attack, evade or escort roles every 0.45 seconds, with bounded steering and aiming implementing each role. Actual hits, player-threatening bullet interceptions, damage and destruction train it online. Shot rewards retain the firing ship's original observation and action, including after that ship has died. All four types share experience across runs in the same session; learning is not saved between application launches. F1 shows training updates, allied kills and protective interceptions. This is a local online learner, not a downloaded model; tests verify learning mechanics, not guaranteed strategic superiority.

## Music and sound

A quiet original instrumental loop uses soft sustained chords and sparse arpeggios. Player shots, allied shots, enemy fire, impacts, explosions and recruitment each have sound effects. Music has dedicated oscillators, so busy combat cannot steal its notes. Use **N** for music, **M** for all sound and **[ / ]** for volume. The controls screen shows the selected audio backend and volume.

Playback prefers the desktop PulseAudio/PipeWire mixer, with ALSA as a fallback. The ALSA stream-direction bug is fixed: playback is enum value 0, whereas PulseAudio uses value 1. The fallback now follows the negotiated sample rate, checks recovery errors and retains partial writes. PulseAudio writes run on a dedicated device-paced thread, keeping its blocking API out of the game loop. Fractional sample timing prevents long-term drift in the nonblocking ALSA fallback; the mixer smoothly limits overlapping effects. Neither audio backend opens a microphone.

For diagnosis, explicitly choose a backend with `MSS_AUDIO_BACKEND=pulse ./run.sh`, `MSS_AUDIO_BACKEND=alsa ./run.sh`, or `MSS_AUDIO_BACKEND=silent ./run.sh`. `MSS_DEBUG_EVENTS=1 ./run.sh --frames 120` reports submitted frames and output errors on exit. If the game reports playback but you hear nothing, check the desktop speaker mute/volume as well as the game's M key. Headless tests and screenshots do not open an audio device.

API reference: [ALSA stream direction and recovery](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html), [PulseAudio buffer units](https://www.freedesktop.org/software/pulseaudio/doxygen/structpa__buffer__attr.html).

## Endless play and fairness

| Limit | Value |
| --- | --- |
| Enemy bullet speed, including horizontal movement | 74 px/s |
| Enemy horizontal / vertical speed | 36 / 46 px/s |
| Minimum time between volleys from one enemy | 0.55 s |
| Enemies / enemy bullets | 12 / 24 |
| Minimum spawn interval | 0.85 s |
| Minimum new-shot reaction time | 0.30 s |
| Post-hit ceasefire | 1.10 s |
| Scheduled inbound corridors per arrival window | At most 4 of 5 |

The director cycles through calm, build, spike, trick and cooldown phases. All eight tricks are reachable, advance through their duration, and finish before a level transition. A separate shot timer prevents ambush reload acceleration from bypassing the volley limit. Enemies that linger for 40 seconds are recycled so retreating policies cannot permanently occupy the spawn pool.

The corridor rule concerns **straight bullet trajectories at the playfield's bottom row**. The firing gate reserves at least one corridor inside each one-second arrival window. It is not a proof that a player can reach that corridor from every position, or that moving into enemies cannot cause damage. Simulation tests check the scheduling invariant; human playtesting is still needed for balance and feel.

Scores, levels and saved counters saturate at `INT_MAX` rather than wrapping. At a saturated score, kills still advance levels; score overflow cannot cause a level-up every frame. Run time uses double precision and is capped at the save format's limit. Animation phases and learner history stay bounded. The game has no final level, but the counters are finite.

## Learning

All gameplay learners are allocation-free and written locally in `src/cpp/ml.*`:

- A two-layer MLP selects advance, strafe left/right or retreat. Rewards train the selected action with its actual features. Bullets retain their firing policy's features/action so a recycled ship cannot receive an old shot's reward. Scripted trick movement does not train invented policy decisions.
- Policy gradient uses the full softmax entropy derivative, verified against finite differences.
- The intent model trains on features from 15 simulation steps earlier and predicts from current features. Stationary movement gets a neutral label.
- The stress model uses a 120-step delayed window, actual recent damage buckets, and terminal-hit labels before a reset clears the ring.
- The eight-arm bandit ages its evidence and reward estimates, allowing it to change preference as the player changes.
- Online k-means uses bounded history and a minimum update rate. Tactics depend on the cluster's measured style, not its arbitrary numeric ID.

The intended simulation step is 1/60 second. `Game::update` rejects invalid times and subdivides bounded larger updates; the delayed label horizons are expressed in simulation steps. Learning survives restarts in the same process, but is not persisted across application launches.

## Records and saves

The save is `~/.local/share/mini-space-shooter/save.txt`, or `$XDG_DATA_HOME/mini-space-shooter/save.txt`. It retains the version 1 text format.

Bronze, silver, gold and platinum thresholds are derived from 10%, 25%, 45% and 70% of the best score, rounded to readable targets; diamond is exactly the record. Targets increase strictly for records of at least five points. Smaller records necessarily share some thresholds. A zero record awards no medals.

Only earned medals update their historical records. The history retains the latest 32 replaced records instead of stopping permanently when full. Saves validate field counts, numeric ranges, tiers and masks, discard malformed rows, and restore leaderboard order. Unknown format versions are rejected. Writes use a unique temporary file, checked flush/close, file synchronization and atomic rename. A failed write before rename preserves the prior save. Simultaneous writers each produce a complete file, but the last writer wins; scores from concurrent game instances are not merged.

`--reset-save` explicitly replaces the save with defaults. No save migration is required.

## Command-line options

| Option | Meaning |
| --- | --- |
| `--seed N` | Reproducible random seed |
| `--scale N` | Initial integer scale |
| `--level N` | Initial simulation level (normal menu runs begin at 1) |
| `--debug` | Enable the overlay |
| `--frames N` | Exit after N rendered frames |
| `--autoplay` | Scripted gameplay smoke test |
| `--headless` | No display window |
| `--shot DIR` | Render screen samples to PPM files |
| `--verify-present N` | X11 pixel read-back check after N frames |
| `--focus-self` | Request window focus for an input test |
| `--reset-save` | Reset records and exit |

## Layout and verification

- `src/c/platform_x11.c`: X11 transport and platform dispatch, timing and scaling.
- `src/c/platform_sdl.c`: optional native Wayland window/input transport via SDL2's stable Linux ABI.
- `src/c/audio.c`, `minui.*`, `font5x7.*`: audio, UI and font.
- `src/cpp/game.*`, `allies.cpp`, `director.*`, `ml.*`: simulation, fleet economy/combat, fairness and learning.
- `src/cpp/save.*`, `limits.hpp`: persistence and bounded counters.
- `tests/selftest.cpp`: 28 tests covering fleet purchases, damage, destruction, friendly shot attribution, adaptive fleet learning, music/SFX mixing and mute behavior, plus learners, drift, exact policy gradients, reward attribution, delayed labels, real shot/spawn limits, repairs, integer boundaries, save corruption, forced short writes and concurrent writers. Includes 24,000 frames of fleet combat, 108,000 frames of game fairness/soak tests and 200,000 director ticks.
- `tests/audio_smoke.c`: optional playback and audio-thread control/shutdown regression.
- `tests/window_smoke.c`: optional real-display test, including native input aliases, quick taps, resize presentation, focus loss and close events.

Validation on the development machine: strict build and all 28 tests passed; AddressSanitizer/UBSan/float-cast checks passed with leak detection disabled because the sandbox's LeakSanitizer cannot run. A real native Wayland autoplay completed all seven gameplay/purchase/learning checks, and the native window smoke test passed. The fleet HUD, all four ally types, controls, debug overlay and summary screens were visually inspected. Both native PulseAudio playback and the ALSA audio smoke test reported zero output errors; system speaker volume remains under user control. ThreadSanitizer found no races in the game audio worker when exercised against a paced test backend. The run against the installed, uninstrumented PulseAudio library reported a warning inside `libpulsecommon` during initialization, so it is not a clean full-stack ThreadSanitizer result. These checks do not establish long-session human balance, leak freedom, or compatibility with every compositor.

SDL API references: [video initialization](https://wiki.libsdl.org/SDL2/SDL_VideoInit), [window surface lifetime](https://wiki.libsdl.org/SDL2/SDL_GetWindowSurface), [event ABI](https://wiki.libsdl.org/SDL2/SDL_Event).
