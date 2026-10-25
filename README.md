# fc2-coop-probe

A research tool for **Far Cry 2** (Steam, version 1.03, Dunia engine): a `dinput8.dll` proxy
that you drop into a **test copy** of the game. Inside the running game it

- logs Winsock, file-open and exit calls of `Dunia.dll`,
- runs Lua 4.0 code on the game's main thread and gives the result back,
- runs native commands (read an entity, find the player's physics body, teleport it, scan
  memory),
- and, as a prototype, exchanges the player's position between two copies over its own UDP
  socket and moves an invincible avatar of the peer.

It is **not** a playable co-op mod: the avatar slides in an idle pose (it turns to face where
the peer looks, but has no walking animation), with no weapons, health or mission sync. It is the code behind the Far Cry 2 write-up in
[coop-anything](https://github.com/cranebeaker/coop-anything/tree/main/games/far-cry-2)
(pull request: https://github.com/cranebeaker/coop-anything/pull/43); the findings, dates and
evidence are there.

## Read this first

- Single-player campaign only. Do not use it online; PunkBuster is not touched and nothing
  here is meant to be used in other people's multiplayer games.
- **Use a copy of the game folder, never your real install.** Back up before you change
  anything. The proxy is loaded only if `dinput8.dll` sits next to `FarCry2.exe`.
- Every address (RVA) in `src/probe.c` is for this `Dunia.dll` only:
  SHA-256 `6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765`,
  20,183,176 bytes (Steam build id 52164). Other builds need new addresses.
- The game files are not included and not needed here.

## Build

32-bit MinGW (gcc 9.2 was used):

```
gcc -O1 -shared -o dinput8.dll src/probe.c src/probe.def -lws2_32 -static-libgcc
```

or run `build.bat`. Copy `dinput8.dll` into the `bin` folder of the test copy (next to
`FarCry2.exe`). On start it writes `coop_probe.log` in that folder.

## Use

The game is controlled through two files that you create in the same `bin` folder. The
probe polls them from the game's main loop about four times a second, runs them and
**deletes** them.

### `coop_cmd.lua`: Lua code

The code runs in the game's main Lua state. Put the answer into the global `__out` (a string);
it is logged as `cmd executed (N bytes) status=S out(L)=...`. `print` and `_ALERT` show
nothing in this build. Examples:

```lua
__out = "alive " .. (1 + 2)
__out = GetLocalPlayerId()
__out = tostring(IsEntityValid(GetLocalPlayerId()))
s = "" foreach(globals(), function(k, v) s = s .. k .. " " end) __out = s
```

Lua is 4.0: use `foreach`, `getn`, `..`; there is no `#`. Status 0 is success, 1 a run error,
3 a syntax error. Entity ids are 64-bit and come as decimal strings.

### `coop_native.txt`: native commands, one per line

| Command | What it does |
|---|---|
| `pos <id>` | Log position and direction of the entity (matrix at `entity+0x30`) |
| `setpos <id> <x> <y> <z>` | Write the entity matrix position once (does not stick; the body overrides it) |
| `ent <id> <len>` | Log the entity object address, vtable and `len` bytes as hex, with a float column |
| `peek <hexaddr> <len>` | Hex dump of memory (float column) |
| `poke <hexaddr> <hex32>` / `pokef <hexaddr> <float>` | Write a 32-bit value / a float |
| `scan <x> <y>` | Scan memory for the float pair (tolerance 0.01; y within 16 bytes after x) |
| `findbody <id>` | Find the Havok body (class vtable RVA `0xE67658`) nearest to the entity and cache it |
| `posb` / `tpb <x> <y> <z>` | Read / write the cached body's position (teleport) |
| `walk <radius> <seconds> <speed>` | Move the cached (local) body on a circle every frame (needs `netstart` running) |
| `hold <id> <x> <y> <z>` / `unhold` | Write the entity matrix position every tick (does not stick; kept as the negative test) |
| `follow <id> <hex offsets>` / `tp <id> <x> <y> <z> <hex offsets>` | Follow a pointer chain from the entity; write the position at its end |
| `bodyvel` / `avvel <x> <y> <z> <hexoffset>` | Write three floats into the cached / avatar body at an offset |
| `netstart <localport> <peerip> <peerport> <id>` | Open the UDP transport and start the avatar logic |
| `netstop` | Stop it and remove the avatar |

## Two copies on one PC

Each test copy needs three same-length byte patches in its own `Dunia.dll` (the copy of the
file in that test folder, never the original; keep a backup). Offsets for the build above:

1. **Mutex name.** The string `FarCry2Instance` (file offset `0xE08950`): change one
   character (for example `FarCry2InstancA` / `FarCry2InstancB`) so both copies can run.
2. **Profile folder.** The 10-byte string `\Far Cry 2` (file offset `0xE0E094`, next to
   `\GamerProfile.xml` and `\My Games`): replace it with another 10-byte string (for example
   `\FC2test_A` / `\FC2test_B`). The profile and the save that is opened then come from
   `Documents\My Games\FC2test_A\`. Give each copy that folder with a copy of your
   `GamerProfile.xml` (set `Fullscreen="0"` and a small resolution) and, to load a save, a
   `Saved Games` folder with a copy of one `.sav`. Without a profile file the client quits.
3. **Save list folder.** The 19-byte string `My Games\Far Cry 2\` (file offset `0xE089A0`):
   replace it with the same folder, e.g. `My Games\FC2test_A\`. The list of saves behind
   Continue / Load Game is built from this folder. Without this patch a copy lists the saves in
   your real profile folder, and on a PC where the game never ran it lists none (only
   New Story). This patch alone does not move the profile: patch 2 does that (check the
   `CreateFileW ... GamerProfile.xml` and `... .sav` lines in `coop_probe.log`).

Expect 17 changed bytes in total compared with the original `Dunia.dll`.

Then start each copy by hand, press Continue, and wait until you are in the world.

Then, in each `bin` folder, create `coop_native.txt` (the entity id of the main character
is the same in both copies when they load the same save; get it with `GetLocalPlayerId()`):

```
netstart 9101 127.0.0.1 9102 <id>     (copy A)
netstart 9102 127.0.0.1 9101 <id>     (copy B)
```

The log shows `net: tx=... rx=...` once a second. After the first packet each copy spawns
an avatar of the peer (`SpawnEntityFromArchetype` with the local player's own name),
makes it invincible (`CFCXCountersComponent_SetInvincible`) and drives its body.

## Automated two-copy test

`tests/two_copy_walk.ps1` runs the two-copy setup above without a human: it starts both test
copies, presses Enter until each has loaded the save from its own profile, runs `netstart` on
both, walks copy A's player on a circle with `walk`, and checks from the logs and `pos`
readings that copy B's avatar follows A while B's own player stays put. Then it turns A's
player three times with relative mouse movement and checks that B's avatar faces the same
way (within 15 degrees). A kill switch stops
both copies if a log line names the real profile folder, and the real profile folder is
compared with a snapshot afterwards. Both copies must already be patched and have their
profiles; the script does not patch anything.

```
powershell -ExecutionPolicy Bypass -File tests\two_copy_walk.ps1 -CopyA <test copy A> -CopyB <test copy B> -OutDir <results folder>
```

It presses Enter and moves the mouse in the game windows, so leave keyboard and mouse alone for
the ~2.5 minutes it runs. Exit code 0 = pass, 1 = a check failed, 2 = setup problem; `run.txt`,
`results.json`, `positions_b.csv` and both logs go to `<results folder>\<timestamp>\`. The check
`B player stays put` is the one that catches the avatar-binding bug (with the unfixed probe
B's own player was dragged 16 m); `avatar heading follows A` fails on a probe without the
heading code (the avatar stays at 0 degrees).

## How it works

- `DllMain` loads the real `dinput8.dll` and a watcher thread patches the import table
  of `Dunia.dll` (`bind`, `connect`, `sendto`, `recvfrom`, `WSASendTo`, `WSARecvFrom`,
  `getaddrinfo`, `gethostbyname`, `CreateFileA/W`, `PeekMessageA/W`, `PostQuitMessage`).
- An inline detour on `lua_gettop` captures the Lua state; the Lua entry points are
  `lua_dobuffer`, `lua_getglobal`, `lua_settop` and `luaL_check_lstr` at fixed RVAs.
- `PeekMessage` runs on the main thread when no script runs, so commands, the per-frame body
  writes and the transport run there.
- The physics body is found by scanning memory in 64 KiB chunks with `ReadProcessMemory`
  (a plain read can fault when another thread frees a page) for objects of one class and
  taking the one whose motion transform is nearest to the entity. Heap addresses change every
  session; the class does not.
- Heading: the body is an upright capsule whose rotation is always identity; where a pawn faces
  is the yaw in the entity matrix (`entity+0x30`, rows `(d0,d1,0)`, `(-d1,d0,0)`, `(0,0,1)`). For
  the local player the controller rewrites it every frame from mouse look; the avatar has no
  controller, so the rotation the probe writes there each tick stays (smoothed 25 % per tick).
  Writing the body's rotation, or calling `TeleportEntity` with angles, changes nothing;
  `SpawnEntityFromArchetype` only uses its last argument (yaw in degrees) at spawn.
- An exception filter and a vectored handler log access violations. Many at start-up come
  from system libraries and are harmless.

## Known limits

The same as in the write-up: no walking animation for the avatar, no weapons, health or
mission state, loopback ports only, exact build only, and positions written into a body bypass
collision (the avatar passes through scenery; it mirrors the peer's already-collided position).

## License

MIT, see [LICENSE](LICENSE).
