# NFS Carbon → native Apple Silicon (static recompilation)

Port of Need for Speed: Carbon (PC v1.4, unwrapped NFSC.exe) to macOS using
veritr1x's recomp-kit, taken from the NFS Most Wanted Mac port
(github.com/elforeign/nfs-most-wanted-mac; no local clone any more). Our kit = MW 1.0.3's kit. MW 2.0-3.0.4
(branch legacy-3.x, 2026-10-10..16) kept changing that kit; MW 4.x (2026-10-27+) is a NEW MW-only runtime, not the
kit (ideas only: video clips, Update now, GitHub problem reports).
The kit's copy lives in `kit/` with our changes on top. Developed on macOS 27, Apple Silicon.
Explain findings plainly and keep commands copy-pasteable.

## Working rules
- Get evidence before patching: read the listing (`analysis/decompiled/<image>/functions/<addr>.asm`),
  the bytes (`xxd`), or the log line first. Guessing cost several rounds on `_initterm`.
- Kit changes go in `kit/` directly; commit them in git with a message saying why.
- Never commit game files or anything derived from them (original/, rebased/, analysis/,
  build/, dist/ are ignored).
- Rebuild: `source .venv/bin/activate && python tools/build.py` (add `--regenerate`
  only when `kit/tools/recomp/translate.py` or `game.toml` changed).
- Never run the build or game from inside `original/`'s parent via `..`:
  `original/` is a symlink to your game folder.

## Layout
- `game.toml`: exe identity (sha256 baaaaec3…fd3, base 0x400000, entry 0x87e926),
  1 GB arena, `[modules.aux.a_msvcr71]` / `[modules.aux.b_msvcp71]`, entry points,
  data_ranges. Sentinel hooks point into padding 0x00c2b400-0x00c2bfff.
- `rebased/`: msvcr71.dll → 0x10000000, msvcp71.dll → 0x10060000 (tools/rebase_pe.py;
  the kit has no relocation support and the originals want 0x7c34/0x7c3a0000).
- `tools/`: analyze.py (Ghidra over exe + both DLLs), fetch_tools.py (Ghidra 12.1.3 +
  JDK 26 into ~/Library/NFSC-Native-Setup), rebase_pe.py, who_names.py (who names an
  address), inspect_exe.py, build.py/setup.py wrappers.
- `docs/kit-patches/carbon-kit*.patch`: history of kit changes 1-15, all already applied to
  `kit/`. Since 2026-10-10 the project is a git repo: kit changes are commits, no new patch files.

## Packaging (tools/setup_kit/)
- `package.py --game DIR [--install]`: build/CarbonRecomp.app + your game folder ->
  `dist/Need for Speed Carbon.app` (and /Applications with --install). Checks NFSC.exe's
  SHA-256, copies the game with APFS clones minus game.toml [bundle] exclude + setup.exe
  etc., puts the rebased msvcr71/msvcp71 beside NFSC.exe, Info.plist: name, icon,
  public.app-category.racing-games + LSSupportsGameMode (Game Mode), ad-hoc signs.
- `launch.c` -> Contents/MacOS/Launch (CFBundleExecutable): profile
  ~/Library/Application Support/NFS Carbon (saves in Documents/NFS Carbon/<profile>/,
  registry.json, mod-settings.json seeded from Resources/Defaults on first run),
  Logs/<date>.log + Logs/latest.log (10 kept), RECOMP_EXE=Resources/Game/NFSC.exe,
  RECOMP_LOG=1; any RECOMP_* already set wins. Then execs CarbonRecomp.
- `defaults/`: registry.json (working video settings), mod-settings.json (perf overlay
  off, host.display/window 2 = full screen; 0 windowed, 1 borderless), fallback
  AppIcon.icns from `make_icon.py` (original art, no EA logos).
- Icon: package.py uses `--icon PICTURE` if given, else the game's own NFS Carbon logo (NFS07_LOGO,
  512x128 A8R8G8B8 in the player's GLOBAL/UniqueBootTextures.bin, read by tools/setup_kit/game_art.py,
  a small TPK reader/DXT decoder) on a carbon tile (make_icon.icns_from_logo), else NFSC.exe's 32x32
  icon, else the fallback. No EA art is stored in the repo. (GLOBALB's PC_LOAD is leftover NFSU2 art.)
- Launcher sets RECOMP_D3D9_SHADER_WARM_DIR=<profile>/ShaderWarm. First launch of the
  installed app (cold Metal cache, 0.66 s of shader compiles at the movies) went black
  after the EA splash; later launches fine. Warm-up helps every launch after the first;
  the root cause of the black (why a slow first frame stalls the game) is NOT found.
- Run from /Applications, not dist/: inside the checkout the runtime sees game.toml
  above it and behaves as a developer build.

## Mods (mods/core/nfsc, game.toml)
- `mods/CMakeLists.txt` builds every mods/core/<id>/ plugin (kit build_core.py) into the
  app's Resources/mods/core. `NOTICE` credits ThirteenAG (MIT) for the widescreen port.
- core.nfsc widescreen stage 1 (2026-10-10, built + packaged, not yet run): replaces
  00712ac0 (racing resolution -> screen shape, <=1080 rows); BEFORE hook on 0071b780
  (projection; view id at [[esp+4]+8]) fills slots 0x00c2b400..0x00c2b410 that 5
  operand_redirects in game.toml point at (aspect int num/den at 0071b858/0071b864,
  1.0 at 0071b8da, 0.5 at 0071b8ec, 1.0 at 0071b923), values from ThirteenAG's
  Rendering.ixx FOVHook (views 1/4 Hor+, mirror 3 = 0.925). 00712ba0 is dead code.
  Settings: widescreen (bool), fov_scaling 0/1/2.
  Fixed since: mods need POP_MOD_DECLARE_ABI(); screen size is read at device creation
  (kit change 13, desktop host never published it to mods); game size = screen / k
  (k = round(rows/1080)) because host_present_fit scales by whole numbers only:
  1668x1080 left a border, 1710x1107 fills 3420x2214. Bars confirmed gone 06:2x.
- Widescreen stage 2, HUD (confirmed: fills the screen, HUD scaled; 2026-10-10): mod writes HUD_SCALE_X
  009e8f8c, world-map X 00a604ac, FE X 009c778c, race-route X 009d5f3c (runtime reads,
  checked in gen), slot 0xc2b418 = HUD centre for 6 MOV 320.0 instruction_patches,
  slot 0xc2b414 = 240*aspect-320 for SetWideScreenMode's 120.0 (2 redirects); widescreen
  HUD forced on (SETZ 005dc507/005d52b2, JNZ 005b6bae/005b6b5b); BEFORE/AFTER hooks on
  SetWideScreenMode 005d5280 move groups 0x1603009E/0x5D0101F1 via FindObject 005a0250 /
  GetCenter 00597900 / SetCenter 00597a70. Mirror/PiP (CONFIRMED by the user 2026-10-10): quad
  builder 00750b30 (cdecl quad*, x, y, w, h; 4 verts 0x18 apart) is called by 00750c30 (rear
  mirror) and 00750d40 (PiP); BEFORE/AFTER hooks narrow the 4 x's by (4/3)/aspect (the fix's
  MirrorFix at 00750c23); instruction_patches PUSH 0.28 at 00750de6 and PUSH [0xc2b420] at
  00750df4 (slot = ((320+offset)/320-1.042)/-2, fPiPUCAPPosition). FE scaling (FEScaleHook1/2 at
  00730e4d/00730ece), loading-screen pillarboxes, rain droplets, autosculpt/arrest blur,
  options resolution list. `tools/find_pattern.py` finds the fix's byte patterns.
