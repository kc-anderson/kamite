# Changelog

Newest first, one line per change; the code comments say how.

## October 30, 2026

- 60 FPS: enemies' own timers (flinches, waits, attack cooldowns, recoveries) run at the 30 FPS pace.
- Sprint attack traversal: a window marked on a thick wall covers its far side too; the first windows ship marked.
- Test cases carry the F2 switches (God mode, enemy cheats, developer switches, AI off) into their replays.
- 60 FPS: enemies no longer act as if slowed down all the time (the devil only taunting in front of Gene, guard
  and evade checks, voices).
- Sprint attack traversal: windows, any number per wall, marked in F2 > Debug > Stage collision; sprint + X / Y dive
  through them.
- 4:3 menus: the picture goes 4:3 once the fade to the pause menu is black, not while the game still shows.
- Modern controls: the camera swings round to the roulette's own angle while the wheel is up.
- Better shop & casino poses: the casino's bystanders come posed (the F2 NPCs tab's file still wins).
- Sprint attack traversal: unmarking a group removes its line from sprint_traversal.txt; the first railings ship marked.
- Sprint attack traversal (launcher > Gameplay, off by default): sprint + X / Y jump over and sprint + A slides
  through the railings marked for them (F2 > Debug > Stage collision, saved to sprint_traversal.txt).
- F2 > Debug > Stage collision: the room's walls and floors near Gene drawn over the picture, its named collision
  groups listed with switches, and a dump to the log.
- Modern controls: enemies right behind Gene but in the free camera's view attack and take their places around him
  like enemies in front of him (they stood idle).
- Modern controls: an enemy right in front of Gene (within point-blank range) fights back even when the camera is
  behind it.
- Launcher / F2: "Camera comes in" is now "Camera distance", the other way round (0% = Gene keeps his size, 100% = the
  zoomed-out view); saved settings carry over.
- Wide view: enemies the wider picture shows at Gene's side no longer hold back (the game's at-his-back hold and the
  per-enemy attack cones widen with the view, as the other enemy cones do).
- Skip intro also skips the Clover Studio movie.
- Roulette double timer: Gene's casting pose lasts until the timer runs out, and the highlighted move is used then.
- Styles: with F2 "Every technique owned" on, a style loads all its moves; turning it off removes the equipped moves
  the save does not own.
- Dev: F2 "Every roulette reel owned" lets every reel be set up for the roulette instead of putting them all on it
  (as in the preview build); "God Hand and roulette for free" no longer puts them all on it either.

## October 29, 2026

- Launcher: Skip intro (Gameplay): the game starts at the main menu, without the logos and the "Press Start" screen.
- Launcher: Play is selected when the title opens, so A / Enter starts the game at once.
- HUD: the gold counter sits under the minimap at both HUD layouts and grows to the left, its G lined up with the
  minimap's right edge.
- Modern controls: with the free camera turned away from Gene, enemies treat the camera's direction as his facing
  (those on screen act as enemies he faces, those off screen as enemies at his back).
- Dev: F2 > NPCs has an Enemies section: the pathfinding switch, AI off (enemies stand idle and take hits but never
  attack or walk) and Kill all enemies.
- Dev: F2 > NPCs > Target enemy: lock onto the nearest enemy, make it unkillable, and have it repeat one of its motions
  (with a pause between repeats, or once).
- 60 FPS: Door-kun's door swings open at the 30 FPS pace (it was twice as fast).
- 60 FPS: the frame-paced fixes keep their rhythm across save-state loads (a load could shift them by a frame).
- Dev: test cases record the commit they were built from.
- 60 FPS: every button-mash move takes the 30 FPS mashing pace (nine of them needed twice the speed).
- 60 FPS: Gene's spinning mash attack turns and lasts as at 30 FPS; his post-hit guard lasts as at 30 FPS in every state.
- 60 FPS: removed the old patch to Gene's attacks that the God Hand meter fix replaces.
- Dev: the "Record a test case" hotkey is named Right Stick Press (R3 in older ini files still works).
- 60 FPS: the God Hand meter lasts as long as at 30 FPS (it ran out in half the time), and its bar moves at the 30 FPS pace.
- Music: the right channel no longer jumps ahead with a crackle and plays a second copy of the track until it changes.
- Dev: replays also take a snapshot of the whole RAM once a second, for values that live outside Gene's object.
- Dev: KAMITE_ADX_DUMP records each stream channel's audio as decoded and as sent to the IOP
  (tools/audio/adx_dump.py reads it), for the music doubling.
- 60 FPS: several of Gene's timers in his attack and dash states count at the 30 FPS pace (they ran twice as fast).
- Dev: test cases keep what each pad read returned, so a replay reads exactly what the tester's game read; replays
  also dump RAM for finding frame counters.
- 60 FPS: after a hit, Gene (and objects like Door-kun) stay out of reach as long as at 30 FPS (it was half as long).
- Dev: test cases recorded with the Modern controls replay their camera and Gene's turns; the report compares fights
  per animation.
- 60 FPS: the dodges keep Gene's head and chest out of reach as long as at 30 FPS (they came back after half the time).
- 60 FPS: loading a save state no longer pushes the left stick by itself for a frame.
- Dev: test cases: R3 (the "Record a test case" hotkey) starts and stops a recording; one cases\<date-time>.case file
  per case; Replay-Cases.cmd replays every new case at 30 and 60 FPS and writes the comparison report.
- Hotkeys can be a controller button (R3, Select...) as well as a key; the launcher and the F2 menu offer them.
- Dev: a test mode (KAMITE_TEST) that loads a save state, records or plays back the pad frame by frame, logs the game
  per frame to CSV and quits by itself.

## October 28, 2026

- 60 FPS: Door-kun heals, shakes and swings his arms at the same speed as at 30 FPS.
- 60 FPS: the roundhouse hits Door-kun once, as at 30 FPS (it hit him twice).
- 60 FPS: all of Gene's timers run in real time: the dodges keep him out of reach as long as at 30 FPS (the duck
  was half as long), and the combo and stick input windows match too.
- 60 FPS: the enemies' shared timers (the enemy manager's countdowns) run in real time.
- 60 FPS: the freeze after a hit lasts as long as at 30 FPS, and Door-kun's arm hits once per swing.
- F2 > Debug > Hitboxes: no longer drawn over menus and the title screen.
- F2 > Debug > Room jump: the cut Floodgate room (r502) and a stale r601 point that dropped Gene outside the map are
  gone from the list.
- F2 > Debug > Developer switches: the game's own debug flags (invincible, free God Hand and roulette, every
  roulette reel, every technique, max gold, max health/tension upgrades, max combo length, no enemy or object
  placement, pathfinding off, no hit blur, no vibration).
- F2 > Debug: Dizzy at once (any stunning hit dizzies the enemy).
- F2 > Debug: God mode and Enemies can't die stop the killing hit itself; One-hit kills kill on the first hit.
- 60 FPS: the stick dash's double tap has the same time to land as at 30 FPS.
- 60 FPS: the button-mash meter fills and drains at the same mashing speed as at 30 FPS.
- F2 > Debug: slow motion (1/2, 1/4, 1/8), pause and step one frame; hotkeys Shift+F2 / Shift+F3 / Shift+F4.
- F2 > Debug: mute the game's sound (next to slow motion).
- F2 > Debug > Hitboxes: the damage shapes the game tests each frame drawn over the picture (attack volumes red, hurt
  volumes blue; inactive ones dimmed on request), with a one-frame dump of the shape list to godhand.log.
