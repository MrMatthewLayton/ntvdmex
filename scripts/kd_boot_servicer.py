#!/usr/bin/env python3
# kd_boot_servicer.py -- keep a /debug+/break XP guest RUNNING under our hand-rolled KD client
# by continuously servicing the DbgLoadImageSymbols break stream.
#
# THE INSIGHT (session 3): with /debug, the kernel executes an int3 (DbgBreakPointWithStatus,
# a LOAD_SYMBOLS state change) on EVERY module load so a debugger can load symbols. Each one
# HALTS the CPU until the debugger continues past it. A real WinDbg services these in bulk; our
# client must too. If we disconnect, the next module load halts the guest forever (blocked,
# ~0% CPU). So: stay connected, and for each state change advance EIP past the int3 (if byte@EIP
# ==0xCC) and Continue -- with a PATIENT ACK wait (the old cont()'s 1s was too short on this slow
# VM, so it desynced and hung). This drives the guest all the way to the desktop and keeps it
# alive there. Optionally arm a breakpoint and STOP (return) when it fires so a caller can trace.
#
# Usage: python3 -u scripts/kd_boot_servicer.py [bp_runtime_hex] [max_secs]
import sys, time, struct, subprocess
sys.path.insert(0, "scripts")
from kdclient import (KD, parse_state_change64, CONTROL_PACKET_LEADER, PKT_ACKNOWLEDGE,
                      PKT_RESEND, PKT_RESET, PKT_MANIPULATE, API_CONTINUE, DBG_CONTINUE,
                      INITIAL_PACKET_ID)

BP_RT    = int(sys.argv[1], 16) if len(sys.argv) > 1 and sys.argv[1] != "-" else None
MAX_SECS = int(sys.argv[2], 0) if len(sys.argv) > 2 else 900

def cpu():
    o = subprocess.run("ps aux|grep qemu-system|grep -v grep|awk '{print $3}'",
                       shell=True, capture_output=True, text=True).stdout.strip().split("\n")[0]
    try: return float(o)
    except: return -1.0

# ---- attach: poll-connect until the /break (or any) halt is grabbed ---------------------
kd = None; pending = None; dl = time.time() + 400
print("[srv] polling for the KD halt...", flush=True)
while time.time() < dl and not pending:
    try: kd = KD("vm/kd.sock")
    except Exception: time.sleep(4); continue
    p = kd.resync(secs=5, tries=2)
    if p: pending = p; break
    try: kd.s.close()
    except Exception: pass
    time.sleep(5)
if not pending:
    print("[srv] no halt within window", flush=True); sys.exit(1)
sc0 = parse_state_change64(pending[1])
print(f"[srv] attached: pc=0x{sc0['pc']&0xffffffff:08x} exc=0x{sc0['exception']:08x}", flush=True)

slide = 0
if BP_RT is not None:
    v = kd.get_version()
    if v and v['kernbase']:
        slide = (v['kernbase'] & 0xffffffff) - 0x400000
    kd.clear_breakpoints()
    st, h = kd.write_bp(BP_RT)
    print(f"[srv] armed bp @ 0x{BP_RT:08x} status=0x{st:08x} handle={h}", flush=True)

from kdclient import PKT_STATE_CHANGE32, PKT_STATE_CHANGE64

