# Issue: findings from the 2006-06-21 preview disc (flag names, extra export names)

**Upstream:** LucasPicoli/god-hand-decomp, as an issue (findings, not a correction: their rule is an issue first for anything that isn't a match or a name). K's call to send it; it says these are our findings from probing the prototype disc.

## Title

```
Findings from the 2006-06-21 preview disc: names for the flag words at 0x747A78/80/84, and 89 extra export names
```

## Body

````markdown
We probed a preview build of the game, "PS2 - Godhand Preview 2006-06-21"
(boots SLPM_665.50, build string "Clover Studio-Jun 20 2006-22:41:04"), to
see what it could add. In case it's useful here, this is what we found. Its
ELF is stripped like retail's and its VU microcode is byte-identical to
retail's, so the useful part is names.

**1. The debug Flag Edit table names the flag words below ZeroMat.**

The preview's .data at 0x384808 is a table of 16-byte entries
{group name, bit names[], word address, bit count}: DBG (debug only, gone
from retail), STP, DSP, STA, SYS, PL, EM, SCE. The preview's STP/DSP/STA/PL
words sit at the same distance below ZeroMat (preview 0x719DE0, retail
0x747AC0) as these retail words, so they map 1:1. Bits count from the top:
name i is `0x80000000 >> (i % 32)` in word `i / 32`.

The names line up with what you already have where you've named a bit:
STP_EM = "enemies skip their update" (0x40000000 of D_00747A78), STP_CAM =
SYS_FLAG_CAM_LOCK, STP_KEY = KeyStop's 0x02000000, STA EVENT =
SYSFLAG_EVENT_RUNNING, STA ROOMJUMP = SYS_FLAG_DOOR_STEP, STA_WORLD_SLOW =
SYSFLAG_SLOWMO, and cDoor_setDoorJump sets STP_CAM | STP_VIB | STP_ACTBTN |
STP_SCEAT and DSP_ACTBTN. MOVIE_SYS_PLAYING (0x2000 of D_00747A84) has no
name in the preview (added later, presumably).

| Word | Group | Bits (mask: name) |
|---|---|---|
| D_00747A78 (flags5D8) | STP, "stop" | 80000000 STP_PL, 40000000 STP_EM, 20000000 STP_OM, 10000000 STP_EFF, 08000000 STP_SCR, 04000000 STP_VIB, 02000000 STP_KEY, 01000000 STP_PAD2, 00800000 STP_CAM, 00400000 STP_ACTBTN, 00200000 STP_CAMTHRU, 00100000 STP_SCEAT, 00080000 STP_MESS, 00040000 STP_COCKPIT, 00020000 STP_OBJ, 00010000 STP_SCE, 00008000 STP_DEFLICKER, 00004000 STP_WORLD_LIGHT, 00002000 STP_TIME, 00001000 STP_SHADOWCHKLINE, 00000040 STP_SE_RENEW, 00000020 STP_PLAYKEY, 00000010 STP_PLSTOPKEY |
| D_00747A7C | STP word 2 | 00100000 STP_SUBSCR |
| D_00747A80 | DSP, "display" | 80000000 DSP_PL, 40000000 DSP_EM, 20000000 DSP_OM, 10000000 DSP_EFF, 08000000 DSP_SCR, 04000000 DSP_FILTER, 02000000 DSP_COCK, 01000000 DSP_EMITTER, 00800000 DSP_OBJ, 00400000 DSP_KAGE, 00200000 DSP_PL01, 00100000 DSP_ACTBTN, 00080000 DSP_WORLD_LIGHT, 00040000 DSP_NPC_HPBAR |
| D_00747A84 (flags5E4) | STA, "state" | 80000000 COCKPIT, 40000000 SCENARIO, 20000000 EVENT, 10000000 TALK, 08000000 SUBSCR, 04000000 STA_ESP_DISP_EVENT_SST, 02000000 ROOMJUMP, 01000000 STA_SUSPEND, 00800000 STA_STAGE_START_JUMP, 00400000 STA_LV_MAX_FIX, 00100000 RELEASE_EM, 00080000 STA_EVENT2, 00020000 STA_WORLD_SLOW, 00010000 STA_ESP_DISP_EVENT_ONLY, 00008000 STA_DOOR_JUMP, 00004000 STA_TITLE, 00000800 SESCR_OFF, 00000080 WIPE, 00000040 SOFT_EVENT, 00000020 ROOMJUMP_NOWIPE, 00000010 SOFT EVENT CANCEL, 00000004 SCE MOVIE CANCEL |
| D_00747A8C | PL | 00004000 PL_FUDEMODEON |

So D_00747A80 is the display word rather than a lock word: a set bit hides
that part (the dog race and the jukebox set them to hide the world, the door
jump sets DSP_ACTBTN, func_002BF708 sets them all). Retail still reads most
of it. DSP_PL at 0x1020F8 / 0x107F28 (the sign test), DSP_EM in
12 enemy draw functions (0x2684A0..0x28E9D0), DSP_OM at 0x1B80C0, DSP_SCR
at 0x319848, DSP_FILTER in classCFILTER_TAG_Filter_main, DSP_KAGE in
cObjBase_KageDraw, DSP_OBJ in IsTargetVisible_14B470, DSP_PL01 in
IsTargetVisibleOrForced_1004C8, DSP_ACTBTN at 0x1F7360. The 0x200000 bit
that func_00307B28 (the hide-model manager) checks is STP_CAMTHRU.

We left the SYS group out: in the preview it sits at Sys+0x578, and our
mapping to D_00747A24 disagrees with your SYS_FLAG_NEW_GAME (0x10), so we
don't trust it.

**2. The preview's SNR2 export table has 89 names that retail's lacks.**

Most are debug-only (cToolMenu, cLog, dbClass, the debug camera, the debug
draw helpers), but some are functions that may still exist in retail under
other names: for example SetFpsMode__9cPlCameraP20SET_PL_CAM_FPS_PARAMb,
GetEmNear__9cEmManageG4cVecfffP4cObjUi (its code starts like retail's
cEmManage_GetTargetNear at 0x290A08), clearMotionCam__4pl00Ui,
setGetRange__7cOmItemf, Initialize__4SlotUcUs, MessageOFF__13cCockTutorial,
Tex_rtrans_sys__9classVRAM, SetPoisonHelp__5cEma2. The addresses are the
preview's, not retail's:

<details><summary>All 89 (preview address, mangled name)</summary>

```
0x102B30  _$_15cPl00ActionCtrl
0x122100  clearMotionCam__4pl00Ui
0x135680  setDbCamera__11cCamManagerUc
0x1356F8  endDbCamera__11cCamManagerUc
0x138848  SetFpsMode__9cPlCameraP20SET_PL_CAM_FPS_PARAMb
0x142680  MessageOFF__13cCockTutorial
0x1457E0  drawSkelton__6cModel
0x14AD28  ReleaseDataManager__4cObj
0x14ADA0  R0_Init__4cObj
0x14ADA8  R0_damage_pl__4cObjP4cObj
0x14C200  R0_Init__8cObjBase
0x14C208  R0_minigame__8cObjBase
0x14D3A8  Tex_rtrans_sys__9classVRAM
0x153BF0  MODF__Ff
0x15C128  Draw_Line_sc2__FffffUib
0x15C558  Draw_window__FfffffUi
0x15C5C0  Draw_MojiWindow__FffssUi
0x15CA58  drawBoxLine__FPC4cVecT0ffUi
0x15CE88  drawBox__FPC4cVecUi
0x15D0F0  drawTriangularPyramid__FRC4cVecffT0fUi
0x15D890  drawLocator__FRC4cVecf
0x15D8E8  drawLine3D__FRC4cVecT0Ui
0x1AFE50  setGetRange__7cOmItemf
0x1CCE40  Initialize__4SlotUcUs
0x1D2948  AreaDataDisp__C5cAreaUib
0x1D2998  AreaDataDisp__C5cAreaUif
0x1D39A0  draw__C12cBoundingBoxRC7cMatrixUi
0x1D4938  setGameDifficulty__9cCoreSavei
0x1D89E0  setFileAddr__12cDataManager6eObjIdPUi
0x244288  SetPoisonHelp__5cEma2
0x24A260  GetEmNear__9cEmManageG4cVecfffP4cObjUi
0x24DE78  updateSetDataPos__11cEmSetParamUiR4cVec
0x250200  getCutLengthCrnt__C6cEvent
0x25DFC0  isReleaseActiveHeap__5cGamei
0x25DFE8  resetReleaseActiveHeap__5cGame
0x2778A8  getJumpPointNum__9cRoomJumpUs
0x277A80  printJumpPointName__9cRoomJumpiiUsUc
0x27BE30  addList__13cSceAtManagerPUiT1
0x27C770  eventExec__9cScenarioUs
0x27DBE0  execShootingTask__9cScenarioPCc
0x281640  DrawArea__6cSescrUi
0x284D48  BgmStatusPrint__4cSndi
0x28D880  AdxVolumeAllSet__4cSnd
0x290318  kill__12cTaskManagerPv
0x2968D8  fileOpenWrite__7dbClassPCc
0x296908  fileWrite__7dbClassiPCvUi
0x296930  fileClose__7dbClassi
0x296B80  danger__4cLogPCce
0x296BD8  error__4cLogPCce
0x296C30  warning__4cLogPCce
0x296D08  modeReset__4cLog
0x296D50  modeSet__4cLogssss
0x297378  DebugMenuSetEnd__Fv
0x2984E8  DebugPrintRoomName__FiiUs
0x29B558  SetActionPickupWep__Fv
0x29B810  SetActionPickupObj__Fv
0x2B8D00  isEspToolBoot__Fv
0x2B8ED0  KillEventCutEffect__FUi
0x2C5C68  getEffDataType__F6eObjId
0x2C6D60  SetSstIdDispFlag__6espSysUib
0x2D3B80  _MemoryAlloc__FUiUiR5cHeapPCcUi
0x2E1EB0  _$_11PathFinding
0x2E2948  PlayerSet2__Fv
0x2E2DE0  CheckIn__Q25aslib7ProfileUl
0x2E2F18  CheckOut__Q25aslib7ProfileUl
0x2E4F80  move__9cToolMenu
0x2E5210  disp__9cToolMenu
0x2E53C8  setMenu__9cToolMenuPCcPFPv_bPvT2T3P9cToolMenu
0x2EE118  ADXPS2_StartPcmTrans
0x2EE130  ADXPS2_StopPcmTrans
0x38AEBC  SceRoomTableNum
0x38B290  Wipe
0x59A340  dbCamWork
0x5E9A70  g_Log
0x7265C8  EnemyHeap
0x726610  DebugCoreHeap
0x726628  DebugRoomHeap
(module)  ChangeIDDogSelectDisp__18DogRaceDogSelectIdUs
(module)  R043Init__Fv
(module)  R043Move__Fv
(module)  R502Init__Fv
(module)  R502Move__Fv
(module)  SetIDDogSelectBetGold__18DogRaceDogSelectIdi
(module)  SetIDDogSelectCursor__18DogRaceDogSelectIdUs
(module)  SetIDDogSelectDogName__18DogRaceDogSelectIdUsi
(module)  SetIDDogSelectOdds__18DogRaceDogSelectIdUsf
(module)  SetIDDogSelectTotalGold__18DogRaceDogSelectIdi
(module)  getCutLength__C12cEventCamerai
(module)  setStop__9cEventEnvb
```

</details>

**3. Debug messages name about 140 functions and 28 source files.**

The preview still has its error and assert texts. Many name the function
they sit in, as "Class::method" or "file.cpp->method", e.g.
cIDBase::trans / setIDData / getIDWork, cIDManager::getTexAddr,
cDataManager::load / loadWait / atEventload, cEmWrap::setAlphaMode /
setGoto / StartEscape, cCoreSave::setGameDifficulty, cRoomSave::setOmBreak,
cCollisionUtils::checkBoxLine, cEff3DPathMng::pushWork, espModSys::dataRegist,
cSndSub::LoadPartition, cSofdec::mwPlyGetStat, cOmset::readFile. The source
paths are D:/GodHand/prog/*.cpp (cDir, cDvd, cGame, cHeap, cMesh, cModel,
cMovie, cObjBase, cOt, cPs2, cSndSub, cSofdec, cTask, cWorldLight, esp03,
esp09, esp31, esp37, main, obj, sub02, ...). Matching those functions to
retail takes more than an exact byte match (the asserts are compiled out of
retail), so we haven't done it; the full list is yours if you want it.

The test tools the debug menu loads (dlltool/BugCheckDVD/RoomJump.rel,
FlagEdit.rel, EmSet.rel and so on) aren't on the disc, and the room modules
(dll/bugcheckhdd) are an earlier version of retail's.
````

## Before you post it

- Copy everything between the four-backtick fences as the body (the `<details>` block folds the 89 names on GitHub).
- The disc itself isn't shared: the issue gives names and addresses only, no data from it.
- Checked against the decomp at beb2575 (Oct 25). If they've named any of these words or bits since, trim that row.
- Our sources, if they ask: the flag table is at preview .data 0x384808 (pointer, pointer, word, count per group); the export table is the SNR2 block at the start of the preview's .sndata (0x3D2900: second table at 0x3DB094, 0x4E6 entries of name, address, info).