- The F2 menu is the dev menu: tabs Display, Graphics, Gameplay, Camera, Controls, NPCs, Debug; "Keep playing" is on
  by default. The launcher's options that can change while the game runs are in it (HUD layout, menu width, full
  screen; the renderer switches; 60 FPS, L3 dash, roulette timer, mute music; the control style); the ones that
  cannot are listed as "set in the launcher".
- 60 FPS mode switches in the game (Gameplay tab), and a save state made in one mode now loads into the mode that is
  set: the game's frame divider is written in both modes (a state made at 60 and loaded at 30 ran at the old,
  unfixed 60).
- Hotkeys: the port's shortcut keys (F2 menu, save and load state, FPS overlay, full screen, HUD layout, the dumps)
  are rebindable in the launcher's Controls tab and the F2 Controls tab, saved in kamite.ini [Hotkeys], with a
  warning when a key is also one of the game's controls. F1 stays the Controls screen.
- Controls tab: the Modern-only settings (Auto, L1, free camera, mouse) show only with Modern selected; the button
  prompt set (auto, Xbox, PlayStation, Nintendo, Steam Deck) switches there, redrawn at once.
- Gameplay tab: Styles is a fold-out section there (no Styles tab). NPCs tab: the poser is the "Pose NPC" fold-out.
- Debug tab: cheats for testing (God mode, enemies can't die, one-hit kills), the room jump, and Hide as a fold-out
  at the bottom; the hair sliders are gone.
- Fixed the boss health bar's background and gauge sitting at the 4:3 position while its portrait stayed at the
  screen's edge: a HUD group that starts at an edge keeps to that edge.
- Fixed the gold blinking after a style save or load lasting twice as long at 30 fps (it was counted in frames).
- The free camera glides back behind Gene 30% faster on L1.

## October 27, 2026 (10)

- Fixed the game hanging when a save was loaded after a save state with a character mod.
- Xavier fits the game's own memory for the player model again.
- Xavier: his chest sits further back (his arms clear it in the taunts); his shoulder pad is its own size again.
- Xavier: his body texture at full resolution.

## October 27, 2026 (9)

- Fixed a larger character file overflowing the game's player buffer on every load.
- Xavier: the snake's head at full detail, with its painted eyes, bolder side teeth and two proper fangs; no front
  teeth or tongue (the tongue showed through the shut jaw).
- Xavier: rows of upper and lower teeth show when he talks or shouts.
- Xavier: hand-painted fur where the gauntlet arm meets his shoulder.
- Xavier: shorter hair without its middle layer; the back hair no longer swings into his neck and necklace and shows
  from behind too.
- Removed Backward knees.
- Smaller character models (better triangle strips).

## October 27, 2026 (8)

- Fixed the crash on Play with a character larger than the game's file (one of the game's disc read paths did not
  see the grown archive).

## October 27, 2026 (7)

- Character models may be larger than the game's file (the port grows the disc archive the game sees; the image
  itself is untouched). The log reports the room the game has for the model when it loads.
- Xavier: more detail again (body, hair, mouth); the snake's head is now Gene's four hand meshes, so the game swaps
  it shut with the fist exactly as it swaps Gene's hand. Bone 31 is free again.

## October 27, 2026 (6)

- Xavier: hair physics (the back hair and side strands swing with his head, gravity and a gusting wind; F2 > Debug
  tunes them).
- Xavier: the snake's mouth closes to touching instead of biting through itself; the tongue stays between the jaws.
- Xavier: the loincloth no longer spreads wide while walking.

## October 27, 2026 (5)

- Xavier shows up again (the Oct 27 (4) model was 17 KB larger than the game's file and was left out: Gene with
  Xavier's loincloth); the encoder now refuses a model that does not fit.
- The snake's mouth swaps shut at once while the fist shows (no easing).

## October 27, 2026 (4)

- Fixed the save-menu hang after a save state load for real (the first fix used the wrong end code).
- Xavier: his boot is one piece (the upper no longer trails the shin); his jaw, lips and tongue follow Gene's jaw,
  so his mouth opens when Gene talks, with the mouth interior back; the snake shuts its mouth while Gene makes a
  fist. His loincloth panels sit 2 cm further from his body (XAVIER_LOIN_OUT).

## October 27, 2026 (3)

- Fixed the game hanging at 0 FPS when loading a save from the options menu after a save state load (an empty
  message text sent the text measurer off into memory; such a text now ends at once).
- F2 > Debug shows which hand meshes the game is drawing (fist / open / grab).

## October 27, 2026 (2)

- NPC poses: Butt wiggle (the pelvis undulation, on any motion) and Hip sway (the hips side to side) are separate
  sliders; Amount of movement scales the motion's own hip travel too. Old poses: the extra sway becomes wiggle.
- NPC poses: hands bend at the wrist (the motions turned the forearm's twist bone instead); female_idle_1's right
  hand no longer clips into the chest.
- Launcher: splash text no longer clipped or garbled; the font ships as a baked atlas (tools/splash/bake_font.py),
  not the .ttf.
- Shift+F8: hang dump (threads, PCs, waits) to the log and the EE RAM to hang_ram.bin.
- Tools: mot_rework.py --arm-out, --wrist.

## October 27, 2026

- Launcher: splash text (a random line of resources/splash/splash.txt at each start).
- NPC poses: motions dance, idle_fold_arms, female_idle_1, female_idle_2; counter_idle is the calm slow take;
  table_tease is counter_tease (old names still load).
- Xavier: fixed olive buttons and sleeve after a save state load; better 16-colour textures.
- F2 > Debug: Hide switches work.
- Tools: ual_motion.py, npc_motion.py biped rig, xavier/requantize.py.

## October 26, 2026 (17)

- **Precise depth: the casino fence's floor shadow** no longer breaks into shards. The shadow is its own mesh, up to
  a sixth of a depth step under the floor it lies on (the PS2 rounds both to the same step); its triangles now start
  a little above the floor and keep climbing through the frame, so a second shadow layer still lands on the first,
  never a whole step up.
- **Build:** the port's resources/ folder (motions, prompts, images) is copied at every build, not only when
  kamite.exe is linked again; a change to resources/ alone (the counter idle takes) now reaches the play folder.

## October 26, 2026 (16)

- **NPC poses:** three takes on the counter idle to choose from in F2 > NPCs (Motion): counter_idle_slow (the same,
  slower), counter_idle_calm (the quick back-and-forth calmed to a third, with a slow sway from side to side) and
  counter_idle_calm_slow (both). New tool: tools/models/mot_rework.py (slow, calm and sway an existing motion).

## October 26, 2026 (15)

- **NPC poses:** "Legs: turn" swings the legs round her middle while her feet keep pointing where she faces (before,
  the feet turned with them). It reaches 100 degrees each way: past 90 the feet pass each other and the legs cross.
- **Launcher, Gameplay:** Better shop & casino poses is the last setting; the F2 > NPCs tab no longer has its own
  switch (it says when the poses are off).
- **F2 > Debug** (a dev tool): hide parts of the picture with the game's own display switches (Gene, his coat,
  enemies, objects, stage, effects, shadows, screen filters, HUD, action prompt...), and jump to any room's jump
  point through the game's door jump (the jump table the game keeps on the disc; the developers' names in English).
- The game's flag words have their own names now (src/kamite/game_layout.h: game::Stp, Dsp, Sta), from the debug
  menu of a 2006 preview disc; docs/decomp/issue-preview-disc.md has the findings for the decomp.

## October 26, 2026 (14)

- **Precise depth: the casino fence's floor shadow** no longer bands. The pass-then-fail draws whose triangles lie
  over each other now keep their precise depth (whole steps had brought back the PS2's banding against the precise
  floor beneath) and draw their triangles in order, each 2/256 of a step above the one before, so a later triangle
  still wins the tie as on the PS2. Draws of more than 128 triangles keep the whole steps.
