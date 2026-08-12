#!/usr/bin/env python3
# desktop_trace.py -- GH #18 reflect trace on /debug-ONLY XP, ROBUST version (session 3 fix).
#
# THE FIX: the "~28 wall" was NOT corruption/spin -- at the wall the kernel is HALTED and fully
# responsive (get_context works); our wait_state_change had simply MISSED a state-change packet
# (receive timing), so we sat waiting while the kernel sat waiting for us. So: never rely solely
# on catching every state-change. Detect halts with get_context (authoritative), and decouple the
# Continue from the receive. NewState 0x3030=EXCEPTION(int3, advance EIP), 0x3031=LOAD_SYMBOLS
# (pc in a validator, byte@EIP!=0xCC, just continue). Reflect bp = 0x805cd7f8.
#
# Flow: break-in at idle desktop (single byte, zero leftover) -> arm bp -> service the load-symbols
# stream robustly -> trigger C:\ntvdmex\pfrun.bat (GUI) when idle -> catch reflect -> single-step.
import sys, time, struct, subprocess
sys.path.insert(0, "scripts")
from kdclient import (KD, parse_state_change64, CONTROL_PACKET_LEADER, PKT_ACKNOWLEDGE,
    PKT_RESEND, PKT_RESET, PKT_STATE_CHANGE32, PKT_STATE_CHANGE64, PKT_MANIPULATE,
    API_CONTINUE, DBG_CONTINUE, INITIAL_PACKET_ID, SYNC_PACKET_ID)
BP_RT = int(sys.argv[1],16) if len(sys.argv)>1 else 0x805cd7f8
MAXS  = int(sys.argv[2],0) if len(sys.argv)>2 else 1800
STEPS = int(sys.argv[3],0) if len(sys.argv)>3 else 400
def cpu():
    o=subprocess.run("ps aux|grep qemu-system|grep -v grep|awk '{print $3}'",shell=True,capture_output=True,text=True).stdout.strip().split("\n")[0]
    try:return float(o)
    except:return -1

kd=KD("vm/kd.sock")
print("[dt] break-in at idle desktop (single byte, ~110s)...",flush=True)
pidd,data=kd.break_in(secs=300,cadence=400)
print(f"[dt] broke in pc=0x{parse_state_change64(data)['pc']&0xffffffff:08x}",flush=True)
v=kd.get_version(); slide=((v['kernbase']&0xffffffff)-0x400000) if v else 0
kd.clear_breakpoints(); st,h=kd.write_bp(BP_RT)
print(f"[dt] KernBase=0x{(v['kernbase']&0xffffffff) if v else 0:08x} armed bp h={h}",flush=True)
print("[dt] servicing (robust). >>> when idle, run D:\\dpbrun.bat (BOP DPMI trigger) <<<",flush=True)

def pure_continue(eip):
    """Advance EIP past int3 if byte==0xCC, then Continue; wait only for the ACK. Robust to RESEND."""
    if eip is not None:
        s,mem=kd.read_vmem(eip,1)
        if mem and mem[0]==0xCC:
            ctx=kd.get_context()
            if ctx and ctx.get('regs'):
                pat=bytearray(ctx['ctx']); struct.pack_into("<I",pat,0xB8,(eip+1)&0xffffffff); kd.set_context(bytes(pat))
    pid=kd.send_id; payload=kd._build_manip(API_CONTINUE,struct.pack("<i",DBG_CONTINUE))
    kd._send_data(PKT_MANIPULATE,payload,pid); t=time.time()
    while time.time()-t<4:
        kd.pump(0.2)
        for lead,ptype,rpid,d in kd.parse_buffered():
            if lead==CONTROL_PACKET_LEADER:
                if ptype==PKT_ACKNOWLEDGE and (rpid&~SYNC_PACKET_ID)==(pid&~SYNC_PACKET_ID):
                    kd.send_id^=1; return
                elif ptype==PKT_RESEND: kd._send_data(PKT_MANIPULATE,payload,pid)
                elif ptype==PKT_RESET: kd.send_id=INITIAL_PACKET_ID; pid=kd.send_id; kd._send_data(PKT_MANIPULATE,payload,pid)
            else:
                kd.ack(rpid)
    return

def halted_eip():
    """Return EIP if the kernel is halted (get_context responds), else None (running)."""
    ctx=kd.get_context()
    if ctx and ctx.get('regs'): return ctx['regs']['Eip']
    return None

pure_continue(parse_state_change64(data)['pc']&0xffffffff)   # service the initial break-in halt
count=0; t0=time.time(); idle_since=None
while time.time()-t0<MAXS:
    eip=halted_eip()
    if eip is None:
        if idle_since is None: idle_since=time.time()
        elif time.time()-idle_since>20:
            print(f"[dt] idle ~{int(time.time()-idle_since)}s ({count} serviced) -- safe to trigger pfrun; waiting for reflect",flush=True)
            idle_since=time.time()
        time.sleep(1); continue
    idle_since=None
    if eip==BP_RT:
        print(f"\n[dt] *** REFLECT HIT after {count} serviced: EIP=0x{eip:08x} (static 0x{(eip-slide)&0xffffffff:06x}) ***",flush=True)
        ctx=kd.get_context(); r=ctx['regs']
        print(f"  EIP=0x{r['Eip']:08x} EAX=0x{r['Eax']:08x} EBX=0x{r['Ebx']:08x} ECX=0x{r['Ecx']:08x} EDX=0x{r['Edx']:08x}",flush=True)
        print(f"  ESI=0x{r['Esi']:08x} EDI=0x{r['Edi']:08x} EBP=0x{r['Ebp']:08x} ESP=0x{r['Esp']:08x} CS=0x{r['Cs']:04x} SS=0x{r['Ss']:04x} EFL=0x{r['EFlags']:08x}",flush=True)
        s,mem=kd.read_vmem(r['Eip'],16)
        if mem: print(f"  code@EIP: {mem.hex()}",flush=True)
        if h is not None: kd.restore_bp(h)
        for i in range(STEPS):
            ssc,sr=kd.single_step()
            if not sr:
                e2=halted_eip()
                if e2 is None: print(f"  step {i}: running (done?)",flush=True); break
                print(f"  step {i:3d}: EIP=0x{e2:08x} (static 0x{(e2-slide)&0xffffffff:06x}) [via get_context]",flush=True); continue
            print(f"  step {i:3d}: EIP=0x{sr['Eip']:08x} (static 0x{(sr['Eip']-slide)&0xffffffff:06x}) EAX=0x{sr['Eax']:08x} exc=0x{ssc['exception']:08x}",flush=True)
        print("HIT-TRACE-DONE",flush=True); break
    count+=1
    if count<=30 or count%20==0:
        print(f"[dt] serviced #{count} EIP=0x{eip:08x} cpu={cpu()}",flush=True)
    pure_continue(eip)
print(f"[dt] end: {count} serviced cpu={cpu()}",flush=True)