def continue_and_next(timeout=20):
    """Advance EIP past an int3 if present, send Continue, then wait for the outcome that
    PROVES the continue was accepted: EITHER the transport ACK, OR the next re-break
    STATE_CHANGE (the kernel resumed far enough to break again). CRITICAL: capture that
    re-break here and RETURN it -- the old code discarded it while looping for the ACK,
    deadlocking (kernel halted at the re-break, we waiting for a 'next' that never came).
    Returns the next STATE_CHANGE dict (already ACKed) if one arrived, else None on plain ACK."""
    ctx = kd.get_context()
    if ctx and ctx['regs']:
        eip = ctx['regs']['Eip']
        st, mem = kd.read_vmem(eip, 1)
        if mem and mem[0] == 0xCC:
            patched = bytearray(ctx['ctx'])
            struct.pack_into("<I", patched, 0xB8, (eip + 1) & 0xffffffff)
            kd.set_context(bytes(patched))
    pid = kd.send_id
    payload = kd._build_manip(API_CONTINUE, struct.pack("<i", DBG_CONTINUE))
    kd._send_data(PKT_MANIPULATE, payload, pid)
    acked = False
    t = time.time()
    while time.time() - t < timeout:
        kd.pump(0.2)
        for lead, ptype, rpid, data in kd.parse_buffered():
            if lead == CONTROL_PACKET_LEADER:
                if ptype == PKT_ACKNOWLEDGE:
                    if not acked: kd.send_id ^= 1; acked = True
                elif ptype == PKT_RESEND:
                    kd._send_data(PKT_MANIPULATE, payload, pid)
                elif ptype == PKT_RESET:
                    kd.send_id = INITIAL_PACKET_ID; pid = kd.send_id
                    kd._send_data(PKT_MANIPULATE, payload, pid)
                continue
            # inbound DATA: ACK it; if it's the next re-break, RETURN it (proves acceptance)
            kd.ack(rpid)
            if ptype in (PKT_STATE_CHANGE32, PKT_STATE_CHANGE64):
                if not acked: kd.send_id ^= 1; acked = True   # re-break implies continue took
                return parse_state_change64(data)
        if acked and time.time() - t > 1.0:
            return None       # continue accepted, no immediate re-break -> caller waits
    return None

# service the initial halt; `sc` always holds the current unresolved break
sc = sc0
count = 0; t0 = time.time(); last_break = time.time(); last_log = time.time()
while time.time() - t0 < MAX_SECS:
    if sc is None:
        sc = kd.wait_state_change(2.0)
    if sc:
        pc = sc['pc'] & 0xffffffff
        count += 1; last_break = time.time()
        if BP_RT is not None and pc == BP_RT:
            print(f"\n[srv] *** TARGET BP HIT after {count} serviced: pc=0x{pc:08x} "
                  f"(static 0x{(pc-slide)&0xffffffff:06x}) exc=0x{sc['exception']:08x} ***", flush=True)
            ctx = kd.get_context()
            if ctx and ctx['regs']:
                r = ctx['regs']
                print(f"  EIP=0x{r['Eip']:08x} EAX=0x{r['Eax']:08x} EBX=0x{r['Ebx']:08x} "
                      f"ECX=0x{r['Ecx']:08x} EDX=0x{r['Edx']:08x}", flush=True)
                print(f"  ESI=0x{r['Esi']:08x} EDI=0x{r['Edi']:08x} EBP=0x{r['Ebp']:08x} "
                      f"ESP=0x{r['Esp']:08x} CS=0x{r['Cs']:04x} EFL=0x{r['EFlags']:08x}", flush=True)
                st, mem = kd.read_vmem(r['Eip'], 16)
                if mem: print(f"  code@EIP: {mem.hex()}", flush=True)
            if h is not None:
                kd.restore_bp(h)                 # remove the bp so we can step the body
            for i in range(400):
                ssc, sr = kd.single_step()
                if not sr:
                    print(f"  step {i}: no state change", flush=True); break
                print(f"  step {i:3d}: EIP=0x{sr['Eip']:08x} (static 0x{(sr['Eip']-slide)&0xffffffff:06x}) "
                      f"EAX=0x{sr['Eax']:08x} exc=0x{ssc['exception']:08x}", flush=True)
            print("HIT-TRACE-DONE", flush=True)
            break
        if count <= 3 or count % 25 == 0:
            print(f"[srv] serviced #{count} pc=0x{pc:08x} exc=0x{sc['exception']:08x} cpu={cpu()}", flush=True)
        sc = continue_and_next()      # continue past this break; capture the next re-break (or None)
    else:
        if time.time() - last_log > 20:
            print(f"[srv] idle tick: {count} serviced, {int(time.time()-last_break)}s since last break, cpu={cpu()}", flush=True)
            last_log = time.time()
print(f"[srv] exiting: serviced {count} breaks, cpu={cpu()}", flush=True)