- **Steady idle camera** ([Video] SteadyIdleCamera, off by default; launcher: Video, and F2 > Camera): while Gene
  stands idle, the camera no longer rocks and shakes with his stomps. The player camera sees him where his idle began
  (moving off half a metre or turning 20 degrees sets a new place), and its shake is left out while he idles.
- **Launcher, Gameplay:** Style switching is now a setting ([Game] StyleSwitching, off by default; off, the port
  leaves the d-pad, the Techniques screen and the HUD as the game has them). New: Better shop & casino poses
  ([Game] NpcPoses, off by default; F2 > NPCs can switch it on for the session while setting poses). Roulette timer
  moved under Control style, Unlock Hard to the bottom.
- **NPC poses:** head tilt, the legs turned without turning her (the leg IK crosses or opens them) and how far apart
  the feet stand. npc_poses.txt gains three columns at the end; older lines still load.

## October 26, 2026 (13)

- **NPC poses:** two new motions ship with the port in resources/npc_motions:
  table_tease (bent over a table, hips swaying) and counter_idle (hands on a low counter). Only posed NPCs now take a
  slot (16 KB halves, 15 posed NPCs a room); the others keep playing the game's idle untouched, and an NPC set back to
  its own idle gets the game's motion again. Resampled curves never become constants, so a motion's records stay the
  same at any slider value.
- tools/models/npc_motion.py: --tolerance, --pelvis (how much hip travel and pelvis wiggle), --head-forward (the
  neck's and head's mean turn taken out); the hips' mean tilt goes into the waist and their mean heading is dropped
  (the legs stand under her, Turn sets where she faces); keys at most 255 frames apart; tiny values encode as zero.

## October 26, 2026 (12)

- **Precise depth: the casino fence's floor shadow** no longer comes and goes in patches. A draw with the alpha
  test's pass-then-fail emulation (the pixels that pass first, with depth, then the ones that fail, colour only) whose
  own triangles overlap (that floor layer lies over itself on 80,000 pixels) lost its ties to rounding: such draws
  keep the PS2's whole depth steps (and its flooring), one step up so they stay over the precise surface beneath.
- **NPC poses: every setting now shows, on every motion, without twitching or stuck arms.** Each NPC's motion has
  two halves: a change that alters the motion's records (another motion, the lean on or off) goes to the other half
  and starts there (the game ignored a restart of the same motion and kept its place in the old curves); slider moves
  are written in place while held and start the motion cleanly when let go (or half a second after the last wheel
  or arrow step). The head, arm and elbow curves are always resampled so a motion's records never change with the
  sliders. 30 NPCs per room.
- **F2 menu sliders** step with the mouse wheel over them and with left/right (keyboard or d-pad) while focused;
  Shift ten times, Ctrl a tenth.
- **Techniques screen:** the save hint reads "[Y] Hold + D-Pad: Save Style" with the list's plus.

## October 26, 2026 (11)

- **NPC poses: a choice of motion and more to set.** The NPCs tab has a Motion list (the built-in rail lean and any
  .mot file in mods/npc_motions next to the exe) and new sliders: head up/down and turn, arms raised or lowered,
  elbows bent, bottom out, and the amount of the motion's own movement. The head, arm and elbow settings turn the bone
  in front of the motion's own rotation, every frame (the curves are resampled), so they work on any motion.
  npc_poses.txt keeps the new values after the old ones (older lines still load). Slots are now 8 KB (60 NPCs).
- tools/models/npc_motion.py: an NPC pose motion from a .blend / .fbx / .glb animation (Mixamo, Quaternius
  and other rigs), with smoothing and slowing.

## October 26, 2026 (10)

- **NPC poses:** switching the lean on or off starts the NPC's idle again, so she no longer jerks about for a moment
  going in or keeps her arms out coming back (the game keeps its place in each curve while a motion plays, and the two
  motions' curves differ). Forward and sideways now reach 2 m, height 0.5 m.
- **Style indicator** a touch right and up (99, 346.5 from 97, 348).
- tools/models/export_glb.py: a game model as a rigged, textured .glb for animation tools (the bunny girl's three
  colour sets were made with it).

## October 26, 2026 (9)

- **NPCs who lean on their counter (F2 menu, NPCs tab).** Walk up to a bystander, tick "Lean on the counter" and
  set how far she bends at the waist, her hip sway, height, forward/sideways place and turn; changes show at once and
  Save writes npc_poses.txt next to the exe. The lean is Quaternius' rail idle (Universal Animation Library 2, CC0)
  retargeted to the game's women (tools/models/npc_rail.py); the legs and feet stay the game's own, placed by its leg
  IK. Every NPC that starts one of the casino's shared idles gets a copy of its own to play, so leaning one does not
  lean the others (the men share those idles). Nothing changes for an NPC that is not set.

## October 26, 2026 (8)

- **Precise depth: the banded lights and shadows (likely the cause of the far-corner stripes too).** The alpha
  test's second pass, which draws the pixels that fail the alpha test (a light's glow, the soft edge of a shadow),
  kept PCSX2's depth flooring when precise depth turned it off for the first pass, so those pixels lost their
  fractions and failed against the surface under them in bands along the depth: the cut-off light strips on the
  casino front and the town's awnings, the floor's shadow banding. Found with the draw dump.
- The draw dump now writes its pictures (the GS library's PNG saving was a stub) and adds _scene.png.

## October 26, 2026 (7)

- **Diagnostic: Shift+F10 draw dump.** Every draw of the next frame as PCSX2's GPU renderer gets it (after the
  port's depth and shading changes: vertices, depth scale, depth write/test, alpha-test passes) into
  drawdump_<tick>.bin, and that frame's colour and depth as drawdump_<tick>_rt.png / _depth.png. For the
  precise-depth stripes; to be stripped once they are fixed.

## October 26, 2026 (6)

- **See-through leaves characters alone again:** which packets may dissolve is now decided per XGKICK from the
  code that leads to it (the stage renderers with their own clipper: yes; the renderers whose clip test the port
  patches, characters and simple objects: no). It was decided per "program", and the game's renderers share a top
  and continuation branches, so one group could hold the stage renderers and a character's: the class then followed
  whichever character programs were loaded (Xavier dissolved when seen from above), and the "skinned" test also
  missed renderers that fetch no per-vertex matrices.
- **Precise depth: the far corner's stripes** are not fixed yet (K: still there; an attempt that gave batches
  with skipped corners precise depth was taken out again, it changed nothing).

## October 26, 2026 (5)

