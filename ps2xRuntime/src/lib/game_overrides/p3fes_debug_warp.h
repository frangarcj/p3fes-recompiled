#pragma once

class PS2Runtime;

// Debug harness: after a save has loaded, make the game itself change scene (field / Tartarus floor / battle)
// through its own sequence manager ("admini", adminiChangeSeq 0x27C080). Everything is inert unless one of
// these environment variables is set (read once at install time):
//
//   P3FES_WARP=<major>,<minor>[,<entry>[,<w3>[,<w4>]]]
//       field warp: adminiChangeSeq(2, {major, minor, entry, w3, w4}, 0x1C, 0). Fields are F<major>_<minor> in
//       DATA.CVM (e.g. 1,1 = dorm lounge, 6,1 = ...). <entry> is the spawn/entry-point id (default 0).
//   P3FES_WARP_TARTARUS=<floor>[,<arg>]
//       Tartarus warp: adminiChangeSeq(5, {floor, arg}, 8, 0) = K_Seq_CallDungeon; floor 1 = entrance hall,
//       2..399 = explorable floors (K_FldDungeon_IsCurrentFloorExplorable). Default arg 0.
//   P3FES_WARP_BATTLE=<encounter id>
//       starts an encounter: btlScrCmd_CALL_BATTLE (0x27D5E0) is called with its script argument 0 forced
//       to the id (scrGetIntPara hook), i.e. the game builds the battle sequence parameters itself.
//   P3FES_WARP_SEQ=<seq id>,<word>[,<word>...]
//       raw adminiChangeSeq(seq id, {words}, 4*n, 0) for experiments (numbers: decimal or 0x hex).
//   P3FES_WARP_AT=<seconds>           wall-clock delay after the first frame with a live field (default 5)
//   P3FES_WARP_BATTLE_AT=<seconds>    same for the battle (default: AT, or AT+25 when a field/Tartarus warp is set)
//   P3FES_WARP_LOG=1                  log every adminiChangeSeq call (seq, size, params, caller) and every
//                                     K_Field creation, even with no warp requested
//
// Menus (all use the game's own code paths, applied from the per-frame hook once the field is live + AT seconds):
//   P3FES_WARP_MENU=1                 unlock the command ("camp") menu: the opening of the game sets flag 0x171 and
//                                     the field-event task (0x1C8AE4) then ignores TRIANGLE; the flag is cleared
//                                     through the game's datSetFlag (0x16F1F0), every frame it is found set, so the
//                                     real TRIANGLE handler opens the menu (a save made with it keeps it unlocked)
//   P3FES_WARP_SAVE=<n>               open the save UI n times (default one): the game's script command 0x1885A0
//                                     (start save UI task), polled like its wait command 0x1885F0; the next UI starts
//                                     4 s after the previous one ended. Drive it with the pad (DOWN = next slot,
//                                     CROSS = save/dismiss). Writes the memory card (use PS2X_MEMCARD1 with a copy!)
//   P3FES_WARP_FLAGS=<flag>[=<v>],...  datSetFlag(flag, v) (v defaults to 1) once at AT, e.g. 0x171=0
//   P3FES_WARP_FLAGLOG=<flag>,...     log those flags every 2 s
//   P3FES_WARP_TASKS=1                every 2 s: the field-event candidate (gp-0x45FC) and every kwln task
//                                     (name, state, first word of its work area = its state machine)
//   P3FES_WARP_HP=<n>                 protagonist's current HP (unit datGetUnit(1) = 0x836224, u16 at +8) forced to n
//                                     (1..999) from AT seconds after the field is live, re-applied every frame it
//                                     differs (battle damage / healing are undone), through the game's own datCalcSetHp
//                                     (0x2FFD90). datCalcIsLowHp (0x30B5E0) calls "low" hp*100/maxHp < 26. Logs the
//                                     first application with the max HP read back.
// Shop: P3FES_WARP_SEQ=7,<shop id> (1 = Be Blue V accessories).
namespace p3fes_debug_warp
{
    // Installs the hooks when at least one of the variables above is set; returns true when it did.
    bool install(PS2Runtime &runtime);
}