- Rumble: mods/core/nfsc/rumble.h (from MW's impact_feedback) hooks the game's FFB functions (impacts 00734cd0,
  road surface 00735270) and drives the pad motors; settings impact_rumble / surface_rumble / rumble_floor.
  So MW 3.0.4's DirectInput force feedback (dinput_effect.cpp, RECOMP_DINPUT_FF) was NOT ported: it would
  make Carbon create the same effects a second time (it already calls SetProperty 9 / SendForceFeedbackCommand).
- Background pause/mute: the game already does it (user-confirmed 2026-10-10: music
  pauses on Cmd-Tab) via WM_ACTIVATE/WM_ACTIVATEAPP the host posts (WndProc 00712060:
  007120ab -> 0050c370; 007121d5 -> 0081e6a0). A host-side pause was tried and reverted.
- Controller / XtendedInput (STARTED 2026-10-10). User has a Switch Pro pad; it already
  works through the kit's DInput joystick, but with Carbon's generic PC layout (Y gas,
  A reverse, B brake, X speedbreaker, R2 pause, L2 nitro, L1 look back, right stick
  nothing). Goal: the Xbox 360 layout XtendedInput (xan1242/Berkay Yigit, MIT;
  github.com/xan1242/NFS-XtendedInput) defaults to: RT gas, LT brake, LS steer, A e-brake,
  B nitro, X speedbreaker, Y crew, LB look back, RB camera, RS Y shift, Start pause, Back
  reset; plus console-style FE prompts. Plan (MW port's way, mods/core/nfsmw/pad_mapping.h
  + frontend_pad.h): write the console layout as the binding DEFAULTS in the game's own
  table (keep Controls-screen remaps), triggers as analog pedals, console FE objects shown
  while the pad is in use. Button icons: XtendedInput's XtendedInputButtons.tpk is Aero_'s
  art with no stated licence -> do NOT ship; Setup app downloads Release-Carbon-Pack.zip
  from xan1242's GitHub release (opt-in checkbox) and copies the TPK into the user's app;
  fallback: our own drawn glyphs. XtendedInput's Carbon addresses (src/Main.hpp +
  Definitions.h, #ifdef GAME_CARBON, match v1.4): INPUTMAPPING_CONSTRUCTOR_CALL 0x00696A74
  (in 006969a0: per-port mapping object, data from 006964a0), INPUTDEVICE_FACTORY_INIT
  0x9B629D, DEADZONE ptr 0x696071, FE deadzone ptr 0x59FF6E, FENG_HIDEPCOBJ 0x583D70->
  0x583D99 (vt 0x9D0680), CFENG_RENDEROBJ_NOP 0x5B092D/flag 0x5B0929, FENG_FINDOBJECT
  0x5A0250, FENG_SETOBJECTVISIBILITY 0x570460, CFENG_INSTANCE 0xA97A78, PRESS_START hook
  0x8577B6, DINPUT_KILL 0x6892EB, OnLoadDone 0x58E250 (profile loaded).
  STAGE 1 CONFIRMED by the user 2026-10-10 (Switch Pro: layout, analog triggers, names, remaps) (mods/core/nfsc/pad.h): bindings = 72 rows
  at 0xa98700 {name, triples*} (template 0xa61c88, 52-byte rows; names GAME_ACTION_GAS...);
  triples 0/1 keyboard, 2p+2/2p+3 port p {kind, port, control}. 00689550 (thiscall port, RET 4)
  = per-device defaults for rows 0..46 (callers: 00694c50 device create, 005c8960 Controls
  Restore Defaults, 004b01f0 reset). REPLACE hook remembers the game's generic rows then
  writes the console layout; per frame (REPLACE 00692240, values float[72] at [this+0x20])
  a port whose rows == generic gets the layout (old saves, Restore Defaults); remaps kept.
  Triggers (kind 7 control 0/1 = Rx/Ry) get full travel after the update (state +0x20c/+0x210,
  range 0..65535). Names: REPLACE 00679ad0 (cdecl out, device, row, primary, port), family
  from setting controller_buttons or SDL_GetGamepadTypeForID via dlsym (SDL exported by host).
  Settings: console_layout, controller_buttons.
  STAGE 2 console menu prompts (fe_pad.h) CONFIRMED working by the user 2026-10-10 (needed
  00583d70 in entry_points: hooks need a listed function). Career map overlap fix (groups
  0x17b1f254 console / 0xa2601d59 PC via FindObject on [0xa977f0]+0xc, only while the map or
  FeWorldMapQuickList.fng is in control (00572350); quick list focus hooks 005cdbf0/005c3880;
  "Free Roam" -> Back in the FE) CONFIRMED 2026-10-10. Details:
  REPLACE 00583d70 (load-time hide of console objects, arg obj, RET 4), REPLACE 005b0920
  (render object; its TEST 0x40/JNZ at 005b092d NOP'd in game.toml), BEFORE 00600c90
  cFEng::Service walks menus ([this+0xe0] next +4; objects +0x4c next +4; group children +0x64)
  hiding PC (0x1008) or console (0x40) set, mark in UserParam +0x28 bit 0x01000000. Console
  set after pad input, PC after key/click. Icons: console images use PS2 names (CROSS...) not
  in the PC data -> retargeted to XBONE_/PS4_/SWITCH_ names in GLOBAL/XtendedInputButtons.tpk,
  loaded via 006b32c0/006b5910/006b68d0. No pack -> keyboard prompts stay. package.py
  --button-icons downloads XtendedInput 1.22 Release-Carbon-Pack.zip (sha256 pinned, cached in
  ~/Library/Caches/NFS Carbon Setup). Setting console_prompts. Offline fallback (our own TPK) TODO.
  Icons swapped only in controller mode (shared images, e.g. map filter bar, keep the PC
  image otherwise). Menu cursor: redirect 00731665 FMUL 640.0 -> slot 0xc2b41c = 2*centre
  (480*aspect); fixed the right-side wall (CONFIRMED 2026-10-10).
  F10 page: game.toml [settings] page = "tabs" (choice names; Controls tab), touch rows removed.
- Right-stick camera (mods/core/nfsc/camera.h) CONFIRMED by the user 2026-10-23 (Switch Pro). PS/Xbox pads:
  no code needed (the kit reads every pad through SDL_Gamepad: same right-stick axes, buttons by position),
  untested by the user (no such pads). User chose full 360 deg
  (stick direction = look direction, down = behind) with shifting moved off the stick; release eases back
  (tau 0.15 s; follow tau 0.07 s, dead zone 0.30). Callsite BEFORE hook (slot 24) on eCreateLookAtMatrix
  0071b430 (cdecl out, eye, target, up; Z up) from CubicCameraMover::Update 004921b0 (call 00492e5b, ret
  00492e60; mover in ESI, dt at [ebp+8]); view type [mover+0xac] (0 bumper, 1 hood, 2..4 chase; 00486500
  maps 1:1), look-back flag [mover+0xb1]. Chase: eye orbits the target; bumper/hood: target turns around
  the eye; the up vector turns too. Stick = device state +0x8 (lZ) / +0x214 (lRz), 0..65535 (the
  game's data format: X Y Z at +0/+4/+8, Rx Ry Rz at +0x20c..; +0x208 is a button, read there first by
  mistake -> camera stuck sideways, x = -1, 2026-10-23); an axis a
  driving row (0..28 but 23) is bound to is left to it. Layout now: RB shift up, LB shift down, RS click
  look back, d-pad down camera change, LS click next song; pad_move_old_layout moves profiles saved with
  the v0.5 layout (rows 7/8/24/25 primary exactly old). Setting right_stick_camera (controls, live).
  One-time log lines "right-stick camera running" / "right stick first pushed". Idea from ThirteenAG's
  Camera.ixx (free look, rate-based; not its feel). If left/right feel swapped: flip the sign of `want`.
- Extra Options (mods/core/nfsc/extras.h, 2026-10-30; CONFIRMED by the user 2026-10-30, all options work): the user's picks from the
  ExOpts Team's NFSC Extra Options (v3.0.1 source, GPL-3, read for addresses only; ~/tmp clone, NOTICE).
  All live F10 settings, new Mods-tab groups unlocks/races/driving/world/cheats (kit kModGroups). Data
  writes: unlock-all 0xa9e6c0, CE 0xa631e8 (1 in the exe; forced each frame while on, first value logged),
  game speed 0xa712ac, rain 0xb74d20 / fog 0xb74d64 / rain amount 0xa797d0, SB recharge 0xa71270, radar
  0xa590a4, minimap cops 0xa607d0/d1, neon barriers = letter 'Y' over "BARRIER_SPLINE_*" names in .rdata
  (0x9d85c8.., read at track load). Slots 0xc2b700.. (game.toml instruction_patches + data_seeds): new
  career cash 004c4cc4/cd4, Quick Race laps 004ac7fb/80e (fn 004ac7c0), opponents 004ac87c/88f/8dd/8e6
  (fn 004ac840; canyon left), traffic PUSHes 007a9e3f/43/47, busted pixelation 0066f800/87b ([0xb74d94]),
  headlights relight PUSH 7 006c63a5 and beams 007cbe93/9b. Always on: no 10-save stop (NOP 005b729e),
  all saves listed (JMP at 005ac479). Calls from the per-frame input update (pad.h -> extras_tick):
  Game_SetWorldHeat 0065c550, SetCopsEnabled 006513e0, Force/ClearAIControl 0065c2c0/0065c330, glare
  on/off/is 006c0ba0/006c0bc0/006d8080 on [0xa9f168] (7 headlights, 0x7000 cop lights); driving = state
  [0xa99bbc] == 6. Heat bounds: callsite BEFORE hook (slot 26) on clamp 004010e0 (value, lo, hi) from
  00449a91 (ret 00449a96) in the pursuit update (fn ~00449920, not in the analysis). Not done: harder
  Quick Race pursuits (its patches are conditional jumps, not switchable), load-any-save.
- Online menu entries: DONE in mods/core/nfsc/online.h (setting hide_online, default on): the carousel
  builder 0083c420 adds items with 005b2320; the Quick Match / Custom Match adds (return 0083c611 /
  0083c5c3) are skipped. (Old notes: main menu state 00843690, carousel actions 005a5560.)
- Autosculpt (issue #4; CONFIRMED by the user 2026-11-07: parts reshape, zone line sits on the part):
  morphs = kit change 27 (PR #6, YevUA). Zone line = AS_SELECT_ANIM001 quads (texture set at 007b9cb0; per border
  segment 007d8c30 -> 007d2860/007d2990 -> 007c9150) drawn by the FE quad renderer 0071c770 with the widescreen
  menu's x scale 009e8f8c. 007c9150 projects two part vertices to pixels (0070f1f0) and turns x into menu units
  with FMUL 640.0 (007c9214, 007c925b), so the line was squeezed towards the left edge by (4/3)/aspect, Widescreen
  on or off (apply_hud sets the menu scale either way). Both redirected to slot 0xc2b41c (480*aspect), as the
  cursor at 00731665. Not this line: "Zone Border FatLine Mesh" 00596a10 = the world map's territory borders.
- Vinyls (issue #4's "can't change the colour"; CONFIRMED fixed by the user 2026-11-07: red paint shows around a
  vinyl, before it went black): kit change 28. Vector vinyls (CARS\VINYLS\VINYLS.BIN, chunks 0x8003CE00, loaded by
  007d23d0 onto the list 0xb77010) are painted on the CPU: compositor steps in 007dba00 (2/3 fill 007b2620, 4/5
  007d09d0 and 6 007d1010 rasterize into a texture locked with 0070f740 = LockRect(0) / 0070f770 = UnlockRect, 9
  007b2420 unlocks, 0xc 007d7580 copies the result by the target's format byte +0x2e: 0x20 copy, 0x08 palette,
  0x22 DXT1 -> 00732ca0 D3DXLoadSurfaceFromSurface). The skin is 1024x1024 A8R8G8B8, transparent outside the
  vinyl, into DXT1. 00752070 is the same rasterizer for crew logos (CREW_LOGO_PLAYER) and menu previews.
- Full resolution list + Display mode (git 579ed7d, CONFIRMED by the user 2026-10-10: 1920x1080
  letterboxed correctly, no stretch; F10 (fn+F10) Window row switches modes): list =
  screen/1..4 ("(native)" on k=1) + common 4:3/16:10/16:9/21:9 sizes <= screen, sorted by
  width; g_aspect follows the chosen size (FOV/HUD); needs kit change 16. Window mode =
  the kit's F10 settings page (game.toml [settings] rows = MW's list). Unknown: whether
  gameplay frames keep aspect for non-screen shapes (compositor's non-legacy gameplay
  path draws the world to the whole drawable -> could stretch).
- Resolution menu (CONFIRMED working 2026-10-10 by the user): list = full-screen size / 1..4
  (>=480 rows), default nearest 1080 rows; option index 00a65388; tables at 0xc2b500
  (supported) / 0xc2b5a0 (refresh) via 8 redirects (indexed operands need kit change 15);
  NOP 007115f0/007116df; save PUSH at 00711d3f -> slot 0xc2b640 (packed 0x80000000|w<<16|h);
  REPLACE hooks IncScreenRes 004a51f0, GetLocg_RacingResolution 004a3830,
  GetLocalizedWideString 00588f20 (labels, keys NFSC_NATIVE_RES_WxH). Unlisted readers
  00712b85/00712c65/0072bee0 keep the original tables. HUD re-applied per size change.
- Source of the fix: github.com/ThirteenAG/WidescreenFixesPack source/NFSCarbon.WidescreenFix
  (Resolution.ixx, Rendering.ixx, Frontend.ixx...). Its addresses match our exe.

## HD UI mod (NFS Carbon Remastered UI 4.4, BadassBaboon; STARTED 2026-10-10)
- Download: nfsmods.xyz/mod/1095, https://nfsmods.xyz/uploads/mods/1095/f096ab-Remastered-Textures-4-4.7z
  (52,135,927 bytes, sha256 e55c7d8e...66ab; macOS /usr/bin/tar reads 7z). No stated licence: never
  ship it; Setup app downloads it (opt-in), like the XtendedInput icons.
- Contents: RemasteredUI/RemasteredUI.end (Binarius "User" launcher: Files list + Endscript
  assets/main.end) + 731 DDS + 4 FNG .bin + 2 whole BUNs per language (LANGUAGE/ENG/BUNS/FRONTB2.BUN,
  GLOBALA.BUN, copied with move_file) + scripts/NFSCHDContentSupport.asi (Aero_, GPL-3,
  github.com/AeroWidescreen/NFSHDContentSupport). Script commands: bind_textures synchronize (270),
  replace_texture (139), update_texture (117), delete (64), update_collection (47), import (10),
  move_file (7), new (6), unpack_stream (2), if collection_exists/checkbox blocks.
- Plan: build-time rewrite of the app's copy of the game files (what Binarius does on Windows):
  Python endscript subset + TPK reader/writer (format from Darius, MIT, github.com/nlgxzef/Darius
  Nikki/Support.Carbon/Class/TPKBlock.cs + Texture.cs; Binarius itself is GPL-3). The game never
  needs the ASI: HD Content Support only makes layout use the ORIGINAL size for FONT_NFS_BODY/DEBUG
  (256x256), FONT_NFS_TITLE (512x256), FONT_NFS_MOVIE_LARGE (512x512), FONT_NFS_TACH_NUMBERS (128x64)
  (reads of TextureInfo +0x28/+0x2a at 00585d20 in 00585d10 and 0071ca06 in 0071c770) and the cursor
  (fixed 16 at 005853a9/bf/d6/f1 in 00585290). Plan: core.nfsc writes the original sizes back into
  those TextureInfo records after load (D3D texture already created from the real size).
- .lzc files are whole-file JDLZ (header 'JDLZ' 02 10, size at +8); the game decompresses only when
  the header says so (0069eda0), so rewritten files can be stored uncompressed under the same name.
  A Python JDLZ decoder matched GLOBALB.BUN / InGameB.bun / FRONTB1.BUN byte for byte.
- tools/setup_kit/bun.py (2026-10-10): JDLZ decoder, chunk file + TPK parse/assemble. Round trip
  is byte-identical for FRONTB1.BUN and GLOBALA.BUN; GlobalB.lzc / InGameA.bun differ only inside
  filler (id 0) chunks. Compressed TPKs (info has 33310003 offslots {key, offset, encoded, decoded,
  user flags, flags, refcount, ?} instead of 33310004/5; each texture a blob with its 0x94-byte
  header at the END; flags 0 raw, 1 one compressed block, 2 by parts (LZ magic blocks)) are NOT read
  yet: DYNTEX.BIN, WMDYNTEX.BIN, TRACKS/L5RA/TRACKMAPS.BIN, MINI_MAP_*.BIN (+ likely NIS bundles).
  RAWW (stored) blocks are accepted by the game (0069eda0), so a writer can avoid a real compressor.
- Stage 1 BUILT 2026-10-10, NOT yet run: tools/setup_kit/hd_ui.py (Endscript subset: if
  collection_exists, combobox (English / Vanilla), checkbox (disabled branch), move_file (skips
  scripts/*.asi), bind_textures synchronize/override, replace_texture, update_texture (FIELDS map),
  import synchronize (FNG 0x30203 chunk with the same .fng name), unlock_memory (Darius FastUnlock x4
  + LongUnlock blob for GlobalMemoryFile.bin)); package.py/setup.py --hd-ui; Setup app checkbox;
  mods/core/nfsc/hd_ui.h writes original sizes into GetTextureInfo(0055cfd0) records each frame
  (BEFORE 00600c90, hook slot 16; g_hooks now 24). Archive cached in ~/Library/Caches/NFS Carbon
  Setup. Writes FrontB1.lzc 40 MB, GlobalB.lzc 117 MB, InGameA.bun, InGameB.lzc, GLOBALA.BUN,
  FRONTB2.BUN, 5 memory files, ~45 NIS bundles. Skipped: WMDYNTEX/DYNTEX/minimaps/TRACKMAPS
  (compressed), streamed L5RA textures, WORLDMAPMAIN import + calibration.
- unlock_memory: Darius's version EMPTIES the memory files and writes its own BuildVersion.moo
  (machine "D1015576"; the player's 1.4 has "D1002400"). With it the user's profile loaded but the
  career showed "Start Career" (save intact on disk, backed up to ~/Documents/NFS Carbon save
  backup 20261010-2043). The memory files only hold byte-identical copies of disk files (checked
  all entries) plus that stamp, so hd_ui.py now removes ONLY the entries of files it rewrote
  (today GlobalB.lzc, hash 4c939b36) and keeps the rest. Built 2026-10-10, career NOT yet re-tested.
- RESUMED + CONFIRMED 2026-10-20: the full HD UI build (all stage-1 files) loads the user's career
  ("Resume Career") and plays fine. The fonts-only test that failed on 10-10 passed today with no
  failed bMalloc (00476b20; note its OOM path writes to address 0, which does NOT crash in the port),
  so the 10-10 "Start Career" was most likely the Netbios bug fixed 10-17 (f2fb6ad: network thread
  crashing at profile load, a stray write whose target depended on memory layout). Setup app
  offerHDUI = true again.
- (old) PAUSED 2026-10-10 by the user (HD UI dropped for now; Setup app hides it, offerHDUI = false).
  Career bisect with HD_UI_SKIP (wildcards): the career shows "Start Career" (save intact, loads
  fine without HD) with ONLY GLOBAL/GLOBALA.BUN rewritten (the mod's GLOBALA copy is byte-identical
  to the original; our install then binds HD FONT_NFS_BODY/TITLE 2048x2048 etc. into its
  GLOBALMESSAGETEXTURES TPK -> 12.9 MB). Not the memory files, not the build stamp, not GlobalB,
  not FRONTB2. No pool OOM (00476b20 would crash). Next suspects: the font TPK itself (our header
  values for reloaded/added textures vs Binarius; try binding only one font; try the mod's
  GLOBALA with no binding), or the hd_ui.h size write-back (writes TextureInfo +0x28 every frame
  when sizes differ - try an HD build with hd_ui.h disabled), or text layout code reading the
  bigger font and the career menu entry's text/label logic.
- STAGE 2 CONFIRMED by the user 2026-10-20 (title logo, world map, minimap, track maps, career all fine): bun.py CompressedTpk (rows 33310003; parts 0x55441122
  {unpacked size, stored size to the next part, unpacked pos, stored pos, 0} + JDLZ/HUFF/RAWW block
  (JDLZ size at +12 includes the 16-byte header, HUFF/RAWW's does not); blob = pixels + 0x7C header +
  0x18 format; offsets from the TPK chunk; blobs 0x40-aligned after the 0x78 filler; the header's
  +20 offset is a combined-layout position). Unchanged textures are copied as stored (round trip
  byte-identical for DYNTEX, WMDYNTEX, TRACKMAPS, both minimaps); changed/new ones are RAWW parts
  of <= 32 KB, header part, part 0 last. The header +20 offsets MUST run on in stored order, 0x80-
  aligned (all retail files do; the game unpacks a pack into one buffer at those offsets). The first
  stage-2 build appended bigger textures past the old end -> overflow: garbled 2x title logo
  (XENON_WS_SPLASH_SCREEN in DYNTEX) and a hang on opening the world map (2026-10-20). SECOND cause (live lldb on the world-map hang, 2026-10-20): a by-parts
  texture is read into a buffer of its UNPACKED size (0055b8d0) and unpacked in place backwards (006aaba0), so
  stored parts bigger than the texture (our RAWW parts: +40 bytes per 32 KB) overflowed it (0x6410 bytes over a
  4096^2 TRACK_MAP, into the TPK list 0xa9017c -> endless loop in 0055ebd0). Changed textures are now stored
  PLAIN (row flags byte +0x11 = 0: stored bytes = the unpacked blob, 0055b710 skips decoding); moved ones keep
  their parts only if CompressedTpk.fits_in_place (all 398 retail textures pass). Now every
  texture is re-laid out; an unchanged one whose offset moved keeps its stored parts except the
  one holding its header, rewritten as RAWW. HUFF decoder
  from the MW port's tools/setup_kit/tpk.py. Collection names match on 27 chars (stored name
  limit). import replaces raw (0x30203) or packed (0x30210 = hash + JDLZ) packages
  (WorldMapMain.fng in GlobalB); update_collection Tracks N TrackMapCalibration* (records 0x110 in
  chunk 0x34201, number at +0x8a, X/Y/Width at +0xac/+0xb0/+0xb4; vanilla already 105/-4150/6300).
  RAWW storage makes DYNTEX 50 MB, WMDYNTEX 154 MB, minimaps ~108 MB each, TRACKMAPS 101 MB
  (a JDLZ compressor would shrink them). Still skipped: streamed L5RA textures (barrier arrows,
  unpack_stream/pack_stream).
- SMALLER APP + BARRIER ARROWS (2026-10-20; shipped in release v0.1.13): tools/setup_kit/jdlz.c (cc'd once into the
  Setup cache, ctypes; self-checks each block) packs changed compressed-pack textures in 32 KB JDLZ parts
  (fits_in_place-checked, plain fallback) and re-JDLZes rewritten .lzc files: HD UI growth ~1 GB -> ~300 MB.
  Stream sections: unpack_stream registers TEMP/<section>/DATA.BIN as virtual files (read from the stream at the
  index record's offset); a changed section is appended (0x800-aligned) to the app's copy of STREAML5RA.BUN and its
  L5RA.BUN record (chunk 0x34110, 0x5C each: name[8], +0x14 offset, +0x18 size, +0x1C stored size; +0x34 hash,
  no checksum strings in the exe) repointed. "TPK<n>" = the n-th pack. Y0 (shared textures) 72.6 -> 116 MB with
  the two 2048^2 ARGB arrows; track streaming pool (006b6e00 -> 007a42e0: 0x445c000/0x441c000 + 65000 KB +
  extras, about 133 MB) made 48 MB bigger by game.toml patches at 006b6ef2/006b6ef9. 60FPS barrier checkbox on.
- Order of work: (1) whole-file copies (GLOBALA/FRONTB2 per language) + uncompressed TPKs (FRONTB1,
  GLOBALA, INGAMEA, INGAMEB, GLOBALB) + font/cursor original-size fix in core.nfsc + Setup checkbox;
  (2) compressed TPKs + FNG imports + NIS; (3) the optional extras (2015 minimap etc.).
- TPK (chunk 0xB3300000): info 0xB3310000 {33310001 0x7C header (version, 0x1C collection name,
  0x40 filename, filename hash), 33310002 key+0 pairs, 33310004 texture headers (0x58 fixed, hash
  +12, class +16, data offset +20, palette offset +24, size +28, palette size +32, area +36, w/h
  +40/+42, log2 w/h, compression, mipmaps ..., then name length byte + name, padded to 4), 33310005
  0x18 per texture (fourcc / 0x15 at +12)}, padding to 0x80, data 0xB3320000 {33320001 0x18, pad
  0x50 chunk, 33320002 pixels from the next 0x80 boundary, each texture padded to 0x80}.

## Setup app (tools/installer/, setup.sh, tools/setup_kit/setup.py)
- CONFIRMED 2026-10-10: a fresh build through the Setup app (selftest: all 7 steps, ~11 min incl.
  analysis on an M4) and the user's own install from the disk image both work; game plays as before.
- `tools/installer/build_installer.sh [version]` -> dist/NFS-Carbon-Setup-<version>.dmg (carries
  `git archive HEAD`, so commit first). `Setup --snapshot DIR` renders every window state;
  `Setup --selftest GAME OUT` runs check + build headless. No @State in SwiftUI views: the macOS 27
  Command Line Tools have no SwiftUI macro plugin.
- setup.sh Python (issue #5, 2026-11-04): only 3.9-3.12 (python3.12/3.11/3.10, then the CLT's /usr/bin/python3):
  numpy==2.0.2 (kit texture-pack pin, unused by Carbon's build) has no cp313+ wheel, so a Homebrew/python.org 3.13
  picked first failed pip and showed the "can't reach pypi.org" message. A .venv outside 3.9-3.12 is remade (the app
  keeps .venv across updates); pip runs --only-binary :all: into build/setup.log; the network message only when the
  log shows a connection error, else the Python version + pip's last ERROR line.
- setup.py pins NFSC.exe (game.toml sha256) and the stock msvcr71/msvcp71 hashes (game.toml's
  addresses inside them are for those exact files).

## Detail level / FirstTime (kit change 17, CONFIRMED fixed by the user 2026-10-10)
- Bug: Level of Detail Maximum came back as High after a restart. Diag hooks showed: 007346b0 opens
  HKLM\...\Need for Speed Carbon with KEY_READ (0x20019) and writes FirstTime = 1 through it
  (refused on Windows); the kit stored it, so 00711810 loaded FirstTime = 1 and 00729d10 re-ran the
  hardware detection 0071d0a0 every start (our d3d9 reports GeForce 7800, device 0x0091, which is on
  the "High" list 009e8078) and saved it. Fix: kit/runtime/misc.cpp refuses RegSetValueEx through a
  handle opened without KEY_SET_VALUE (ERROR_ACCESS_DENIED), as Windows does.
- Levels: 0 Low, 1 Medium, 2 High, 4 Maximum, 5 Custom (00ab0ae8); 007115d0 applies a level.
- BLACK SCREEN, found 2026-10-10 19:3x (live capture + lldb on a black game): the game keeps running
  (main thread in Dev_Present) but the presenter shows nothing: a frame's GPU work never completed
  ("GPU completion timed out; retaining target until completion", right after the full-screen window
  lost and regained focus), it held one of the 3 flight slots forever, the slots filled and
  Service::tick returned early every time. Fix (kit host/present_thread.cpp; no black screen in the user's alt-tab test 2026-10-10): a
  flight with no GPU completion after 2 s cancels every flight into `retiring` (targets kept until
  their GPU work ends) and recreates the swapchain at the next tick, as installing a new layer does.
  Earlier first-launch blacks were likely the same (the move into full screen at start-up).
- Start-up black screen CONFIRMED FIXED by the user 2026-10-11 (kit change 19); the temporary
  diag_movie.h trail was removed.
- Black screen (paused game) ROOT CAUSE FOUND 2026-10-10 20:53 (live process, lldb on g_mem):
  [0xab0ad0] (WM_ACTIVATEAPP active flag, set by WndProc 007121da) = 0 and [0xab0b3c] = 1
  (WM_ACTIVATE "inactive"), host focused; main thread blocked in GetMessageA (the game's paused
  loop), no audio. host/sdl/main.cpp queue_or_apply queued a focus LOSS during an idle slice and
  applied the following GAIN immediately, so (1) reached the guest before (0). Fix (kit change 19,
  CONFIRMED 2026-10-10 22:3x: many launches, no black screen): input arriving outside a slice while older input waits is appended and the
  queue drained in order; apply_focus posts the CURRENT g_focused. lldb attach to the app works
  (ad hoc signed): `lldb -p PID --batch -o "p/x *(unsigned long*)&g_mem"` then guest addr + base.
- Second black-screen kind (2026-10-10 21:0x, focus flags fine, F10 page draws over it, no music):
  FE stack = FeBlackScreen.fng + FEAnyMovie.fng, queued movie (0xa97a88) = "EA_Bumper"; no video
  decode thread, so the movie never started. Root cause NOT found. Mitigation (built, not yet run):
  mods/core/nfsc/intro.h sets the game's skip-movies dword 0x00a9e6d8 (the Extra Options switch;
  read at 005baec2 for FE movie objects, type 7 -> message 0xc3960eb9) only while the queued movie
  is EA_Bumper(_WS)/PSA_Nikki/EAHD_Bumper; setting skip_intro (default on). FE engine =
  **0x00a97a78; its package list at +0xe0 (next +4), package name pointer at +0xc.
- First-launch black screen: tools/dev/first_launch_capture.sh (fresh profile + cold Metal cache +
  `sample`) did NOT reproduce it in 3 launches on 2026-10-10; left as a rare known issue.

## Safehouse crash (CONFIRMED fixed 2026-10-10)
- First safehouse visit: ExitProcess(0xC0000090) via the CRT (LoadLibrary mscoree first).
  00730840 calls _controlfp(3, 0x8001f) (unmasks invalid/zero-divide/overflow) before CreateDevice;
  Direct3D 9 masks them again unless D3DCREATE_FPU_PRESERVE. Kit d3d9 D9_CreateDevice now ORs 0x3f
  into the guest fpu_cw (masks only, precision unchanged).
- Keyboard key names: kit user32 ToUnicode (letters were blank on Controls) CONFIRMED.

## Render resolution (2026-10-11)
- The Metal backend draws every render target at drawable rows / game rows (rounded to 1/8), so
  the in-game resolution never changed GPU work: 1920x1080 on the M4 Air rendered 3840x2160 + 4x
  MSAA (GPU 14.6 ms, 47 fps in Hillsborough). RECOMP_D3D9_SCALE=1 gave a stable 60 (user test).
  Setting core.nfsc/render_scale (0 Screen = follow screen, 1 = 1.5x, 2 Game resolution = 1x,
  default since 2026-10-20: Balanced dropped frames in heavy scenes on the M4 Air) is read by launch.c from mod-settings.json and exported as RECOMP_D3D9_SCALE.
- Other video options: in-game V-Sync does nothing (presenter always display-synced; F10 frame
  limit rules); in-game Texture Filtering is overridden by F10 World filtering unless "Game's own";
  FSAA works (game's multisample type -> 2x/4x).

## Languages (2026-10-11)
- Language = registry REG_SZ "Language" in the settings key (read by 007346b0, names table
  0xa65588: 19 entries incl. Thai/Japanese/Korean/Chinese), but this exe's file table (0xa5eac4)
  has only ENGLISH FRENCH GERMAN ITALIAN SPANISH DUTCH SWEDISH POLISH FINNISH DANISH HUNGARIAN
  CZECH MEXICAN (LANGUAGES\\<X>_GLOBAL/FRONTEND/INGAME.BIN). Asian releases (a Reddit user's Thai
  copy) used a different exe -> Setup rejects them. package.py game_language(): one non-English
  language in LANGUAGES -> Defaults/registry.json gets Language (first-run profiles only).

## NFSCarbon.WidescreenFix port (rest, 2026-10-11; user: HD intro, widescreen movies, loading screens OK)
- game.toml patches: movie text 005ab6d7, native WS movies 005bb818/005bd4a1, EAHD_Bumper pushes
  (005bb83b/005bd4a3/005bd4b0, 00857701 PUSH 0), autosave 005cd170 JMP 005cd14d, world-map cursor
  00570dcd/00570ddc, CrashFix 0059606d/005960a9, car shadow slot c2b424, water reflections
  (redirects to c2b430/c2b434, 0071a9f7 MOV EDX,0xf0, NOP 0071a9fc), rain 00722cb0 (slots c2b428 =
  0.5/aspect, c2b42c = 0.5, filled in apply_hud; rain CONFIRMED by the user). Arrest blur 0x9d0584 written
  in apply_hud. Autosculpt = the cursor redirect at 00731665 (already there); its zone line needed two
  more (Mods > Autosculpt, 2026-11-07).
- MoviePlaybackFix (mods/core/nfsc/movie.h) BACK and CONFIRMED by the user 2026-10-23 (issue #1: a
  player's game on an external drive froze on the EA logo 3x and hung in story movies; their logs show
  the movie player starting 2 threads but never the 2 video-decoder threads). 00888908 checks the audio
  reader ([esi+0x180], null = no audio) once via 008d3ca0; callsite hook (slot 20) waits alertably
  (SleepEx IAT 0x9c1140) up to 5 s for audio, as ThirteenAG's. The 2026-10-10 "pause after the intro"
  was MEASURED: Attract has an audio reader but no audio chunks (MVhd + MV0F only), so the wait timed
  out. Now the stream's chunk list ([reader+4]+0x50; node +0 next, +8 size, +0xc chunk bytes, +0x14
  reader index) decides: SC* chunk = audio, a frame (MV0K/MV0F) with no SC* first = none (SCHl always
  precedes the first frame). Readers: video [esi+0x15c] (00888831 waits for >= 0x20 bytes, then MVhd),
  audio [esi+0x180], subtitles [esi+0x19c]. Each movie start is logged ("core.nfsc: movie NAME: ...").
- Not ported (not widescreen): Camera.ixx free look, PostFX SMAA, MSAA, SimRate/high-FPS.

## Cheats (mods/core/nfsc/cheats.h, 2026-10-17; user: all work)
- Settings (group cheats, Mods tab, live): infinite_nitrous, infinite_speedbreaker, engine_power
  (Normal/150/200/300%), opponent_power (Normal/Half/Quarter; every non-player car).
- REPLACE 006e3df0 (engine nitrous step, thiscall, RET 0xc): tank [engine+0xf0]; its returned
  ST0 is the power multiplier the drivetrain 006ffe60 stores at [engine+0xf4] and the torque
  curve 006e31e0 multiplies by, so the cheat scales ST0. Player test = [[engine+0x2c]]->vfunc 0x20
  (the game's own infinite-nitrous reward branch, card 10). Speedbreaker byte 0x00b4d86c
  (Tweak_InfiniteRaceBreaker, NFSPluginSDK). 006e31e0 itself is not hookable (not a listed function).
  Canyon duels turn nitrous/speedbreaker off; the power settings still work.
- no_catchup (2026-10-20, CONFIRMED by the user): REPLACE 0063ead0 (GRaceParameters catch-up getter,
  thiscall, plain RET, AL; attribute 0x10db04e6, missing = on; Quick Race's option writes it via
  00626440 from QR block profile+0x2b0c4+mode*0x28 byte 0xb, menu id 0x13e6). Hook slot 23.
- Player's Kenji hang (log 20261015-205451, 58-min session, external drive): not reproduced by
  the user with these cheats (2026-10-17); not Netbios (that session never called it). Unconfirmed;
  next evidence = `sample CarbonRecomp 10` during a hang. Scheduler "baton held" numbers count idle
  time of the last hand-off thread, not starvation.

## Kit changes made so far (all in kit/)
1. Loader: aux DLLs map before the exe's IAT is bound; exe imports bind to their
   exports; statically imported DLLs get DllMain(PROCESS_ATTACH) in key order before
   the entry; a stock DLL beside the exe with the wrong hash no longer shadows the
   rebased one.
2. Shims: kernel32 (InterlockedCompareExchange/ExchangeAdd, DisableThreadLibraryCalls,
   GetComputerNameA, console/pipe stubs…), advapi32 (not admin; CryptGenRandom),
   ole32 CoCreateGuid, rpcrt4 UUID strings, ws2_32 byte-order helpers.
   D3DX shims also registered as d3dx9_30.dll; 6 texture/surface functions fail cleanly.
   `CreateVolumeTexture` in d3d9 works (A8R8G8B8/X8R8G8B8/A8/L8); see kit change 7.
3. Translator: JCXZ; CRT exits through the IAT never return (not RaiseException);
   `data_ranges` config; masked jump-table slots that overlap an instruction are holes
   (msvcr71 memmove); `_initterm` found when called via IAT, a register, or an
   `FF 25` import thunk (NFSC.exe uses thunk 0087f1a0, table 0xa4a000-0xa4bf68).
4. Runtime: RaiseException(0x406D1388) = SetThreadName, returns immediately.
5. Translator (`carbon-kit-xmmword.patch`): 16-byte memory operands spelt `xmmword`, so
   unlisted functions with an SSE path recover (SSE ops become recomp_unmodelled traps;
   CPUID advertises no SSE so the game takes its x87 path). Fixed 008ab884 + 4 others;
   recovery errors 19 -> 8 (remaining 8 are width-6 decodes of data).
6. d3d9 shadergen (`carbon-kit-psfog.patch`): ps_3_0 inputs declared with usage 11
   (FOG) read the fog varying instead of failing the whole pixel shader.
7. d3dx9 (`carbon-kit-fillvolume.patch`): D3DXFillVolumeTexture implemented (helper
   d3d9_fill_volume in d3d9.cpp calls the guest LPD3DXFILL3D per texel). Carbon's
   colour-grading lookup (0073c1a0 creates an N^3 A8R8G8B8 volume, callback 0073c100)
   was all zeros, so IDI_VISUALTREATMENT_FX turned every 3D pixel black.
8. d3dx9 (`carbon-kit-fxshared-refs.patch`): shared effect-pool parameters
   (share_parameter / join_pool) now take their own texture reference
   (set_param_texture); fx_destroy released one per effect, so destroying effects at
   race load freed the colour-grading LUT the game still used. Fx_GetTexture returns
   a volume texture as IDirect3DVolumeTexture9.
9. d3dx9 (`carbon-kit-fxsampler-refs.patch`): effect passes binding device sampler
   slots (bind_constants sampler, pass SAMPLER_TEXTURE state) go through
   bind_sampler_slot, which takes/releases the volume-texture reference the way
   Dev_SetTexture does. Backtraces proved Dev_SetTexture released the LUT twice
   (refs 2->1->0) for slots the effect had filled without a reference.
10. d3d9/d3dx9 (`carbon-kit-dds-cube-volume.patch`): D3DXCreateCubeTextureFromFileInMemory
   and D3DXCreateVolumeTextureFromFileInMemory load DDS (DXT1-5, A8R8G8B8, X8R8G8B8,
   A8, L8, mips). Called by GLOBALB.BUN chunk handlers 0070f6c0 (0x30300201: five
   256^2 DXT1 environment cubes into 0xab09d0[]) and 0070f670 (0x30300200: 32^3
   A8R8G8B8 volume, 6 mips, into 0xab09e8). volume_create() split out of
   Dev_CreateVolumeTexture.
11. d3d9/d3dx9 (`carbon-kit-loadsurface.patch`): D3DXLoadSurfaceFromSurface for whole
   surfaces (no rects): same format+size copies rows, otherwise decode (DXT1/3/5,
   A8R8G8B8, X8R8G8B8, R5G6B5, X1/A1R5G5B5, A4R4G4B4, A8, L8) -> nearest-neighbour
   scale -> colour key -> encode (simple bounding-box DXT encoder). Source is the
   surface's CPU copy. Callers: 007d7580 (texture copy when formats differ),
   00759040 (per mip), 00732ca0 (boot).
Ported from MW legacy-3.x on 2026-11-05 (kit tests pass apart from failures that were already there). First game run
(log 20261105-221307, 25 min, clean exit): F10 paused 15.6 s and 1.3 s with 3 settings applied while paused; pacing
60.00 fps most minutes, 1 drop in 89k frames; no heap guard hits; sound objects in the kit span (0x7e1...):
22. Scheduler lock (07cc255, MW 2.0.1): handle table inserts/erases and thread_spawn take g_sched_m; fault log
   prints host pc/lr + image load (atos). runtime_tests --sched-race.
23. Heap guards + kit span (b07c98e, MW 3.0.2-3.0.4, issue 8): 16 KB 0xfd gaps around DirectSound rings and guest
   thread stacks ("heap guard: ... written outside its span" in the log = a stray write caught), top 32 MB of the
   extra heap (..0x80000000) only for kit objects; first 48 guest blocks >= 256 KB logged with EIP. Off switches
   RECOMP_HEAP_GUARD=0 / RECOMP_KIT_SPAN=0; read-only sound views opt-in RECOMP_READONLY_PAGES=1.
24. Refresh-slot pacing + 120 Hz (c245c92, MW 3.0.2/3.0.3): Dev_Present follows the presenter's refresh slots
   when limit == refresh (SlotPacer; RECOMP_PRESENT_OWN_CLOCK=1 = old); "d3d9: frame pacing, last 60 s" log line;
   presenter period from NSScreen.maximumFramesPerSecond of the window's screen (host/sdl/os_display_macos.mm).
25. F10 menu pauses the game (8fa8f63, MW 3.0.4): core.nfsc pause_in_menu (default on); Dev_Present parks the
   main guest thread, host_millis held, mixer paused; game.toml [settings] core_mod = "core.nfsc"; parked menu
   redraw publishes to the glass views. Log: "mods: settings menu paused the game" / "resumed ... after N s".
   Not yet ported from 3.0.4: Check for updates (kit/host/update_check.cpp), Save bug report, thread start
   captured at CreateThread (kernel32), mute in background, deadzones.
26. Performance overlay on native glass (2026-11-06; user: "looks really good"): host/perf_glass.h +
   host/sdl/perf_glass_mac.mm (macOS desktop app only; RECOMP_PERF_GLASS=0 = old Metal overlay): pass-through
   container right above the SDL Metal view (ticked after the menu's, so the F10 menu stays on top), Liquid Glass
   panel top-right (below safeAreaInsets.top), SF monospaced digits, fps coloured green/orange/red vs the limit,
   graph = CAShapeLayers (green / red late bars, dashed budget). present_thread publishes the snapshot (<= 4 Hz)
   and skips PerformanceOverlay::draw unless a capture run is on (its marker lines stay Metal). Weak defaults in
   performance_overlay.cpp.
27. Still-locked vertex buffers (PR #6 by YevUA, 2026-11-07; CONFIRMED by the user: Autosculpt reshapes parts):
   Autosculpt's CPU morph 00558e10 locks the part's vertex buffer via 0070ed70, writes it with 0070eeb0 and ends in
   0070eda0; both helpers call Lock (+0x2c), never Unlock (+0x30), which works on Windows (the pointer is the
   buffer). The kit's lock is a staged copy that reached blob only at Unlock, so Metal drew the stock part.
   gpu_sync_buffer (dx/d3d9.cpp) now copies a still-locked buffer's staged copy into blob (whole buffer dirty)
   when they differ; one memcmp otherwise.
28. DXT1 alpha in D3DXLoadSurfaceFromSurface (2026-11-07; CONFIRMED by the user: paint shows around a vinyl):
   rgba_to_surface (dx/d3d9.cpp) wrote every DXT1 block opaque; now a block with pixels under half alpha is written
   in 3-colour mode (index 3 transparent, endpoints from the opaque pixels), as D3DX does. The car's vinyl skin
   (1024x1024 A8R8G8B8 -> DXT1: 63774 of 65536 blocks have transparent pixels) came out black around the vinyl.
   Each kind of copy is logged once: "d3dx9: D3DXLoadSurfaceFromSurface WxH fmt -> WxH fmt (N DXT1 blocks with
   transparent pixels)". d3d9_tests test_dxt1_alpha_copy (48 failures on the old encoder).
   Endpoints (PR by YevUA, 2026-11-07): dxt_fit takes the opaque texels' principal axis and refines the two colours
   by least squares against the 565 palette the decoder uses (3- or 4-colour); 565 packing rounds. The bounding-box
   corners were colours no texel had (a red-green ramp became black / yellow). d3d9_tests test_dxt1_endpoint_fit
   (22 failures on the box fit).
21. MetalFX upscaling (git, 2026-10-31; CONFIRMED running by the user's log 2026-10-31: "gpu: MetalFX spatial
   upscaling 1920x1080 -> 3420x1924", 5907 frames, no new faults): gpu::Device::spatial_upscale (default false;
   metal_device.mm: MTLFXSpatialScaler, perceptual colour, cached per in/out size + format, needs
   supportsDevice + Private storage + the descriptor's usages; MetalFX framework in host/gpu/CMakeLists.txt).
   compositor.cpp upscaled(): the main frame (legacy frame, or gameplay world) goes through it into one of 2
   cached targets when it is enlarged by >= 10%, BGRA8/RGBA8 and not HDR; then the quad draws that target
   (sampling ~1:1). Anything refused -> the old smooth stretch. compositor_set_upscaling() from present_thread
   hdr_poll every 30 polls, setting core.nfsc/upscaling (Graphics, live, default on). Render scale "Screen"
   draws at full size, so nothing is upscaled there. Metal 4 / frame interpolation judged not worth it (no
   motion vectors; already 60 fps).
20. F10 menu on native glass (git, 2026-10-27; CONFIRMED working by the user 2026-10-29): host/sdl/
   menu_glass_mac.mm (macOS desktop app only; RECOMP_MENU_GLASS=0 turns it off): a pass-through container
   (hitTest nil, so SDL still gets every event and input_gate maps the pointer by page fractions as before)
   over the SDL Metal view holds a dim, NSGlassEffectView (macOS 26+, style regular, tint black 0.34; older:
   NSVisualEffectView HUD material) at the panel, and the page RGBA (premultiplied CGImage) on top. The
   presenter (attach_menu_page) publishes pixels instead of uploading a texture and sets
   CompositorInput::settings_page_native, so the compositor only places the page (compositor_settings_page_
   drawn). Views are updated by host_menu_glass_tick from pump()/idle_wait() (main thread). The drawer
   (mods/settings_menu_draw.cpp) has a glass look (mods_menu_set_style: no dim/panel fill, capsule tabs and
   buttons, rounded selection, switches for "On"/"Off" rows, system accent, mods_menu_panel exports the panel
   rect) and a text backend hook (mods_menu_set_text_backend; the .mm draws SF with Core Text, semibold via
   kCTFontWeightTrait 0.3). Weak defaults in page_overlay.cpp / mods_seam.cpp keep other hosts unchanged.
18. Brightness (git, 2026-10-10; CONFIRMED working by the user 2026-10-24): Dev_SetGammaRamp/GetGammaRamp store the
   D3DGAMMARAMP and call host_set_gamma_ramp (dx/host_api.h, weak no-op default); host/compositor.cpp
   keeps 17 samples per channel and the Metal compositor_fragment applies them (Quad.pad = 1, Quad
   grew a float4 gamma[17]) to every draw except the settings page. Identity ramp = off. Metal only:
   Vulkan/WebGPU shaders read the old Quad prefix and ignore it. compositor_tests: 24 failures are
   from scaling = "smooth" (tests expect integer rects), the same with and without this change.
17. Registry (git, 2026-10-10): writes through a read-only key handle fail with ERROR_ACCESS_DENIED
   (see "Detail level / FirstTime").
15. Translator (`carbon-kit-indexed-redirects.patch`): operand_redirects also match an
   address that ends an indexed operand ([EAX*0x4 + 0xADDR]).
14. Host (`carbon-kit-notch-fullscreen.patch`): note_screen_size also records the
   full-screen size = display minus NSScreen.safeAreaInsets.top (helper
   host_mac_safe_top_points in metal_surface.mm); mods' screen_size prefers it. On the
   13" notched MacBook Air: display 3420x2214, full screen 3420x2146 (notch strip 68 px), so the game is
   1710x1073 and fills the window; 1710x1107 was shrunk to fit -> side borders.
13. Runtime (`carbon-kit-screen-size.patch`): host_display_screen falls back to
   host_display_screen_size, so mods' screen_size() works on the desktop host.
12. Translator (`carbon-kit-module-redirects.patch`): configure_module() clears the exe's
   operand_redirects / instruction_patches / data_seeds, so an aux DLL translation no
   longer fails with "operand redirects matched nothing".
16. Presenter (git 0b704cd): game.toml `[settings] scaling = "smooth"` (kit default
   "integer") makes the compositor's whole-frame rect and host_present_fit take the
   fractional aspect-preserving scale, so any game resolution fills the screen. Carbon
   sets it. Built 2026-10-10, no visible change yet (the menu list is still screen/k);
   groundwork for a full resolution list + Display Mode (windowed/borderless/full).

## Where we are
- All three images translate (exe 49947/49970 functions) and the app builds:
  `build/CarbonRecomp.app`.
- 008dacdd is fixed (`carbon-kit-raisereturns.patch`, applied and built 2026-10-10 00:52).
- 2026-10-10 ~05:10: RACE RENDERS. 3D world in races and the menu garage/car both
  render, including after returning from a race (kit changes 7-9). Game is playable:
  profile, career, races, sound, HUD, 60 fps.
  Cube maps + file volume: kit change 10 — CONFIRMED working 05:19 (car reflections
  visible, all 6 DDS loaded). D3DXLoadSurfaceFromSurface: kit change 11, built, not
  yet run. SetGammaRamp deferred: the presenter has no gamma stage (needs a Metal
  present-shader change). Then: SetGammaRamp
  (brightness), LightEnable; thread calling address 0 at boot; registry.json install
  keys; then the user's mods (XtendedInput, widescreen, Extra Options, HD reflections).
- Run 2026-10-10 03:10-04:54: menu 3D renders; race black and menu black after a race.
  Refcount diagnostics showed the first LUT (volume 1292) freed at race load by kit
  internal releases only (no guest Release): over-release of shared effect params
  -> kit change 8. Not enough (05:01 run still black): backtrace diag showed the real
  over-release in Dev_SetTexture -> kit change 9. All temporary diag removed. Built,
  not yet run.
- Run 2026-10-10 03:00: Options > Audio crash fixed. Still no 3D anywhere (menu
  background car/scene and race world black; HUD, minimap, FE 2D fine, 60 fps).
  Cause found: D3DXFillVolumeTexture unimplemented -> black colour-grading LUT
  (kit change 7). Built, not yet run. Next if still black: cube maps
  (D3DXCreateCubeTextureFromFileInMemory, reflections/sky), D3DXLoadSurfaceFromSurface.
- Run 2026-10-10 02:41: BIG progress. Profile/alias created, main menu works, career
  boots and the race progresses with sound and HUD, but no 3D world renders (also some
  menu elements missing). Options > Audio crashed/hung. Said No to EA servers.
  Log: unknown targets 00555450, 005507a0 (translator recovered then withdrew both
  as "data") and 005a69b0 -> all added to entry_points; one pixel shader failed
  ("pixel shader input usage 11" = FOG) -> kit change 6. Rebuilt, not yet run.
  The "guest thunk ... fast-path miss" lines are NOT a problem: heap code is audio-bank
  routines linked by loader 00898a13 (helper table 0xa7d6c0, 40 fns from 00897ba8);
  after the thunk miss, interp_call runs them (no unknown-target follows).
  If 3D is still missing, next suspects: D3DXCreateCubeTextureFromFileInMemory /
  D3DXCreateVolumeTextureFromFileInMemory / D3DXFillVolumeTexture /
  D3DXLoadSurfaceFromSurface (all "not implemented") and CreateVolumeTexture stub.
- Run 2026-10-10 02:14 (with 0089777e): pressing a key at press-start now opens a
  yes/no prompt, which then hangs. Log: unknown target 008ab884 on the audio thread
  (return 008b65fe) — an audio decoder the translator refused ("unknown operand width
  16"); fixed by kit change 5, added to entry_points, rebuilt, not yet run. Still open:
  `guest thunk 016b0b64 fast-path miss` — the game runs code from HEAP memory (bytes
  `56 8b 74 24 08 56 e8 ..`, call resolving to exe 00897ba8; no copy of it in the exe).
  The runtime only runs tiny jump stubs from the heap and returns 0 otherwise. If the
  prompt still hangs, find who writes/calls 016b0b64 next.
- Bulk static scan FAILED (2026-10-10 01:51): `tools/find_missed_functions.py` found
  42k "function start after RET/INT3/JMP" candidates (analysis/scanned-entries.UNUSED-broke-boot.txt);
  building with them (`--discovered`) crashed on boot (the scanner, tools/find_missed_functions.py, was deleted in the cleanup): real functions such as the
  WndProc 00712060 became unknown targets, then SIGBUS at EIP 0. Don't feed the
  translator a static guess list. Reverted after the 02:06 crash: build is back to the good one
  (58164 functions, 18 recovery errors) plus 0x0089777e in game.toml (unknown target
  right after the press-start key press, run 01:32 at LOG=1: 60 fps, key registers,
  nothing happens). Not yet run. Faster option to try: `kit/tools/discover.py`
  (records only addresses a run really reaches).
- Run 2026-10-10 01:24: EA splash, street-racing warning video and sound all play
  correctly; reaches "press any key", but a key press does nothing and the menu runs
  ~3 fps. Keys do reach the game (TranslateMessage/MapVirtualKeyExA/GetKeyState
  after the press). One new unknown target before the press: 0059d6d0 (vtable +0x1c,
  called from 00589093 on a freshly allocated object) — added to entry_points,
  rebuilt, not yet run. That run was with RECOMP_LOG=2: 1.1 GB / 16.8M log lines,
  every import call and d3d9 draw traced — likely a big part of the 3 fps; measure
  speed with RECOMP_LOG=1 before suspecting the renderer.
- Run 2026-10-10 01:12 (after adding the 4 entry points below): ~2000 frames, loads
  GLOBAL/FRONTEND/LANGUAGES/CARS/TRACKS/SOUND, plays MOVIES\EA_Bumper_ENGLISH_ntsc.vp6:
  user saw a glitchy EA splash, then an all-green screen. VideoDecodeThread called
  unknown targets 0088510e and 00884a1d (MMX code Ghidra folded into 008849c8), and
  the main thread called 0054bac0. All three were added to entry_points and the app
  rebuilt; not yet run. Also new in that log: D3DXLoadSurfaceFromSurface,
  D3DXCreateCubeTextureFromFileInMemory, D3DXCreateVolumeTextureFromFileInMemory,
  D3DXFillVolumeTexture not implemented; SetGammaRamp not implemented; SendInput
  unimplemented; dinput force feedback unsupported (harmless).
- Earlier run (2026-10-10 01:01, window closed by hand): gets much further.
  The window opens, D3D9 device is created on Metal (640x480), all ~30 shader effects
  (IDI_*_FX) compile, cube maps/render targets are created, DirectSound starts and
  plays (silent) audio, and threads 2-7 start (incl. "RWAudioCore Dac"). Then the main
  thread stalls: only 2 Present calls, 0 ReadFile calls, no game data is ever loaded.
- Next problem (evidence): 4 "call to unknown target ... returning 0":
  0089f604, 0089f07c, 0089f494 (called via `CALL EBX` in the vector-constructor
  helper 00403a60, i.e. object constructors) and 008999ee (audio thread). Raw bytes
  show each starts right after a RET/JMP of the previous function with a normal
  prologue (`mov eax,ecx` / `push ebp; mov ebp,esp`): real functions Ghidra missed
  because they are only reached through pointers. Fix = add them to
  `[translate] entry_points` in game.toml and rebuild with `--regenerate`
  (or let `kit/tools/discover.py` collect more like them). Constructors "returning 0"
  leave objects unbuilt, the likely cause of the stall.
- Also seen, lower priority: one thread calls address 00000000 (return 0fdfff00,
  all registers 0) → access violation, unhandled. FOUND 2026-10-24: it is guest thread 5,
  CreateThread start=0094c740, a real function missing from the analysis (only reached as a thread
  start): DirtySock-style idle thread, `while ([0xbcbfbc]) { 0097ea10(); Sleep(50); }`, running flag
  [0xa83744]; 0097ea10 runs the registered network idle callbacks (0xbcc028 table) under a try-lock.
  The thread never runs in the port (offline play unaffected so far). Fix if ever needed: add
  0x0094c740 to entry_points (+ --regenerate). Left alone on purpose: it would start a thread that
  has never run here. Also: registry install key missing, so
  `Install Dir` (read in 007346b0, along with CD Drive, FirstTime, CacheSize,
  SwapSize, Language, StreamingInstall) falls back; it then looks for
  `C:\Program Files\EA GAMES\Need for Speed Carbon/NFS\ZDIR.BIN` (guest_root maps
  to original/, which has no NFS folder); WSAStartup/WSACleanup unimplemented;
  d3d9 LightEnable unimplemented; DirectSound rejects a 22050 Hz 6-ch buffer.
- Known next items after that: registry.json with Carbon's install keys
  (HKLM\Software\Electronic Arts\Need for Speed Carbon, values above); real hooks
  instead of sentinels; volume textures; then the user's mods (XtendedInput,
  widescreen, Extra Options, HD reflections; MW's mods/core/nfsmw is the template).

## Judder at 60 fps: uneven physics steps (FIXED 2026-11-06, user: "feels much better")
- Symptom: counter at 60 fps but motion not smooth. Cause measured (temporary per-frame hook, removed): the main loop
  006b7930 passes the frame time [0x00a99a5c] (006b4cb0, the game's QPC clock quantized to 1/3600 s, wobbles about
  +-1.5 ms around 16.67 even when real frames are even, sd 0.45 ms) to the sim service 00761150: accumulator
  [0x00b4d7ec] += dt, floor(acc / (1/60)) fixed steps (max 8; [0x00b4cfe0] = sim running). When the remainder sat
  near 0 or a whole step, on-time frames flipped 0 steps (world frozen a refresh) / 2 steps (jump): 10-40 pairs per
  10 s in half the samples, up to 131. Fix mods/core/nfsc/smooth_sim.h (REPLACE 00761150, hook slot 28, setting
  smooth_sim, Graphics, live, default on): before each call the remainder is pulled 10% (max 0.25 ms) towards half
  a step. After: 0 pairs in nearly all 46 samples (only next to really late frames). Game time stays within 8.3 ms
  of real time.

## Stutter while driving (investigated 2026-11-05, NOT fixed)
- Symptom: 60 fps most minutes, but some minutes 8-245 refreshes repeat a frame (busy city/race: 52-57 fps).
- Per-frame hitch logger (temporary, removed; patch kept only in that session's scratchpad): on late frames
  file reads, texture create/upload, GPU settle, render-thread sync, shader/pipeline compiles were all < 1 ms.
  Late frames = main thread's own work 12-17 ms + 5-9 ms of other guest threads holding the single baton.
  Per 30 s: main thread 17-20 s CPU, thread 7 4-7.5 s -> the baton is ~87% busy in heavy scenes.
- Thread 7 = EA sound mixer (CreateThread via 008daf40 -> 008a800c). `sample` (installed app has fn_/body_
  symbols) hot spots: 008ad12e, 008ad305, 008ab884, 008b70b0, 008abbb2, 008ac988 (DSP loops), plus
  interp_call for the heap-copied audio-bank routines (the "guest thunk fast-path miss" code runs interpreted).
- TRIED AND REVERTED: main-thread-first scheduling (main keeps the baton up to 12 ms, others run in the
  pacing gap), A/B in one run: better in light halves, WORSE in heavy ones (52.7-53.9 vs 58.1-59.9 fps) and the
  user's sound cut out (host audio sink late pulls 3 -> 445). The mixer must not be delayed.
- DONE (2026-11-06, committed; user chose to keep it, default on): mixer on its own core. RECOMP_FREE_THREADS="RWAudioCore Dac" (launch.c, setting
  core.nfsc/mixer_own_core, default on): the thread naming itself that (SetThreadName, guest thread 7) runs without the
  baton while it calls only imports.cpp free_ok imports (steady-state set measured: Enter/Leave/TryEnterCriticalSection
  ~4000/s, Sleep 100/s, timeGetTime, QPC, Interlocked*, ReleaseSemaphore, IDirectSoundBuffer GetCurrentPosition/Lock/
  Unlock ~200/s); any other import -> sched_free_leave (takes the baton). Translator: LOCK XADD / XCHG mem / LOCK
  CMPXCHG(8B) -> host atomics (x86.h atomic_*; exe has 30 XADD, 10 XCHG, 1 CMPXCHG); Interlocked* shims atomic;
  ReleaseSemaphore under g_sched_m; tramps() reserved to TRAMP_MAX; COM lookups via a lock-free ComIndex array; all
  DirectSound shims under g_dsound_m. Test 1: sound perfect but free halves 17-50 fps: the main thread blocking on a
  critical section the free mixer held slept in the host idle wait (HOST_IDLE_MAX_SLICE 50 ms, deaf to g_sched_cv).
  Fix: a baton thread waits on g_sched_cv holding the baton (<= 20 ms) when the owner is free running; a free thread's
  Leave/ReleaseSemaphore also calls g_idle_wake. Test 2 (light session, main 9-14 s CPU/30 s): both halves 60 fps,
  audio 0 late, clean exit; the mixer's baton CPU 2.7-3.4 s -> 0.3-0.8 s per 30 s. Test 3 (busy, 16 min, main up to
  17 s CPU/30 s): stable, audio 1 late pull (during the F10 pause), but late refreshes the SAME: 99 free vs 110 taking
  turns over 15 halves each (20-36 per half in the heaviest bursts in both). So the remaining stutter is the main
  thread's own heavy frames, not the mixer. ReleaseSemaphore's handle lookup moved under g_sched_m before commit.
  The A/B test switching (launch.c RECOMP_AB_PERIOD/SWITCHES, d3d9 "A/B half" lines) was removed; recomp_ab FREE
  stays for future A/Bs (RECOMP_AB_PERIOD=30 RECOMP_AB_SWITCHES=free). Log: "sched: guest thread 7 runs on its own
  core". Turn off: F10 Graphics "Sound mixer on its own core" (restart) or RECOMP_FREE_THREADS= (empty).
- Next for the stutter: per-frame logger again (now with the mixer out of the way) to see what the main thread does
  in the heavy frames.
- Next idea: make the mixer cheaper (native versions of the hot DSP loops, as MW 3.x's native/audio_mix.cpp did;
  translate the interpreted bank routines), not reschedule it. Main-thread kit work under the game's draw
  code (gpu_draw, put_matrices, apply_pass, queue_draw, dx_report_failure on a hot path) is the other target.

## Run the game (from Terminal, logs to a file)
```
cd ~/nfsc-mac/original
RECOMP_EXE="$PWD/NFSC.exe" \
RECOMP_PROFILE_DIR="$HOME/Library/Application Support/NFS Carbon Dev" \
RECOMP_LOG=2 RECOMP_IMPORT_STATS=1 RECOMP_MAX_SECONDS=90 \
~/nfsc-mac/build/CarbonRecomp.app/Contents/MacOS/CarbonRecomp > ~/nfsc-mac/run-log.txt 2>&1
grep -v '^\[recomp\] [-<]> ' ~/nfsc-mac/run-log.txt | awk '!seen[$0]++' > ~/nfsc-mac/run-summary.txt
```
LOG=2 logs every import call (100k+ lines); read run-summary.txt first.
`kit/tools/discover.py --game-dir ~/nfsc-mac` batches "unknown target" addresses.