- **Casino: the 30 FPS view is back to 60.** At the top of the stairs, looking at the balconies, the game ran every
  frame twice: its VU1 work took 12 of the 16.7 ms on the PC, but the port also keeps the game waiting as long as
  the PS2's VU1 would need (the MTVU timing model, which keeps the game in step with its own hardware), and there
  that is more than one field. The trace showed the emulated VU1 still busy at every vblank, so the game held each
  frame for two. The 60 FPS mode asks the VU1 for twice the PS2's work per second, so it now gives the emulated
  VU1 half the time per program (as PCSX2's 60 FPS patches need its EE/VU overclock); the real VU work is still
  waited for. PS2X_VU1_TIMING overrides it.
- **Style indicator:** empty styles a darker grey, apart from the cream.

## October 26, 2026 (4)

- **Perspective shading fixed:** shadows vanished and some faces (the shop girl's) went grainy with it on. Draws
  with an alpha test run a second pass with the same vertex shader, and that pass's pixel shader never got the
  perspective flag, so it drew the colour times Q. Both passes divide now, and a colour that is the same at every
  vertex comes out exact (it used to land a hair under, which a truncating GS turns into one step lower). The
  see-through oval now also reaches the second pass.
- **Even frame pacing:** the trace showed vblanks 15 to 18.5 ms apart (60 per second on average): the timed wait
  woke up to ~2 ms late and the next frame caught up. The wait now ends 1.5 ms early and yields to the exact
  deadline (PS2X_PACING_SPIN_US, 0 = the old wait), so each frame lasts the same.
- **Style indicator:** saved styles show in the Techniques page's cream, not white (the colour was the on-screen
  value, which the GS doubles).

## October 26, 2026 (3)

- **Perspective shading (new, on by default; [Video] PerspectiveShading, F2 > Graphics):** the floor's shadow near
  the camera no longer goes darker and lighter as the camera turns. The PS2 interpolates vertex colours evenly across
  the screen, not along the surface, and the scenery programs clip big floor triangles at the screen's guard band
  with new vertices whose colours depend on where the edges fall, so the same patch of floor got a different shadow
  strength from each viewpoint (the warehouse: 98 vs 69 at one pixel). Colours of perspective-textured Gouraud
  triangles are now interpolated with perspective, like the textures, so a clipped piece shades as the whole
  triangle would. PerspectiveShading=0 is the PS2's look.
- **VU1: another 10% off** the casino's VU work (105 to 95 million instructions per frame, results identical): the
  generated code writes partial registers with one blend instruction instead of three plus a table load, and the
  normalisation of loaded values uses two blends instead of six logic operations.
- **Style indicator** a little further left, so the letters, the charges and the bars step round the level ring.

## October 26, 2026 (2)

- **VU1 microcode 44% faster (the casino):** the generator now leaves out the exact flag work for every FMAC whose
  MAC flags no instruction can ever read. A new analysis follows the program's control flow from each FMAC: if every
  path to a flag check (or to the program's end) passes a later FMAC at least four instructions before the check, that
  later one is what the check sees, so the earlier one needs neither the exact range checks nor a flag record. Folded
  FMACs inside a segment also skip the exact path when the program never reads the status register. On a traced
  casino frame the VU1 work went from 174 to 105 million instructions (20.5 to 11.4 ms on the test machine); every
  run of the frame gives bit-identical registers, VU memory and GIF output to before and to the interpreter.
  VU1_RECOMP_OBSERVABLE=0 and VU1_RECOMP_LIVE_FAST=0 (set for `build.py vu`) bring back the previous code.
- tools/vu/vu1_replay.cpp: an offline replay of F8 VU1 traces to check and time changes to the VU1 compiler.
- **Style indicator** moved next to the level meter's ring: the first letter now starts over the first roulette charge
  (it was centred over all six charge slots, far out to the right with only two charges).
- **Exit crash, properly:** the port now ends its process without the DLLs' unload (TerminateProcess after the log is
  flushed). The first fix kept the GS renderer and device alive, and the texture cache then crashed handing its
  palettes back to the device during the unload; the GS DLL now also leaves the texture cache alone if it is ever
  unloaded at exit another way.
- **Decals on precise depth:** a draw that writes no depth (a shadow or light laid on a surface) now always gets its
  one-step lift. It was skipped whenever a vertex of the draw's batch lay past the 16-bit depth range, which happens
  with the vertices of camera-crossing triangles the game culls: so whether such a decal striped depended on the
  camera.

## October 26, 2026

- **Style indicator in play:** the four style letters ("S E X Y"; a save's own names) stand above the roulette
  charges, right of the level meter, in the Techniques list's colours: the loaded style gold, the saved ones cream,
  empty ones dim grey; the loaded letter blinks a moment after a swap, a letter held with the D-pad blinks until it
  saves. Shown once a style exists. The text rides in two spare entries of the money display's layout and keeps with
  the level meter at the left edge in widescreen. The "Style X" corner notices are gone (only "No save data in memory
  yet" remains).
- **Casino speed:** the VU1 microcode analysis for the near-geometry filter ran again for every upload (several
  hundred a frame in the casino; a quarter of the VU thread's time) and is now cached by the microcode's content, and
  the analysis itself no longer searches the program for each branch. The "near geometry:" log line reports the
  cache's hits and misses.
- **No crash on closing the window:** the GS renderer's caches are left to the process on exit instead of being torn
  down in the DLL's unload (the main program ends with `_Exit`, and the DLL's statics went in the wrong order).
- **Floor-light stripes, second try:** draws that leave the depth buffer alone are given one whole depth step (not
  all but one unit) in front of the geometry they lie on, which is what the PS2's own coarse depth gives them; the
  first version's smaller offset left the stripes and made some shadows change with the camera's position.
- Diagnostics: the glow probe ("[glow]" lines) is removed (its finding is fixed); `KAMITE_PC_SAMPLE_ALL=1` samples
  every thread of the process (the VU, GIF and GS threads too), not only the game thread.
- **Floor light no longer falls into stripes in the distance** (the warehouse's floor shadow "banding"): the floor's
  light layer is a decal that keeps the depth buffer untouched through the alpha test (every pixel fails, frame buffer
  only), and the precise-depth code only recognised decals that do it through ZMSK, so against the floor's exact depth
  it passed or failed by rows where the depth steps are coarse. Such draws now get the top of their depth step like the
  others, and pass wherever the PS2 let them. (pcsx2gs.dll)

## October 25, 2026 (night, 3)

- **Style list:** the plus now touches its arrow and the name follows the arrow closely (the text draw's own icon
  advance is hooked as well; the measure hook alone only moved the block's centring), the block pinned by the names'
  right end; the line pitch is documented as it really is (a letter is the tallest code, not an icon).

## October 25, 2026 (night, 2)

- **Style list:** lines 16 px apart (the block's top line stays where it was); the plus starts right after the Y
  button, the arrow right after the plus, and the style's name right after the arrow (the plus and the arrows now
  advance the text by their own width instead of a full icon cell).
- **Hints follow their buttons closely:** the Techniques screen's "[A]Change" / "[A]Select" and "[Y]Hold D-Pad: Save
  Style" lose the gap after the button (as "[B]Exit" has it); the save and load menus' "[A] : Confirm" and
  "[B] : Cancel" become "[A]Confirm" and "[B]Cancel", the two buttons one above the other (the lines had kept the PS2
  layout's spacing for two buttons on the first line). The same strings in the pause menu's files are reworded too.

## October 25, 2026 (night)

- **60 FPS: effects keep the PS2's timing.** The effect system counts frames; the 60 FPS patch had halved only an
  emitter's own frame, so an emitter lived twice as long in game frames and emitted twice as many children while it
  did. The screen glows (the casino lights, the God Hand's arm) are such children that live until their emitter ends,
  so two stood on top of each other: far too bright, and grainy. Now the emitters' frames, their emission countdown
  and the children's frames all step by half a frame at 60 FPS, so every effect lasts and repeats as on the PS2. A
  log probe ("[glow]" lines) still watches the glow's births and deaths for one more round.
- **60 FPS: the Techniques screen's move demo plays at full speed again.** The 60 FPS patch halved the demo's motion
  step on top of the halving every motion already gets at 60 FPS, so Gene demonstrated in slow motion.
- **The shop's radio:** while its song list is open the camera stands still on the radio as on the PS2 (the Modern
  free camera turned with the stick), and Gene stays in the picture instead of vanishing (his update runs with the
  "player off" bits lifted for its duration; his controls are still off, so he stands and idles).
- **Style list polish:** the lines are closer together (19 px instead of 23); the "+" is the game's own plus from its
  "+arrow" art, drawn against the arrow as one piece (a third piece in the icon sheet's free cells, PS2 and full
  resolution) instead of the font's oversized plus; the drop shadow follows the same line pitch instead of drifting
  down line by line.
- **Launcher title:** the KAMITE wordmark stands still; only the painted kana (and the speed lines behind them)
  shift with the mouse, and nothing drifts on its own. `resources/images/kana.png` is the kana layer;
  `tools/logo/build_logo.py --layers logo.psd` rebuilds both images from the layered PSD.

## October 25, 2026 (evening)

- **Style presets, stage 2: swapping in play and on the Techniques screen, kept in the save.**
  - In play (Gene under your control, mid-fight and mid-combo too), a **d-pad tap loads a style**: up = S, right = E,
    down = X, left = Y (L, I, F, E as Xavier). The next move of a chain already comes from the new style. A small
    notice in the corner names the style for now.
  - On the **Techniques screen** (Modern menu buttons): **Y + d-pad** loads that style when let go before 1.5 s, and
    **held 1.5 s saves** the equipped moves to it. The d-pad doesn't move the cursor while Y is held.
  - The styles are **stored in the save** (in a part of the save data the game never uses), so they go to the memory
    card with it, each save file has its own, and a new game starts with none. A Continue keeps them.
  - The Techniques screen **lists the four styles** in the empty space right of the big TECHNIQUES title, one per
    line: [Y] + ↑ Style S, [Y] + → Style E, [Y] + ↓ Style X, [Y] + ← Style Y (reads SEXY; LIFE as Xavier). The arrows
    are drawn like the game's own "↓+" art, at the same size; the loaded style is gold, empty ones are dim, a style
    blinks while Y + its direction is held and again right after it loads or saves. The bottom bar's spare line says
    "[Y] Hold D-Pad: Save Style". The list steps aside while a move is being picked.
  - F2 > Styles shows which style is loaded and saves/loads them too.
- **The shop's radio (jukebox) is no longer a menu:** its screen has an invisible backdrop bigger than the screen,
  which the 4:3-menu detection took for a menu background, so the picture went 4:3 while it was open.
- **The shop window no longer gets garbage over it:** the game packs each frame's model textures into video memory
  by their size in bytes, but a narrow, tall texture (the jukebox's) takes more room than that, so the next texture
  (the window glass) was loaded partly inside it. On the PS2's 4:3 view the jukebox is off screen there; the wider
  view shows both. The game's texture slots now reserve a texture's real footprint.
- Fixed: digits in the port's rewritten menu hints used the wrong glyphs (none were in use).
- **Tutorial pop-ups are no longer menus:** with 4:3 menus in a 16:9 game, a tutorial pop-up's full-screen backing
  made the picture go 4:3 and back, and the health block could land in its 4:3 place. While the pop-up is up (and no
  pause menu is open over it) nothing counts as a menu. HUD pieces also keep their last place across any menu instead
  of starting from the middle for a frame.

## October 25, 2026 (style presets, stage 1)

- **Menu buttons moved to F2 > Controls** (a testing option until the whole game has been played with it) and **Modern
  by default**. It switches live both ways (the save menu's own wording follows at the next start); the launcher no
  longer has it.
- **Style presets, stage 1:** F2 > Styles shows the moves Gene has equipped (the six combo sets of the save data) and
  saves them to four styles (S, E, X, Y; L, I, F, E as Xavier) and loads them back, moves the save does not own left
  out. Kept in memory until the game closes; the Techniques screen's own controls and list come next.

## October 25, 2026 (title art)

- **The launcher has its logo:** K's cracked KAMITE wordmark with かみて painted in turquoise over its top right
  (`resources/images/logo.png`), much bigger than the old text title (as wide as fits, up to 44% of the window's
  height). Behind it, turquoise speed lines (`resources/images/burst.png`) in two layers that turn slowly in opposite
  directions and pulse; the mouse shifts the layers and the logo by different amounts (parallax), with a slow drift of
  its own when the mouse is still. The burst fades out on the Options screen. Both images are built by
  `tools/logo/build_logo.py` from the wordmark render and the brush kana (the sources are not in the repo).

## October 25, 2026 (late night)

- **Borderless fullscreen is a normal window again:** raylib's borderless mode also made the window topmost, so it
  stayed over every other app even after Alt+Tab. That flag is now cleared (at start and on F11).
- **Launcher Options reworked:** a line between settings, a setting's own sub-options in its section; F2/F7/F11
  hints on a "NOTE:" line of their own; "Show advanced settings" sits under Camera with the camera's advanced
  settings in its section; Menus is now "Menu width"; the HD texture pack is "Replacement textures" with its folder
  in the same section; Window and Window size share a section. New defaults: aspect ratio 16:9 (menu width 4:3, camera
  as on the PS2 as before). See-through moved to a new Experimental tab.
- **Near geometry follows See-through:** it only exists for the see-through, so it is on with it and off without (read
  at start); its own setting is gone.
- **Window size, Automatic:** follows the internal resolution (448 x InternalResolution high, at most 9/10 of the
  screen) instead of 3/4 of the screen.
- **Controls in the launcher:** Options > Controls has player 1's bindings, keyboard & mouse (two keys per button) or
  controller (one each): click a cell and press the key, mouse button, controller button, stick or trigger; right-click
  clears; one button resets everything to the defaults. The same controls.ini as the F1 screen in game, saved when
  leaving Options or the launcher.

## October 25, 2026 (night)

- **Button prompts at full resolution, with or without a texture pack.** The prompts come from Xelu's free prompt pack
  (CC0, in `resources/prompts/xelu`): Xbox 360 buttons for Xbox and other pads, Steam Deck buttons for a Steam Deck or
  Steam Controller, Switch buttons (Nintendo's letters) for Nintendo pads, key caps for keys and mouse buttons
  (the four direction keys as an inverted T); PlayStation pads keep the game's own art. Launcher Options > Controls >
  Button prompts (`[Game] ButtonPrompts=auto|xbox|playstation|nintendo|steam`) overrides the automatic choice.
  Controllers are now told apart as Xbox, PlayStation, Nintendo or Steam (SDL's type, Valve's own hardware by its
  vendor id, then the name). How: the port still writes its own small version of the two prompt sheets into the game,
  marked per art set in one unused CLUT entry so each set has its own texture name, writes a full-resolution image for
  that name into `cache/hd-textures` and hands it to the GS (`texture_add`, added at the next vsync). A texture pack's
  own prompt images no longer apply over the port's, and the port's images are not snapped to the game's alpha levels.
  With texture replacements off the small version shows.
- **Techniques screen:** its "Exit" hint shows the circle/B with Modern menu buttons (it showed the triangle/Y), and
  the pause menu's own screens count as menus for the widescreen layout even without a full-screen background (an
  open sub screen), so they are stretched or pillarboxed as a whole: the RETURN panel and its highlight no longer
  come apart.

## October 25, 2026 (evening)

- **Menu buttons fixed.** Modern did nothing in the menus: the command table was read with the pad's two button bytes
  the wrong way round, so the trade went to two d-pad entries (up and right) instead of the buttons. Modern now sets
  the menus' own OK and Cancel entries (command bits 28 and 29, the ones the decomp's matched menus test with the
  confirm and cancel sounds): cross/A confirms, circle/B cancels. The roulette is part of play and keeps the game's
  buttons and its own hint text in both modes (its bar is no longer reworded or re-laid out).
- **Texture packs and the port's own art.** PCSX2 finds a pack's image by the texture's content, so the textures the
  port changes lost the pack's images and showed the PS2's art at the PS2's resolution. The port now knows the pack's
  names (`hd_textures`, the same hash PCSX2 uses, checked against a pack's radar and prompt sheets):
  - a controller keeps the pack's own prompt sheets when the pack has them (full-resolution Xbox prompts in the
    common packs); the drawn letters are only used for keys, or with no pack;
  - the radar's glow is drawn at full resolution on the pack's radar (an image written into `cache/hd-textures` next
    to the exe at start, which PCSX2 searches too): yellower, brighter, centred on the disc, and without the PS2
    art's rope rings or the cone's shift. Without a pack the PS2-resolution glow got the same colour and centring.
- **Hang watch:** Windows flags the game window as not responding now and then while the game loop is pumping
  messages every frame. The watch took a full stack dump each time (138 in one session; the first also loads the
  symbols), which is what slowed the start. It now dumps only when the loop itself has stopped; the flag alone is
  one log line per session.

## October 25, 2026 (later)

- **The project is now Kamite.** The exe is `kamite.exe`, the settings file `kamite.ini`, the start script
  `Start-Kamite.cmd`; switches are `KAMITE_*` and the code lives in `src/kamite`. An existing `godhand.ini` next to the
  exe is copied to `kamite.ini` once on the first start (its switches renamed). The README is simplified; building and
  using the port is in `docs/BUILDING.md`.
- **Menu button text from the disc:** with Modern menu buttons, the reworded hints (roulette bar, Change/Select, the
  dialogs' Yes/No, the save menu's Confirm/Cancel) are written over the game's files as they are read from the disc,
  checked against the disc's own text first. The thread that searched RAM for the save-menu text twice a second is
  gone (it stays only as the fallback when a text is not where the disc should have it).
- **Tools:** `tools/models/mot.py` reads the game's motions and samples them at any time (ported from chihuahua's
  reader, checked against it), `tools/models/seq.py` reads and edits the motions' event tables (speed windows, sound
  cues, attack windows, effects), and the model reader understands room geometry.

## October 25, 2026

- **Menu buttons** (`[Game] MenuButtons=classic|modern`, launcher Gameplay > Menu buttons). Classic is the PS2's:
  circle and cross confirm, triangle cancels (the Japanese layout the US release kept: B/A confirm and Y cancels on an
  Xbox pad). Modern: the bottom button confirms and the right one cancels (A/B; cross/circle on a PlayStation pad), in
  the menus, the dialogs' choices, the roulette and the title screen only; the moves keep their buttons. The game
  reads the pad twice: play takes the raw button word, the menus a "command" word built from it through a table of
  masks (0x3C3D50, and 0x3C3E50 with the d-pad turned), so Modern exchanges the circle and triangle masks in the
  tables and nothing else. The prompt art keeps showing each cell's own button (a move slot's label is the button that
  throws it); the hint strings that name the menu buttons are reworded to the ones that now work them: the roulette's
  bar reads "RB Cycle / A OK / B Cancel" instead of the squeezed "Select / A B OK / Y Cancel", "[A][B] Change/Select"
  becomes A, the dialogs' "[B] Yes / [Y] No" become A / B, and the save menu's "Confirm / Cancel" (in its menu file,
  found by scanning RAM) likewise. The first cut of this traded the two buttons at the pad, which traded two attack
  slots too; that is gone.
- **The roulette's hint bar laid out again** (Modern wording): full letter width instead of the squeeze, its right
  end on the block's right edge (roulette_bar.cpp: the glyph and block extents of one frame set the text entry's scale
  and position in the next, before cIDBase_transText).
- **Controller letters in the look of the PS2 symbols:** A/B/X/Y as big as the symbols (18 px caps), bold, no black
  outline, lit along the top-left inside of their strokes and shaded along the bottom-right, as the rings and crosses
  are. The yellow ramp made of the square's pink entries is spread in brightness so the Y shades too.
- **Radar glow for the controls in effect** (Modern control style only): the radar's floor glows yellow-orange along
  the inside of its rim while the original controls are in effect (L1, Auto in a fight, the game holding the camera)
  and not while the free camera is on. The radar texture in the core block gets a second version of its texels and
  CLUT (eight little-used blip entries become the glow's ramp), written over it like the button prompts. Classic
  players see the PS2 art.
- **See-through is tagged experimental** in the launcher and the ini: the banded blur where hazy distance shows
  through the dissolve (the depth of field reads the dissolved depth) stays for now; DepthOfField=0 removes it.
- **Controller prompts in Xbox colours:** A green, B red, X blue, Y yellow, on the grey caps, as the PS2 art has coloured
  symbols on them (white letters read as a Switch pad). The letters are drawn from the sheets' own colour ramps; the
  pink square ramp, used by nothing else, is turned yellow in the CLUT for the Y. Keyboard caps stay white.
- **See-through oval: characters are never dissolved.** The VU1 filter tells the GS what each packet draws (a NOP
  register write carrying the class; nothing to the PS2): the skinned renderers (Gene, enemies, coats) are class 0 and
  the oval leaves them alone; only the stage dissolves. The first version keyed on the wrong renderers and dissolved
  nothing.
- **Near geometry: the behind-the-camera "vertex explosion".** A renderer's vertex loop has two exits (the plain one
  and the path that marks a stitched or clipped last vertex), each with its own XGKICK and [E]; the filter took the
  second one for another, unpatched program, and everything kicked there went to the GS uncut. A program is now the
  stretches between [E] bits united by the branches between them. The same renderer's stride instruction sat outside
  the window the layout search used, so it never matched its stitch flags either.
- **Window hang diagnostics:** when Windows reports the game window hung, the log now also shows whether the main loop
  is alive (iterations, time since its last message pump), whether the window's thread answers a message, whether
  user32's PeekMessage/GetMessage are hooked by another module, and the modules loaded from outside Windows and the
  game folder; a second look 15 s later. Windows no longer replaces the window with its "Not Responding" copy
  (`PS2X_WINDOW_GHOSTING=1` restores that).
- **Near geometry: no more streaked walls.** The previous build told the filter which program sent a packet by where
  the VU1 run started; the scenery renderers end with [E] and a branch back to their top, and the game continues them
  with MSCNT from there, so their packets counted as another program's and went to the GS uncut: triangles reaching
  off the screen range wrapped across it (dump gif_8136: 23 such triangles drawn). The filter now gets the address of
  the XGKICK itself (interpreter and ahead-of-time VU1 code; `build.py vu` regenerates it).
- **"Not responding" watch:** the earlier silent "crashes" were hangs (Windows: "stopped responding and was closed"):
  the window's thread stops taking messages while the game runs on. When the window has not answered for ~7 s, every
  thread's native stack is written to the log once (`hang:` lines).
- **Near geometry: floors no longer flicker.** The filter also cut triangles from the game's big renderers, which clip
  by themselves: a triangle they skip near the camera they draw again as a clipped fan. The cut copy and the fan lay on
  the same floor and fought in depth ("shadows" flickering in and out; dump gif_1652). Only packets from the renderers
  whose clip test was patched are touched now: VU1 tells the filter where the run that kicked a packet started
  (`ps2_vu1_hooks.h`, programPc), and the upload hook marks which programs it patched. Non-finite positions are dropped
  instead of cut.
- **See-through oval, rebuilt in the world:** the renderer gets the camera (eye, inverse of its world-to-screen matrix)
  and rebuilds each pixel's place in the world. What lies inside the oval and more than `SeeThroughMargin` (now 0.5 m,
  was 1 m with a 0.8 m ramp) nearer the eye than Gene dissolves, fully 0.3 m further. Walls just in front of him now
  open up even with the camera far behind them (the "bubble"), and the ground below his middle (floors, steps, table
  tops) never dissolves: the floor before his feet used to dissolve in stripes of dither.
- **Crash reports for silent crashes:** a crash that skips the in-game reporter (a fail-fast, heap corruption) is still
  recorded by Windows; at start the game reads the last three days of kamite.exe crashes from the Application event log
  and writes them to the log (code, module, offset, and the function when it is this build). Heap corruption and stack
  overflow are also logged as they are raised (src/kamite/crash_history.cpp).

## October 24, 2026

- **Walls stay, the oval dissolves them:** the walls that vanished whole near the camera were the game's own doing: its
  hide-model manager (func_00307B28) casts rays from the camera to Gene and to either side of him every update and hides
  every mesh a ray hits. With the see-through oval on, that hiding is off (patch at 0x308650, flag `keep_walls`,
  switched live with the oval) and the walls dissolve round Gene instead; with the oval off, the game hides them as
  on the PS2.
- **Near geometry:** a corner behind the camera has a garbage fog value (computed from its depth, then clamped); cut
  pieces now take the nearest corner's fog in front, so floors near the camera no longer flicker darker.
- **Near geometry** (`[Video] NearGeometry`, on; launcher Graphics; read at start): walls, pillars and floors next to
  the camera are drawn instead of vanishing. The PS2 cannot draw a triangle with a corner off its 16-bit screen range or
  behind the camera, so the game's scenery renderers drop such triangles whole, and a wall the camera comes close to
  disappears (it was never hidden on purpose: its triangles are sent every frame, then dropped). Their clip test is
  patched to the far plane only (in VU1 micro memory as each upload lands, and in the uploads compiled ahead of time),
  and every PATH1 packet goes through a filter that cuts what the GS cannot take from the full 32-bit positions the
  packet still carries: along the screen range (exact: everything the GS interpolates is affine on screen), and at the
  near plane in clip space for triangles reaching behind the camera. The game's strip stitching is read from each
  batch's input in VU1 memory, so stitches stay out and back faces are recognised (src/kamite/near_geometry.cpp;
  runtime hooks in ps2_vu1_hooks.h). F2 > Graphics shows its counters, and the log a line every 200,000 packets. A triangle with a corner between the camera and the near plane is cut there too (its depth runs past the 16-bit range: clamped, whole surfaces flickered in front of others).
- **See-through oval:** both radii at the same scale (it was squashed when the camera looked down); default half-height 2.2 m.
- **See-through** (`[Video] SeeThrough`, on; launcher Display; F2 > Camera sizes it live): walls, pillars and stalls
  between the camera and Gene dissolve in a soft-edged oval round him (an ordered dither, densest in the middle), so
  he and the enemies about him stay in view. Only what is more than `SeeThroughMargin` (1 m) nearer the camera than
  Gene dissolves; he, the enemies round him, the floor at his feet and the HUD are untouched. The game projects Gene
  with the camera's own matrix every frame (camera.cpp, hook in cCamera_update) and the renderer dissolves the
  depth-tested draws (pcsx2gs `see_through`, `GSRendererHW::ApplySeeThrough`, tfx.fx `PS_SEE_THROUGH`). Off in
  cutscenes.
- **Radar follows the Modern free camera:** while the free camera has the view, the radar turns with the camera
  (up = where it looks) instead of with Gene's facing (hook in func_00143D20, where it copies his rotation).
- **Precise depth keeps the PS2's ties:** draws that test depth without writing it (shadows, decals, glows laid on a
  surface) passed on the PS2 wherever their integer depth tied the surface's; against exact depths they passed or
  failed pixel by pixel (the roof beams' shadows on the town's walls flickered). They now get the top of their
  integer step.
