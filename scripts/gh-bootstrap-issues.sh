#!/usr/bin/env bash
# Bootstrap the GitHub issue tracker from the local docs (ROADMAP.md / STATE.md /
# risks.md). Epics are modelled as GitHub **milestones** (M4-M8); bugs and
# follow-ups get labels but no milestone. Safe to re-run: milestones, labels, and
# issues are each created only if a same-named one does not already exist.
#
# Prereq:  gh authenticated with repo scope  ->  gh auth login   (or export GH_TOKEN)
# Usage:   scripts/gh-bootstrap-issues.sh          # create everything
#          DRY_RUN=1 scripts/gh-bootstrap-issues.sh # print what it *would* create
set -euo pipefail

REPO="MrMatthewLayton/ntvdmex"
DRY_RUN="${DRY_RUN:-0}"

run() { if [ "$DRY_RUN" = 1 ]; then echo "DRY: $*"; else "$@"; fi; }

command -v gh >/dev/null || { echo "gh not found on PATH"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "gh not authenticated -- run: gh auth login"; exit 1; }

echo "== labels =="
label() {  # name  color  description
  if gh label list --repo "$REPO" --limit 200 --json name --jq '.[].name' 2>/dev/null | grep -qxF "$1"; then
    echo "  exists: $1"; return
  fi
  if [ "$DRY_RUN" = 1 ]; then echo "DRY: label $1"; return; fi
  # tolerant: a same-named label created concurrently / cached-list miss is not fatal
  if gh label create "$1" --repo "$REPO" --color "$2" --description "$3" >/dev/null 2>&1; then
    echo "  created: $1"
  else
    echo "  exists: $1"
  fi
}
label epic        5319e7 "Milestone-level epic"
label bug         d73a4a "A defect in shipped behaviour"
label enhancement a2eeef "New capability / feature work"
label spike       fbca04 "Throwaway proof-of-feasibility"
label research    0e8a16 "Contract recovery / disassembly / design"
label follow-up   c5def5 "Deferred, non-blocking loose end"
label blocked     b60205 "Waiting on another item"

echo "== milestones (epics) =="
milestone() {  # title  description
  if gh api "repos/$REPO/milestones?state=all" --paginate --jq '.[].title' 2>/dev/null | grep -qxF "$1"; then
    echo "  exists: $1"; return
  fi
  if [ "$DRY_RUN" = 1 ]; then echo "DRY: milestone $1"; return; fi
  # tolerant: a 422 'already_exists' (cached-list miss) is not fatal
  if gh api "repos/$REPO/milestones" -f title="$1" -f description="$2" >/dev/null 2>&1; then
    echo "  created: $1"
  else
    echo "  exists: $1"
  fi
}
milestone "M4 - Memory extensions" "XMS/EMS done + VM-confirmed (8/8 selftest). DPMI is the open third."
milestone "M5 - Win16/WOW foundation" "WOW bootstrap + NE loader; a trivial Win16 .EXE reaches its message loop."
milestone "M6 - Win16 thunking" "16:16<->flat thunks; USER/GDI 16-bit objects -> Win32 handles; message bridging."
milestone "M7 - Peripheral VDDs" "Sound (full audio), networking, serial/parallel VDDs; bare-metal vs virtualized strategy."
milestone "M8 - Polish & SDK" "Pluggable VDD/driver SDK + docs; Luna theming/full-screen; installer/registration."

echo "== issues =="
# Cache existing titles once so re-runs don't duplicate.
EXISTING="$(gh issue list --repo "$REPO" --state all --limit 500 --json title --jq '.[].title' 2>/dev/null || true)"
issue() {  # title  milestone(empty for none)  labels(csv)  body
  local title="$1" ms="$2" labels="$3" body="$4" url
  if printf '%s\n' "$EXISTING" | grep -qxF "$title"; then
    echo "  exists: $title"; return
  fi
  if [ "$DRY_RUN" = 1 ]; then echo "DRY: issue [${ms:-no milestone}] $title"; return; fi
  local args=(--repo "$REPO" --title "$title" --label "$labels" --body "$body")
  if [ -n "$ms" ]; then args+=(--milestone "$ms"); fi
  # non-fatal per issue: one failure must not abort the remaining creates
  if url=$(gh issue create "${args[@]}" 2>&1); then
    echo "  created: $url"
  else
    echo "  FAILED : $title -> $url"
  fi
}

# ---- M4: Memory extensions ------------------------------------------------
issue "DPMI: 16-bit real->protected-mode-switch spike" "M4 - Memory extensions" "spike,research" \
"Prove the real->PM mode-switch round-trip by reusing the kernel monitor's PM support (the *same* NtVdmControl VDM runs PM, distinguished by the MSW PE bit): services 10/11 (VdmSetLdtEntries / VdmSetProcessLdtInfo) install the LDT, service 13 (VdmPMCliControl) drives the client IF.

Spike shape: real -> PM far-call -> confirm PM via MSW PE bit -> INT 31h 0000 (alloc descriptor) -> 0300 thunk INT 21h back to real mode -> exit.

Ref: docs/research/dpmi-under-ntvdmcontrol.md. This is the M4 keystone risk."

issue "DPMI: INT 31h surface + advertise INT 2Fh AX=1687h" "M4 - Memory extensions" "enhancement,blocked" \
"After the mode-switch spike proves out, implement the INT 31h DPMI service surface and only THEN advertise the host via INT 2Fh AX=1687h. Advertising before the switch works crashes extenders worse than absence, so 1687h stays unhandled until the spike is green.

