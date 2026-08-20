#!/usr/bin/env python3
# trace_break.py -- GH #18 reflect trace via the /break-at-boot CLEAN-ARM path (session 3).
#
# WHY: actively breaking into a RUNNING kernel on this heavily-throttled HVF VM takes ~110s and
# leaves a backlog of queued break-in bytes; draining that backlog churns KDCOM and repeatedly
# BUGCHECKED the guest / CRASHED QEMU. The fix: boot with /break so the kernel halts ONCE at
# init with a FRESH, retransmitting state change -- grabbable by a plain resync with ZERO
# break-in bytes. No spray, no backlog, no drain, no churn.
#
# FLOW: (1) poll-connect until the /break halt is grabbed (resync only). (2) read KernBase,
# arm the reflect breakpoint at runtime (0x4f67f8 + slide), resume -> guest boots to desktop
# with the bp live. (3) wait for the kernel-initiated HIT when pfrun triggers the PM #GP;
# single-step `STEPS` insns logging rebased EIP+EAX so we SEE which gate returns 0.
#
# Trigger pfrun (C:\ntvdmex\pfrun.bat) in the guest AFTER "armed; resuming" prints and the
# desktop is up. Usage: python3 -u scripts/trace_break.py [steps] [attach_secs] [hit_secs]
import sys, time
sys.path.insert(0, "scripts")
from kdclient import KD, parse_state_change64

REFLECT_STATIC = 0x4f67f8
STEPS       = int(sys.argv[1], 0) if len(sys.argv) > 1 else 400
ATTACH_SECS = int(sys.argv[2], 0) if len(sys.argv) > 2 else 600
HIT_SECS    = int(sys.argv[3], 0) if len(sys.argv) > 3 else 1200

def rb(rt, slide): return (rt - slide) & 0xffffffff

# ---- Phase 1: poll for the /break halt (resync ONLY -- no break-in bytes) ----------------
pending = None; kd = None
deadline = time.time() + ATTACH_SECS
print(f"[break] polling vm/kd.sock for the /break halt (up to {ATTACH_SECS}s)...", flush=True)
while time.time() < deadline and not pending:
    try:
        kd = KD("vm/kd.sock")
    except Exception:
        time.sleep(4); continue
    p = kd.resync(secs=5, tries=2)
    if p:
        pending = p; break
    try: kd.s.close()
    except Exception: pass
    time.sleep(5)

if not pending:
    print("[break] NO /break halt detected within window", flush=True); sys.exit(1)

sc = parse_state_change64(pending[1])
print(f"[break] HALTED at /break: pc=0x{sc['pc']&0xffffffff:08x} exc=0x{sc['exception']:08x}", flush=True)
v = kd.get_version()
if not v or not v['kernbase']:
    print("[break] GetVersion failed", flush=True); sys.exit(1)
base  = v['kernbase'] & 0xffffffff
slide = (base - 0x400000) & 0xffffffff
print(f"[break] KernBase=0x{base:08x} slide=0x{slide:08x}", flush=True)
kd.clear_breakpoints()
rt = (REFLECT_STATIC + slide) & 0xffffffff
status, h = kd.write_bp(rt)
print(f"[break] bp @ static 0x{REFLECT_STATIC:06x} -> runtime 0x{rt:08x} status=0x{status:08x} handle={h}", flush=True)
handles = {rt: h} if h is not None else {}
print("[break] armed; resuming -> guest boots to desktop with the bp live.", flush=True)
print("[break] >>> when the desktop is up, run C:\\ntvdmex\\pfrun.bat in the guest <<<", flush=True)
kd.resume()

# ---- Phase 2: wait for the kernel-initiated reflect HIT (no churn) -----------------------
deadline = time.time() + HIT_SECS
hits = 0
try:
    while time.time() < deadline:
        sc = kd.wait_state_change(1.0)
        if not sc:
            continue
        if sc['pc'] not in handles:
            # not our reflect bp (unexpected under /break) -- step past and keep waiting
            kd.resume(); continue
        hits += 1
        print(f"\n[HIT #{hits}] PC=0x{sc['pc']:08x} (static 0x{rb(sc['pc'],slide):06x}) "
              f"exc=0x{sc['exception']:08x}", flush=True)
        ctx = kd.get_context()
        if ctx and ctx['regs']:
            r = ctx['regs']
            print(f"  EIP=0x{r['Eip']:08x} EAX=0x{r['Eax']:08x} EBX=0x{r['Ebx']:08x} "
                  f"ECX=0x{r['Ecx']:08x} EDX=0x{r['Edx']:08x}", flush=True)
            print(f"  ESI=0x{r['Esi']:08x} EDI=0x{r['Edi']:08x} EBP=0x{r['Ebp']:08x} "
                  f"ESP=0x{r['Esp']:08x} CS=0x{r['Cs']:04x} EFL=0x{r['EFlags']:08x}", flush=True)
            st, mem = kd.read_vmem(r['Eip'], 16)
            if mem: print(f"  code@EIP: {mem.hex()}", flush=True)
        if sc['pc'] in handles:
            kd.restore_bp(handles.pop(sc['pc']))
        for i in range(STEPS):
            ssc, sr = kd.single_step()
            if not sr:
                print(f"  step {i}: no state change", flush=True); break
            print(f"  step {i:3d}: EIP=0x{sr['Eip']:08x} (static 0x{rb(sr['Eip'],slide):06x}) "
                  f"EAX=0x{sr['Eax']:08x} exc=0x{ssc['exception']:08x}", flush=True)
        kd.resume()
        break
    else:
        print(f"[break] hit window elapsed with {hits} hit(s)", flush=True)
finally:
    for rt, h in list(handles.items()):
        try: kd.restore_bp(h)
        except Exception: pass
    kd.resume()
print("[break] done", flush=True)
