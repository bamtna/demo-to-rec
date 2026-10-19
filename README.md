# demo-to-rec

Converts Counter-Strike: Source demos (client and SourceTV) to `.rec` replays (v2)

Uses [css-demo-parser](https://github.com/sockfish/css-demo-parser) for the demo parsing.

## Building

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Some tests need files that aren't in the repo and are skipped without them:
- `DEMO_TO_REC_TEST_DEMO_DIRS`: folders of demos to convert, default `../css-demo-parser/bin/Debug`
- `DEMO_TO_REC_GAME_DIR`: a `cstrike` folder, its demos are converted and replay_viewer bots are compared against the `.rec` they play
- `DEMO_TO_REC_REC_CORPUS`: folder of `.rec` files that should re-serialise to the same bytes

## Usage

```
demo-to-rec list <demo> [--player WHO] [--min-ticks N]
demo-to-rec export <demo> --player WHO (--segment N | --ticks FIRST LAST) -o <out.rec[.bz2]>
demo-to-rec compare <demo> --player WHO (--segment N | --ticks FIRST LAST) <reference.rec>
demo-to-rec dump <demo> --player WHO
demo-to-rec info <replay.rec>
```

`WHO` is a userid, steamid or name.

`list` shows each player's segments. A segment ends at a death, respawn, team change, teleport or a gap in snapshots.

```
$ demo-to-rec list tendies.dem
tendies.dem: surf_tendies, client demo, ticks 7323-10353, tick interval 0.015, 2995 packets
userid 2 "batman" [U:1:227003714] recording
userid 3 "replay_css_4074_0_712551_173040" BOT bot
  segment 0: ticks 7323-10353, 3031 ticks (45.47s), team 3, start to end
```

`export` writes the replay and a `.json` with the map, tick rate, source demo and which frames were interpolated.

```
demo-to-rec export tendies.dem --player 3 --segment 0 -o run.rec --mark 206 3 1 --mark 2956 2 99
```

- `--team 2|3`: defaults to the player's team
- `--mark FRAME TYPE ZONE`, `--mark-tick TICK TYPE ZONE`: bookmarks (type 2 enter zone, 3 leave, 1 checkpoint; zone 1 start, 99 end)
- `--target NAME`, `--classname NAME`: bookmark strings
- `--linear`: linear interpolation instead of cubic
- `--no-commands`: ignore the recording player's user commands
- `--no-duck`, `--no-jump`: don't derive IN_DUCK/IN_JUMP
- `--gravity G`: default 800

`compare` converts the same way and compares against an existing replay. `--decimate N` drops all but every Nth snapshot first, to test SourceTV-like snapshot rates.

## Library

```cmake
add_subdirectory(demo-to-rec)
target_link_libraries(your_target PRIVATE demo_to_rec::demo_to_rec)
```

```cpp
auto demo = demo_to_rec::load_demo("tendies.dem");
auto* player = demo.find_player("3");
auto segments = demo_to_rec::find_segments(*player, demo.tick_interval);
auto conversion = demo_to_rec::convert(demo, *player, segments[0].first_tick, segments[0].last_tick);
demo_to_rec::write_export("run.rec", conversion);
```

## Accuracy

The recording player of a client demo has an exact position, networked velocity, and buttons and angles from their user commands. Everyone else, and every player in a SourceTV demo, has positions rounded to 1/32 units, 11 bit eye angles, no velocity and no buttons. SourceTV also only sends a snapshot every few ticks.

Ticks without a snapshot are interpolated (cubic, using the neighbouring snapshots). Velocity that isn't networked is estimated from the position change, which has a median error under 0.01 u/s against velocity in KSF replays that I tested.

IN_DUCK is derived from the duck flags and IN_JUMP from leaving the ground, other buttons can't be recovered. A jump held in the air also can't be recovered.

User commands are matched to ticks with `m_nTickBase`. This assumes the demo was recorded on a listen server. On a remote server the buttons and angles will be early by the latency, use `--no-commands` in that case.

## Limits

- Only writes v2. v3 can be read but not written.
- A teleport to within ~16 units of where the player was going isn't detected
