#!/usr/bin/env python3
# pmfault_observe.py -- GH #18: OBSERVE what a raw PM #GP (pfrun/pmfault HLT) does in the kernel.
#
# WHY THIS EXISTS (2026-08-11 finding): a BOP-based DPMI client (dpmiback/i310102) can NEVER reach
# the #GP reflect at static 0x4f67f8 -- NTVDM BOPs are C4 C4 (an invalid LES encoding) => #UD =>
# KiTrap06 => VdmDispatchBop, whereas 0x4f67f8 hangs off #GP => KiTrap0D (via the "BOP-only" gate
# 0x565041). Confirmed empirically: dpmiback ran end-to-end on the real-CPU path and the reflect bp
# was never hit. So ONLY a genuine privileged-instruction #GP (pfrun's HLT) routes toward 0x4f67f8.
#
# desktop_trace.py catches ONLY the reflect bp (and pure_continues everything else); kdclient.py's
# do_session blindly resume()s any non-bp halt -- both would step PAST the raw #GP. This harness
# classifies every halt by PC and STOPS+DUMPS+single-steps on the UNEXPECTED one (the raw #GP or a
# bugcheck), finally making the runs-65..69 "invisible fault" visible.
#
# Flow: break in at idle desktop -> arm reflect bp 0x805cd7f8 -> service the LOAD_SYMBOLS burst that
# ntvdmhost's launch triggers -> when pfrun's HLT #GPs, catch it (reflect bp HIT, or an unexpected
# EXCEPTION at a KiTrap0D-path PC) -> dump ctx/stack/code + single-step to see KiTrap0D's decision.
import sys, time, struct, subprocess
sys.path.insert(0, "scripts")
from kdclient import (KD, parse_state_change64, CONTROL_PACKET_LEADER, PKT_ACKNOWLEDGE,
    PKT_RESEND, PKT_RESET, PKT_STATE_CHANGE32, PKT_STATE_CHANGE64, PKT_MANIPULATE,
    API_CONTINUE, DBG_CONTINUE, INITIAL_PACKET_ID, SYNC_PACKET_ID)

BP_RT   = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x805cd7f8   # reflect entry 0x4f67f8 runtime
MAXS    = int(sys.argv[2], 0)  if len(sys.argv) > 2 else 1800
STEPS   = int(sys.argv[3], 0)  if len(sys.argv) > 3 else 200

# Build-specific benign halt PCs (KernBase 0x804d7000): the break-in int3 and the LOAD_SYMBOLS
# validator. Any OTHER non-bp halt is treated as the interesting raw fault.
BREAKIN_PC = 0x80527bdc                      # RtlpBreakWithStatusInstruction 0xCC
SYMVAL_PC  = 0x8052e4c4                      # MmIsAddressValid validator (LOAD_SYMBOLS reporter)
NEWSTATE_EXCEPTION    = 0x3030
NEWSTATE_LOADSYMBOLS  = 0x3031

def cpu():
    o = subprocess.run("ps aux|grep qemu-system|grep -v grep|awk '{print $3}'", shell=True,
                       capture_output=True, text=True).stdout.strip().split("\n")[0]
    try: return float(o)
    except: return -1

kd = KD("vm/kd.sock")
print("[pf] break-in at idle desktop (single byte, ~110s)...", flush=True)
pid, data = kd.break_in(secs=300, cadence=400)
sc0 = parse_state_change64(data)
print(f"[pf] broke in pc=0x{sc0['pc']&0xffffffff:08x}", flush=True)
v = kd.get_version(); slide = ((v['kernbase'] & 0xffffffff) - 0x400000) if v else 0
kd.clear_breakpoints(); st, h = kd.write_bp(BP_RT)
print(f"[pf] KernBase=0x{(v['kernbase']&0xffffffff) if v else 0:08x} slide=0x{slide:08x} "
      f"reflect bp @0x{BP_RT:08x} armed h={h}", flush=True)
print("[pf] servicing. >>> when idle, run D:\\pfrun.bat (raw PM #GP) <<<", flush=True)

def rb(rt): return (rt - slide) & 0xffffffff   # runtime -> static VA (r2 base 0x400000)

def pure_continue(eip):
    """Advance EIP past int3 if byte==0xCC, then Continue; wait only for the ACK (robust to RESEND)."""
    if eip is not None:
        s, mem = kd.read_vmem(eip, 1)
        if mem and mem[0] == 0xCC:
            ctx = kd.get_context()
            if ctx and ctx.get('regs'):
                pat = bytearray(ctx['ctx']); struct.pack_into("<I", pat, 0xB8, (eip+1) & 0xffffffff)
                kd.set_context(bytes(pat))
    pid = kd.send_id
    payload = kd._build_manip(API_CONTINUE, struct.pack("<i", DBG_CONTINUE))
    kd._send_data(PKT_MANIPULATE, payload, pid); t = time.time()
    while time.time() - t < 4:
        kd.pump(0.2)
        for lead, ptype, rpid, d in kd.parse_buffered():
            if lead == CONTROL_PACKET_LEADER:
                if ptype == PKT_ACKNOWLEDGE and (rpid & ~SYNC_PACKET_ID) == (pid & ~SYNC_PACKET_ID):
                    kd.send_id ^= 1; return
                elif ptype == PKT_RESEND: kd._send_data(PKT_MANIPULATE, payload, pid)
                elif ptype == PKT_RESET:
                    kd.send_id = INITIAL_PACKET_ID; pid = kd.send_id
                    kd._send_data(PKT_MANIPULATE, payload, pid)
            else:
                kd.ack(rpid)
    return