Blocked by: the DPMI mode-switch spike. Ref: docs/research/dpmi-under-ntvdmcontrol.md."

# ---- M5: Win16 / WOW foundation -------------------------------------------
issue "WOW bootstrap + krnl386/user/gdi hosting" "M5 - Win16/WOW foundation" "enhancement,research" \
"Stand up a wowexec analog and host the 16-bit krnl386 / user / gdi surface. Built on the same V86+DOS foundation as everything before it (ADR-0003). Lean on WINE + ReactOS for API semantics (risk R3).

Exit (milestone): a trivial Win16 .EXE loads and reaches its message loop."

issue "NE loader + 16-bit module/segment management" "M5 - Win16/WOW foundation" "enhancement" \
"New-Executable (NE) loader: parse the NE header, load segments, apply relocations, and manage 16-bit module/segment tables. Feeds the WOW bootstrap."

# ---- M6: Win16 thunking ---------------------------------------------------
issue "16:16 <-> flat pointer translation (generic/flat thunks)" "M6 - Win16 thunking" "enhancement,research" \
"Implement 16:16 <-> flat pointer translation and the generic/flat thunk machinery so 16-bit code can call into 32-bit host services and vice versa."

issue "USER/GDI 16-bit objects -> Win32 handles + message bridging" "M6 - Win16 thunking" "enhancement" \
"Map 16-bit USER/GDI objects onto Win32 handles and bridge the 16-bit message loop.

Exit (milestone): a real Win16 GUI app runs and paints."

# ---- M7: Peripheral VDDs --------------------------------------------------
issue "Sound VDD: full audio output" "M7 - Peripheral VDDs" "enhancement" \
"Promote the PC-speaker VDD stub (src/vdd/vdd_speaker.c, reports active/Hz, no audio) to real host-backed audio; add Sound Blaster-class emulation as a pluggable VDD."

issue "Networking VDD" "M7 - Peripheral VDDs" "enhancement" \
"Host-backed networking as a pluggable VDD (packet driver / NDIS-era interface as appropriate)."

issue "Serial/parallel port VDDs" "M7 - Peripheral VDDs" "enhancement" \
"Host-backed serial (COM) and parallel (LPT) port VDDs."

issue "Bare-metal vs virtualized device strategy" "M7 - Peripheral VDDs" "research" \
"Decide, per device class, the bare-metal-vs-virtualized strategy and add explicit bare-metal validation each device milestone (risk R5, ADR-0005). The pluggable VDD model isolates host specifics."

# ---- M8: Polish & SDK -----------------------------------------------------
issue "Pluggable VDD/driver SDK + third-party docs" "M8 - Polish & SDK" "enhancement" \
"Ship a documented SDK for third-party VDD/driver developers on top of the internal ntvdd.h ABI (+ the deferred vddsvc.h binary-compat veneer, ADR-0008)."

issue "Luna theming pass + full-screen story" "M8 - Polish & SDK" "enhancement" \
"Finish the Luna theming pass and the full-screen presentation story (DirectDraw fullscreen flip is in; wire the remaining UX)."

issue "Installer / registration tooling" "M8 - Polish & SDK" "enhancement" \
"Installer + registration tooling (IFEO Debugger redirect setup per ADR-0007, uninstall, etc.)."

# ---- Bugs & follow-ups (no milestone) -------------------------------------
issue "[BUG] No text rendering after a graphics->text mode switch" "" "bug" \
"After INT 10h sets mode 13h and then restores text mode 3, the host renders no further text -- INT 21h console output goes to a dead (black) display. The video VDD does not re-establish the text-grid renderer on return to mode 3, and/or the present path stays on the 13h framebuffer.

selftest works around it by not switching the visible mode, but any real program that switches 13h->text is blank. **Current top priority** (STATE.md 'Single next action' item 0)."

issue "[BUG] Mode-12h store decoder: arbitrary MOV/ModRM stores" "" "bug,enhancement" \
"The planar mode-12h A0000 store decoder (host_try_mem -> vga_planar_write) handles the REP STOSB/STOSW idiom and the QBasic MOV/XCHG stores that the demos need, but arbitrary MOV/ModRM stores (general QBasic SCREEN 12 and other 12h software) are still a gap. Ref: ROADMAP.md M3, docs/research/hardware-vga-acceleration.md."

issue "[follow-up] M2.5: recover real-shell args + exit-to-shell notify" "" "follow-up" \
"Best-effort (non-blocking): recover arbitrary real-shell args from CSRSS's undocumented multi-call GetNextVDMCommand protocol, and wire the exit-code-to-shell notify. Also confirm mem.exe gets past 'Parse Error 1' (likely fixed by the env block). Guest-visible plumbing (env block, command tail, errorlevel) is already done + off-VM-tested."

issue "[follow-up] Sweep demos for remaining INT 21h / INT 10h gaps" "" "follow-up" \
"Sweep the QuickBASIC demos (and other DOS apps) for any remaining unhandled INT 21h / INT 10h services unrelated to the mode-12h per-pixel speed wall. The host logs 'INT21 AH=0x.. unhandled', so one run per app pinpoints gaps."

echo
echo "Done. Review at: https://github.com/$REPO/issues"