- **Modern controls: mouse camera** (`[Game] MouseCamera`, on; `MouseSensitivity` 0.1 degrees per pixel; F2 >
  Controls): the mouse turns the free camera. A click in the game captures it, Esc frees it. It never reaches the
  game as the right stick, so it cannot dodge.
- **Precise depth** (`[Video] PreciseDepth`, on; F2 > Graphics live): the jagged, shifting slivers where distant
  surfaces meet (a pillar against a ledge) are the game's own z-fighting: its 16-bit depth falls as 1/distance from a
  near plane at 0.1 m, so a depth step is 6 cm at 20 m and 55 cm at 60 m (the same in PCSX2; under a pixel at the
  PS2's resolution). The renderer now takes the depth of perspective-textured surfaces from their Q (1/w, a full
  float), which the projection makes an exact line of the integer depth, keeping 8 bits of fraction. (A first try
  moved the near plane out instead; the game culls what comes nearer than it, so walls and floors near the camera
  vanished. Reverted.)
- **Walls no longer vanish near the camera, and things behind them no longer show through** (with a wider field of
  view): the game's depth range was put back for the player camera's projection only, but part of the scenery is
  drawn with a second projection built from the same widened view, so the two disagreed by the field-of-view factor
  and a near wall could lose to a cabinet behind it. Every projection keeps the game's own range again (the wider
  view shrinks it, as before); precise depth makes up the precision.