def halted_eip():
    ctx = kd.get_context()
    if ctx and ctx.get('regs'): return ctx['regs']['Eip']
    return None

def dump_and_trace(label, pc, exc=None):
    print(f"\n[pf] *** {label}  PC=0x{pc:08x} (static 0x{rb(pc):06x})"
          + (f"  exc=0x{exc:08x}" if exc is not None else "") + " ***", flush=True)
    ctx = kd.get_context()
    if ctx and ctx.get('regs'):
        r = ctx['regs']
        print(f"  EIP=0x{r['Eip']:08x} CS=0x{r['Cs']:04x} EFL=0x{r['EFlags']:08x} ESP=0x{r['Esp']:08x} SS=0x{r['Ss']:04x}", flush=True)
        print(f"  EAX=0x{r['Eax']:08x} EBX=0x{r['Ebx']:08x} ECX=0x{r['Ecx']:08x} EDX=0x{r['Edx']:08x}", flush=True)
        print(f"  ESI=0x{r['Esi']:08x} EDI=0x{r['Edi']:08x} EBP=0x{r['Ebp']:08x}", flush=True)
        print(f"  DS=0x{r['Ds']:04x} ES=0x{r['Es']:04x} FS=0x{r['Fs']:04x} GS=0x{r['Gs']:04x}", flush=True)
        st, mem = kd.read_vmem(r['Eip'], 24)
        if mem: print(f"  code@EIP: {mem.hex()}", flush=True)
        st, stk = kd.read_vmem(r['Esp'], 64)
        if stk:
            words = struct.unpack_from("<16I", stk)
            print("  stack@ESP: " + " ".join(f"{w:08x}" for w in words[:8]), flush=True)
            print("             " + " ".join(f"{w:08x}" for w in words[8:]), flush=True)
    # restore the reflect bp if this is it (so single-step doesn't re-trap on the 0xCC)
    if pc == BP_RT and h is not None:
        kd.restore_bp(h)
    print(f"  --- single-stepping {STEPS} (watch for the return-0 / KiTrap0D divergence) ---", flush=True)
    for i in range(STEPS):
        ssc, sr = kd.single_step()
        if not sr:
            e2 = halted_eip()
            print(f"  step {i}: no regs (eip={None if e2 is None else hex(e2)})", flush=True)
            if e2 is None: break
            continue
        extra = ""
        if ssc: extra = f" exc=0x{ssc['exception']:08x} ns=0x{ssc['new_state']:04x}"
        print(f"  step {i:3d}: EIP=0x{sr['Eip']:08x} (static 0x{rb(sr['Eip']):06x}) "
              f"EAX=0x{sr['Eax']:08x} ECX=0x{sr['Ecx']:08x} EDX=0x{sr['Edx']:08x}{extra}", flush=True)
    print("PF-TRACE-DONE", flush=True)

# Halt detection is PASSIVE wait_state_change ONLY. CRITICAL (learned the hard way, 3 reboots
# 2026-08-11): do NOT poll get_context in the idle loop -- each get_context is a manipulate
# send+recv, and polling it every ~1s over a multi-minute idle is exactly the "break-in churn
# bugchecks XP over an extended session" failure kdclient warns about (the guest rebooted every
# time). wait_state_change is a pure RECEIVE (pumps the socket, sends nothing) so a long idle
# generates ZERO KD traffic; the kernel retransmits an un-ACKed state-change, so missing one is
# self-healing. sc carries new_state + exception, so we classify richly and dump the UNEXPECTED
# fault. (get_context is still used AFTER a halt, in dump_and_trace/pure_continue -- bounded, fine.)
pure_continue(sc0['pc'] & 0xffffffff)     # service the initial break-in halt
count = 0; t0 = time.time(); idle_since = None
while time.time() - t0 < MAXS:
    sc = kd.wait_state_change(2.0)         # PASSIVE: recv-only, no traffic when idle
    if sc is None:
        if idle_since is None: idle_since = time.time()
        elif time.time() - idle_since > 20:
            print(f"[pf] idle ~{int(time.time()-idle_since)}s ({count} serviced) -- trigger pfrun; waiting", flush=True)
            idle_since = time.time()
        continue
    idle_since = None
    pc = sc['pc'] & 0xffffffff; ns = sc['new_state']; exc = sc['exception']

    if pc == BP_RT:
        dump_and_trace("REFLECT HIT (0x4f67f8 reached!)", pc, exc); break
    if ns == NEWSTATE_LOADSYMBOLS or pc == SYMVAL_PC:
        count += 1
        if count <= 25 or count % 20 == 0:
            print(f"[pf] load-symbols #{count} pc=0x{pc:08x}", flush=True)
        pure_continue(pc); continue
    if pc == BREAKIN_PC:
        pure_continue(pc); continue          # leftover break-in int3, drain
    # ---- UNEXPECTED halt: the raw #GP or a bugcheck. This is the observation. ----
    dump_and_trace("UNEXPECTED FAULT (raw #GP / bugcheck path)", pc, exc); break

print(f"[pf] end: {count} serviced", flush=True)
