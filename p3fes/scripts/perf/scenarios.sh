#!/bin/sh
# Scenario rig for the Mac build of the Vita native path: reaches a game situation by a scripted pad, captures it,
# and profiles it while it is held on screen.
#
# usage: scenarios.sh NAME[,NAME...]|all OUT [VAR=value ...]
#   NAME   lounge dialogue load-menu lockers hall desk corridor                       field scenes (pad only)
#          title title-menu title-config                                              title screens (pad only)
#          camp (alias pause-menu) camp-skill camp-item camp-persona camp-equip camp-status camp-slink camp-system
#          camp-calendar camp-quest camp-fusion camp-dictionary camp-config           command menu + sub-screens
#          save reload                                                                save UI + a real save / load of it
#          shop shop-buy shop-sell battle battle-skill tartarus                       sequence warps (p3fes_debug_warp)
#          (`scenarios.sh list x` prints this text; `all` runs every one of them: ~45 min)
#   OUT    output directory; per scenario NAME it gets
#            OUT/NAME.log OUT/NAME.sample OUT/NAME/frame-<s>.p3fb + .png   (captures, s = seconds since first frame)
#            OUT/NAME.report                                                (fps, busy share, top functions)
#          and OUT/summary.txt collects the headline of every report.
#   VAR=value  forwarded to the game process.
# Environment: JOBS=n runs n scenarios at once (default 1: parallel runs disturb the fps/busy numbers).
#
# Memory cards: EVERY run uses a private COPY of game-mac/memcards/Mcd001.ps2 (or Mcd001.lockers-save.ps2) through
# PS2X_MEMCARD1, so the user's cards are never opened; the originals are checksummed before/after.
# Without PS2X_MEMCARD1 the runtime opens <elf dir>/memcards/Mcd001.ps2 itself (ps2xIOP iop_emulator.cpp), i.e. the real
# card: a plain mac_run.sh run is only safe while nothing writes the card.
#
# Timeline facts (measured, 30 fps game, real-time pad clock):
#   pad clock t == "seconds since first frame" of P3FES_FRAME_DIAG; the process ([perf] t=) clock is t + ~5.5 s.
#   START x4 (20..29 s) passes the logos, CROSS at 34 picks "The Journey", 36 LOAD GAME, ~39 the slot list,
#   40 slot 1, 42 "Yes", ~44 "Load successful", lounge visible and controllable from ~48 s.
#   The command ("camp") menu opens with TRIANGLE, but the opening of the game sets flag 0x171 (datGetFlag, bit array
#   at 0x83A21C) and the field-event task (K_FldEvent_UpdateFldEventTask 0x1C8AE4) then ignores the button: the 4/7 save
#   has it set. P3FES_WARP_MENU=1 (p3fes_debug_warp.cpp) clears it through the game's own datSetFlag, after which the
#   real TRIANGLE path runs. The save UI is started with P3FES_WARP_SAVE=1: the game's own script command 0x1885A0
#   (the one the calendar script uses), then the pad drives it like any other screen. The pad's face buttons are the
#   game's own (TRIANGLE opens, CROSS = OK, CIRCLE = back); D-pad presses must be short (0.1 s) or they auto-repeat.
#   `reload` needs `save` to have run first (it loads slot 2 of OUT/cards/save.ps2, the saved copy) and logs flag 0x171
#   (0 = slot 2, saved with the menu unlocked; 1 = the original slot 1).
#   The menu screens are held for the sample window; all camp-* take ~100 s each (the game boots in ~48 s).
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)

if [ "$1" = list ] || [ $# -lt 2 ]; then
    sed -n '2,/^here=/p' "$0" | sed '$d'
    exit 0
fi

names=$1; out=$2; shift 2
case "$names" in all) names="lounge dialogue load-menu lockers hall desk corridor title title-menu title-config camp camp-skill camp-item camp-persona camp-equip camp-status camp-slink camp-system camp-calendar camp-quest camp-fusion camp-dictionary camp-config save reload shop shop-buy shop-sell battle battle-skill tartarus" ;; esac
names=$(echo "$names" | tr ',' ' ')
mkdir -p "$out" || exit 1
out=$(cd "$out" && pwd)
cards="$root/game-mac/memcards"
OFFSET=5.5          # process clock minus pad/frame clock