- **Modern controls:** Auto's fight range 3 m; free camera 200 degrees a second by default; it can look further up,
  as far as keeps it 0.35 m above Gene's feet.
- **Control Style: Modern** (`[Game] ControlStyle=modern`, Gameplay, F2 > Controls): the right stick turns a free
  camera round Gene and the left stick moves him relative to the camera (he faces where he walks). The original
  controls (tank movement, right stick = dodges, the camera behind Gene) while L1 is held (or toggled with
  `ModernL1=toggle`), and with `ModernAuto` in fights: from the battle camera coming on until no enemy has been within
  3 m for 1.5 s. Double-tap L1 = the original quick turn, the camera staying behind Gene through it. Whenever the game
  takes the camera (grab and special-move close-ups, cutscenes, menus) the controls are the original ones.
- **Depth of field switch** (`[Video] DepthOfField`, Graphics; F2 > Graphics switches it while playing): the game's
  distance blur is composited from a quarter-size copy and mask of the picture, which at high internal resolutions
  shows as dark, jagged, shimmering fringes along the edges of distant geometry. The renderer takes the switch live
  (`pcsx2gs_set_option`).
- **Enemies follow the wider view** (`[Game] EnemiesFollowView`, on by default; Options > Show advanced settings):
  the angles round Gene in which enemies close in and lunge widen by as much as the picture reaches further round him
  than the PS2's did (`docs/enemy-ai.md`), so enemies at the edges of a wide view fight as they did at the PS2's
  edges. Nothing changes at 4:3 and 100%.