# --- parent: run the children (one per scenario, JOBS at a time) and check the original cards
if [ -z "$P3FES_SCEN_CHILD" ]; then
    before=$(shasum "$cards"/*.ps2 2>/dev/null)
    if [ "${JOBS:-1}" -gt 1 ]; then
        printf '%s\n' $names | P3FES_SCEN_CHILD=1 xargs -P "$JOBS" -I{} sh "$0" {} "$out" "$@"
    else
        for n in $names; do P3FES_SCEN_CHILD=1 sh "$0" "$n" "$out" "$@"; done
    fi
    after=$(shasum "$cards"/*.ps2 2>/dev/null)
    if [ "$before" = "$after" ]; then echo "original memory cards unchanged"; else echo "!!! ORIGINAL MEMORY CARDS CHANGED !!!"; echo "$before"; echo "$after"; fi
    echo "--- $out/summary.txt"; cat "$out/summary.txt" 2>/dev/null
    exit 0
fi

name=$names
# std prefix: logos, title, Load Game, slot 1 -> lounge from ~48 s
std() { python3 -c "print(','.join([f'{t}:START:0.3' for t in (20,23,26,29)]+[f'{t}:CROSS:0.35' for t in range(34,47,2)]))"; }
only_menu() { python3 -c "print(','.join([f'{t}:START:0.3' for t in (20,23,26,29)]+[f'{t}:CROSS:0.35' for t in (34,36)]))"; }
# camp_seq IDX [SUB]: pad script that opens the command menu (TRIANGLE at 56 s) and enters entry IDX (0 skill, 1 item,
# 2 persona, 3 equip, 4 status, 5 s.link, 6 system), then SUB of the system list (0 calendar 1 quest 2 fusion
# 3 dictionary 4 config); prints "script ready_second"; no IDX: only the menu.
camp_seq() { python3 - "$@" <<'PY'
import sys
a = [int(x) for x in sys.argv[1:]]
ev = [(56, 'TRIANGLE', 0.2)]
ready = 62
if a:
    idx = a[0]
    ev += [(62 + 1.3 * k, 'DOWN', 0.1) for k in range(idx)]
    sel = 62 + 1.3 * idx + 1.5
    ev.append((sel, 'CROSS', 0.2)); ready = sel + 6
    if len(a) > 1:
        base = sel + 7
        ev += [(base + 1.3 * k, 'DOWN', 0.1) for k in range(a[1])]
        sel = base + 1.3 * a[1] + 1.5
        ev.append((sel, 'CROSS', 0.2)); ready = sel + 6
print(','.join('%.1f:%s:%s' % e for e in ev), int(ready) + 1)
PY
}
card=Mcd001.ps2; prefix=$(std); extra=""; envx=""
case "$name" in
lounge)     # baseline: spawn point of slot 1, nothing pressed
    frames="56,64,72,84"; from=64; secs=94 ;;
dialogue)   # Yukari at the dining table: portrait 74-77 s, then the 3-choice box held until the end
    extra="60:LEFT:2.5,64:DOWN:0.5,65.5:RIGHT:1.0,69:LEFT:0.5,69:DOWN:0.5,72:CROSS:0.4,76:CROSS:0.4"
    frames="75,82,95,105"; from=84; secs=114 ;;
load-menu)  # the memory-card slot list ("Load which file?") held from ~39 s
    prefix=$(only_menu); frames="42,50,60"; from=42; secs=72 ;;
lockers)    # same flow as lounge with the second card
    card=Mcd001.lockers-save.ps2; frames="56,64,72,84"; from=64; secs=94 ;;
hall)       # DOWN 3.5 s from the spawn: dark hall camera, a door with an interaction bubble
    extra="60:DOWN:3.5"; frames="64,72,80,90"; from=68; secs=98 ;;
desk)       # RIGHT 4 s then DOWN 3 s: reception desk with a table (dark, different camera)
    extra="60:RIGHT:4,66:DOWN:3"; frames="64,72,80,92"; from=72; secs=102 ;;
corridor)   # DOWN 3.5, LEFT 3, DOWN 3, LEFT 3: carpeted corridor with a staircase banister (still labelled 1st Floor)
    extra="60:DOWN:3.5,66:LEFT:3,72:DOWN:3,78:LEFT:3"; frames="68,76,86,98"; from=84; secs=112 ;;
camp|pause-menu) # command menu root (TRIANGLE; needs P3FES_WARP_MENU=1)
    r=$(camp_seq); extra=${r% *}; from=${r##* }; envx="P3FES_WARP_MENU=1 P3FES_WARP_AT=3"
    frames="$((from-2)),$((from+4))"; secs=$((from+30)) ;;
camp-skill|camp-item|camp-persona|camp-equip|camp-status|camp-slink|camp-system)
    case "$name" in camp-skill) i=0;; camp-item) i=1;; camp-persona) i=2;; camp-equip) i=3;; camp-status) i=4;; camp-slink) i=5;; camp-system) i=6;; esac
    r=$(camp_seq $i); extra=${r% *}; from=${r##* }; envx="P3FES_WARP_MENU=1 P3FES_WARP_AT=3"
    frames="$((from-2)),$((from+4))"; secs=$((from+30)) ;;
camp-calendar|camp-quest|camp-fusion|camp-dictionary|camp-config) # System > entry
    case "$name" in camp-calendar) i=0;; camp-quest) i=1;; camp-fusion) i=2;; camp-dictionary) i=3;; camp-config) i=4;; esac
    r=$(camp_seq 6 $i); extra=${r% *}; from=${r##* }; envx="P3FES_WARP_MENU=1 P3FES_WARP_AT=3"
    frames="$((from-2)),$((from+4))"; secs=$((from+30)) ;;
save)       # save UI via the game's script command, slot 2 (empty) + OK: "Checking memory card" (~58-72 s, the P3FES_CUT_MCFREE
            # case), the slot list, "Save successful", back to the field; the card copy must come out CHANGED, no errors
    extra="78:DOWN:0.1,81:CROSS:0.2,86:CROSS:0.2"; envx="P3FES_WARP_MENU=1 P3FES_WARP_SAVE=1 P3FES_WARP_AT=14 P3FES_WARP_LOG=1"
    frames="60,66,74,83,92"; from=58; secs=100 ;;
reload)     # title > The Journey > LOAD GAME > slot 2 of the card `save` wrote (flag 0x171 = 0 there), then TRIANGLE opens the
            # command menu without any warp switch
    [ -f "$out/cards/save.ps2" ] || { echo "reload needs the card written by the save scenario ($out/cards/save.ps2): run save first" >&2; exit 1; }
    cardpath="$out/cards/save.ps2"; card=
    prefix="20:START:0.3,23:START:0.3,26:START:0.3,29:START:0.3,34:CROSS:0.35,36:CROSS:0.35,42:DOWN:0.1,44:CROSS:0.2,47:CROSS:0.2,52:CROSS:0.2"
    extra="66:TRIANGLE:0.2"; envx="P3FES_WARP_FLAGLOG=0x171"
    frames="50,56,70"; from=70; secs=100 ;;
shop|shop-buy|shop-sell) # sequence 7 (fclShop 0x3E0780), shop 1 = Be Blue V accessories, opened by the warp harness
    envx="P3FES_WARP_SEQ=7,1 P3FES_WARP_AT=10"
    case "$name" in
    shop)      extra="70:CROSS:0.2"; from=76 ;;
    shop-buy)  extra="70:CROSS:0.2,76:CROSS:0.2"; from=86 ;;
    shop-sell) extra="70:CROSS:0.2,76:RIGHT:0.1,78:CROSS:0.2"; from=88 ;;
    esac
    frames="66,$((from-2)),$((from+4))"; secs=$((from+30)) ;;
battle|battle-skill) # encounter 1 started through btlScrCmd_CALL_BATTLE; ring on Attack from ~70 s; DOWN = next ring entry
    envx="P3FES_WARP_BATTLE=1 P3FES_WARP_AT=10"
    case "$name" in
    battle)       from=72 ;;
    battle-skill) extra="74:DOWN:0.1,77:CROSS:0.2"; from=82 ;;
    esac
    frames="$((from-2)),$((from+4))"; secs=$((from+30)) ;;
tartarus)   # Tartarus entrance hall (sequence 5, floor 1): Fuuka, the Velvet Room door, the clock
    envx="P3FES_WARP_TARTARUS=1 P3FES_WARP_AT=10 P3FES_WARP_MENU=1"
    frames="70,78,86"; from=80; secs=110 ;;
title|title-menu|title-config) # pad only: title screen, "The Journey" menu, its CONFIG
    prefix="20:START:0.3,23:START:0.3,26:START:0.3,29:START:0.3"
    case "$name" in
    title)        from=31 ;;
    title-menu)   extra="34:CROSS:0.3"; from=40 ;;
    title-config) extra="34:CROSS:0.3,40:DOWN:0.1,41.5:DOWN:0.1,44:CROSS:0.2"; from=48 ;;
    esac
    frames="$((from-1)),$((from+4))"; secs=$((from+30)) ;;
*)  echo "unknown scenario $name (try: scenarios.sh list x)" >&2; exit 1 ;;
esac
[ -n "$RUN_SECS" ] && secs=$RUN_SECS   # longer hold (more samples)
script="$prefix"; [ -n "$extra" ] && script="$script,$extra"

copy="$out/cards/$name.ps2"; mkdir -p "$out/cards"
if [ -n "$cardpath" ]; then cp "$cardpath" "$copy.in" && mv "$copy.in" "$copy"; else cp "$cards/$card" "$copy"; fi
orig_sha=$(shasum "$copy" | cut -d' ' -f1)
at=$(python3 -c "print(int($from+$OFFSET))")
echo "[$name] card=$card script_extra='$extra' frames=$frames sample_at=${at}s (frame $from) run=${secs}s"
rm -rf "$out/$name" "$out/$name.log" "$out/$name.sample"
SAMPLE_AT=$at SAMPLE_SECONDS=${SAMPLE_SECONDS:-20} sh "$here/mac_run.sh" "$out" "$name" "$secs" \
    PS2X_PAD_SCRIPT="$script" PS2X_MEMCARD1="$copy" P3FES_FRAME_DIAG="$frames" PS2X_EE_SAMPLE=1 $envx "$@" 2>&1 | grep -v Terminated

rep="$out/$name.report"
{
    echo "scenario: $name"
    grep -a "^\[SIO2\] memory card in slot" "$out/$name.log" | head -2
    new_sha=$(shasum "$copy" | cut -d' ' -f1)
    if [ "$new_sha" = "$orig_sha" ]; then echo "card copy bytes: unchanged"; else echo "card copy bytes: CHANGED (sha $orig_sha -> $new_sha)"; fi
    grep -a "^\[SIO2\]" "$out/$name.log" | grep -v "in slot" | head -3
    echo "last [speed]: $(grep -a '^\[speed\]' "$out/$name.log" | tail -1 | cut -c1-70)"
    echo "last perf: $(grep -a '^\[perf\]' "$out/$name.log" | tail -1 | cut -c1-16) (run asked for ${secs}s)"
    echo "captures:"
    for f in "$out/$name"/frame-*.p3fb; do
        [ -f "$f" ] || continue
        python3 "$here/p3fb2png.py" "$f" "${f%.p3fb}.png" > /dev/null 2>&1 && echo "  ${f%.p3fb}.png" && rm -f "$f"
    done
    echo "draws per capture: $(grep -a 'p3fes-frame\] t=' "$out/$name.log" | sed -E 's/.*t=([0-9.]+) .*draws=([0-9]+).*/\1:\2/' | tr '\n' ' ')"
    python3 "$here/scenario_report.py" "$out" "$name" "$at" "$((at + ${SAMPLE_SECONDS:-20}))"
    echo "error-ish log lines (digits folded):"
    grep -a -v '^\[p3fes-frame\]\|^INFO:' "$out/$name.log" | grep -a -i "error\|fail\|abort\|assert\|unimplemented\|not found\|cannot" | grep -v "fail=0\|fallback=0\|fails=0\|failed=0\|install-fail=0\|prepare-failed=0\|bad=0\|guestFallbacks\|invalid=0\|why-fail" | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -6
} > "$rep" 2>&1
cat "$rep"
{
    printf '%-12s fps=%s busy=%s%% card=%s\n' "$name" \
        "$(sed -n 's/^fps_samples .*fps_mean \([0-9.]*\).*/\1/p' "$rep")" \
        "$(sed -n 's/^busy_pct //p' "$rep")" "$(grep -a 'card copy' "$rep" | sed 's/card copy bytes: //')"
} >> "$out/summary.txt"