- **F2 in-game menu, Camera tab:** field of view, field of view in fights, camera distance and tilt, the enemy
  setting and the presets, all applied while playing; "Keep playing" leaves the controller and keyboard to the game
  and the mouse to the menu; Save writes kamite.ini and shows the values as a preset line.
- **Launcher:** Display > Camera offers presets ("As on the PS2", "Wider", "Widest", "Action"); "Show advanced settings" shows
  the camera's own values.
- **Field of view** (Display > Field of view, `[Video] FieldOfView`, 100-150%): the gameplay camera's own fov is
  scaled where the game eases it toward its target, so the projection and culling follow; cutscenes keep their
  framing. **Field of view in fights** (`FightFieldOfView`, default 100%): the same while the game's battle camera is
  on, so fights are framed as on the PS2 by default; the camera eases between the two. **Camera distance**
  (`CameraDistance`, default: comes all the way in) moves the camera toward Gene as the view widens so he keeps his
  size instead of the view looking zoomed out; **Camera tilt** (`CameraTilt`, degrees) raises it to look down at him.
- **How enemies pick their moment** is written up in `docs/enemy-ai.md`, from the code: a queue by distance to Gene,
  a wedge of 60 degrees either side of the way he faces, lunges inside a cone that widens with the level gauge, and
  Hard as the gauge locked at Level DIE. No camera test is involved, so widescreen and the field of view do not
  change how enemies fight (an earlier note here said the game's on-screen test decided it; that test only decides
  what is drawn).
- **HD packs:** replacement textures keep to the alpha levels of the game's own texture (no more see-through enemies
  or grainy distance haze with packs made from edited dumps).

## October 23, 2026

- **The project is Kamite**, on its own `main` branch; the launcher and window carry the name, and the
  launcher draws `resources/images/logo.png` when it is there.
- **Function names from the decomp:** `recomp/kamite_functions.csv` carries the decomp's names (4,255 of the 10,553
  functions; `tools/symbols/import_decomp_names.py` refreshes them), the generated code is named after them, and the
  crash log, the hang dump, the PC sampler and the call profiler print `cPlCamera_update+0x1c` instead of an address.
- **Post effects option** (Graphics): the game's blur and heat-haze passes at the PS2's resolution (PCSX2's fix, grainy
  at 3x and up) or upscaled.
- **Xavier:** the arm stump is capped, Gene's gauntlet arm is clipped exactly where it enters the stump and its skin
  fades in from the fur colour, the gauntlet stays under the shoulder pad, the cheeks sit inside the loincloth, the
  hair strips no longer z-fight (lifted off the scalp and each other), the hair a little shorter and off the neck.
- **Playable characters:** `[Mods] Character=gene|xavier`; Xavier (from *Xavier: Renegade Angel*) in place of Gene,
  with Gene's gauntlet arm and his loincloth on the coat's cloth physics, full-resolution textures through the
  renderer's replacement system. Character files are written over the game's at disc level (the AFS table is never
  changed: a changed size broke later files).
- **Launcher** in the game window: Install (pick and check the disc image), Play, Options (Display, Graphics, Gameplay,
  Mods, Controls), "don't show again".
- **Unlock Hard** and **mute music** options. **Roulette**: d-pad scrolls the God Reel, up steps back with the
  animation following, `RouletteTimer=double` for twice the time.
- **Menus** drawn as on the PS2 (4:3 with side bars) or stretched; the in-game HUD at the screen edges (16:9 layout)
  or where the PS2 had it.

## October 22, 2026

- **PCSX2's hardware renderer** (Direct3D 11, `pcsx2gs.dll`) is the default, with the game's PCSX2 GameIndex fixes.
- **VU1 microcode compiled ahead of time** per upload (`build.py vu`), captured uploads included; MTVU on by default.
  The in-game slowdowns of the first builds are gone.
- **60 FPS mode** (`[Game] Fps60=1`): a port of the "60 Fps" PCSX2 patch as switchable recompiler patches, with the
  places where the port's timing differed from PCSX2 fixed (a stuck left stick, the roulette timer and dial, HUD and
  menu animation speed, stick-wiggle and scrolling-arrow prompts).
- **L3 dash** (`L3Dash=1`): clicking the left stick dashes (a quick double tap forward), also while walking.
- **Any aspect ratio** (`[Video] AspectRatio`): a port of the PCSX2 widescreen patch plus a HUD layout system that
  places each on-screen group per screen third; low-health corners and other screen-edge overlays stay stretched.
- **Crash log** for normal runs (exception, module and offset, guest pc, host stack), a PC sampler
  (`KAMITE_PC_SAMPLE`), a call profiler (`KAMITE_CALL_PROFILE`), a counter finder, HUD frame dumps (F6) and the RAM
  dump (F8).
- Window opens at a usable size; SDL3 controllers on; build fixes.

## Before the fork: LasVola's godhand-recomp (to October 20, 2026)

The entries below are from the project Kamite was forked from.

## October 20, 2026

- **Minigames show the proper buttons:** the crates in the pirate ship cannon minigame (stage 3) showed the PS2
  symbols. They now show your bound keys, or A/B/X/Y on an Xbox-style controller, and switch the moment you change
  between keyboard and controller (`src/kamite/crate_prompts.cpp`, `src/kamite/button_prompts.cpp`). A PlayStation
  controller keeps the original symbols.
- **Sound cutting out in stage 3:** the same kind of gap as in stage 2 (the streamed music reaching the sound
  processor late). Data transfers to the sound side are now reported finished after a short, PCSX2-like delay
  (`Kernel/Stubs/SIF.cpp`). Not heard in the next play session. New log lines (`[sif-dma]`, `[spu2-adma]`) show it if it comes back.
- Note for developers: `KAMITE_STALL_DUMP` pauses the game itself for up to about 1.5 s each time it fires (it loads
  the debug symbols). Use it only to catch a freeze, not during normal play.

## October 19, 2026

- **"Heat wave" ripples in cutscenes** (stage 3): the game's depth-of-field blur on the far background was drawn
  wrong (wavy bands at 1x, sharp with ripples at 3x). It now matches the PS2: an ordered path for the game's
  in-place depth shuffle, and the blur passes keep their original resolution (`gs_gpu_backend.cpp`).
- **Freezes up to about a second** when the game read from the disc: the port reopened the whole disc image for
  every read. It now stays open, and the image is loaded into memory in the background at start
  (`Kernel/Stubs/Helpers/Support.h`, `src/main.cpp`; `KAMITE_ISO_PREFETCH=0` turns the background load off).

## October 18, 2026 (evening)

- **Sound cutting out** (tiny gaps in music and voices, worst in stage 2 and the God Reel): the port told the game
  that every data transfer to the sound processor was still busy for one frame too long, which slowly starved the
  streamed music. Fixed in the SIF DMA status (`runtime/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp`).
- **Grid of lines over the sepia flashback cutscenes** at 3x: some 1:1 copies were filtered slightly differently from
  the PS2. They now read pixels exactly like the PS2 does (`gs_gpu_backend.cpp`, `gs_sprite_rules.h`).
- **Thin line under movies** at 3x: the movie picture was filtered past its own edge. Fixed the same way.
- **The game could close while saving** after about 30 minutes of play: a small memory area for game threads was
  never given back. It is now reused (`EeScheduler.cpp`).
- **Button prompts show your bound keys** (for example "F" instead of the PS2 circle), or Xbox-style names with a
  controller (`src/kamite/button_prompts.cpp`).
- **Controls:**
  - Start/Select no longer reach the game while you press Back + Start for the Controls screen.
  - Alt+Tab and similar shortcuts are no longer recorded as a key while rebinding.
  - Keys bound to two buttons are flagged on the F1 screen.
  - The Windows Sticky Keys popup no longer appears while playing.
- **Rare missing 3D pieces** in busy scenes: long-running VU1 programs are now finished instead of cut off.
- **The ending credits cannot hang** if a movie ever stalls (a 20-second safety check).
- Diagnostics for developers: `KAMITE_STALL_DUMP=<ms>` prints every thread's stack when the game stalls.

## October 18, 2026 (morning)

- **Boss "CHECK" freeze** (stage 1), second cause: a "stop sound" request could interrupt the sound mixer. Such
  requests now wait for the sound driver, as on a PS2.
- **Slow cutscenes:** missing ahead-of-time VU1 programs are now logged and saved to `logs\vu1_missing\`.
- Caps Lock / Num Lock bindings follow the key, not the keyboard light.
- The launcher keeps the previous session's log.

## October 17, 2026

- First public release: playable through stage 1. Fixes for boot, pad input, graphics speckle, sound effects,
  doubled audio, the floor rendering, the boss "CHECK" freeze (first cause), and the in-game Controls screen.
