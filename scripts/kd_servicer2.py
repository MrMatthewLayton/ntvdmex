#!/usr/bin/env python3
# kd_servicer2.py -- instrumented + protocol-faithful boot servicer (session 3, informed by
# ReactOS kddll.c). Key facts: the kernel ACKs EVERY inbound packet but only PROCESSES one whose
# PacketId == RemotePacketId (which toggles only on processing). So an ACK does NOT prove the
# Continue was applied. We confirm application by the re-break OR by probing get_context (if the
# kernel answers a query, it's still halted => our Continue was ignored => resend at the other id).
import sys, time, struct, subprocess
sys.path.insert(0, "scripts")
from kdclient import (KD, parse_state_change64, CONTROL_PACKET_LEADER, PKT_ACKNOWLEDGE,
    PKT_RESEND, PKT_RESET, PKT_STATE_CHANGE32, PKT_STATE_CHANGE64, PKT_MANIPULATE,
    API_CONTINUE, DBG_CONTINUE, INITIAL_PACKET_ID, SYNC_PACKET_ID)
BP_RT   = int(sys.argv[1],16) if len(sys.argv)>1 and sys.argv[1]!='-' else None
MAXS    = int(sys.argv[2],0) if len(sys.argv)>2 else 900
def cpu():
    o=subprocess.run("ps aux|grep qemu-system|grep -v grep|awk '{print $3}'",shell=True,capture_output=True,text=True).stdout.strip().split("\n")[0]
    try:return float(o)
    except:return -1
TN={1:"SC32",2:"MANIP",3:"DBGIO",4:"ACK",5:"RESEND",6:"RESET",7:"SC64"}

kd=KD("vm/kd.sock")
pend=None;dl=time.time()+400
print("[s2] polling for halt...",flush=True)
while time.time()<dl and not pend:
    p=kd.resync(secs=5,tries=2)
    if p: pend=p; break
    time.sleep(4)
if not pend: print("[s2] no halt"); sys.exit(1)
sc=parse_state_change64(pend[1])
print(f"[s2] attached pc=0x{sc['pc']&0xffffffff:08x} send_id=0x{kd.send_id:08x}",flush=True)
slide=0
if BP_RT is not None:
    v=kd.get_version(); slide=((v['kernbase']&0xffffffff)-0x400000) if v else 0
    kd.clear_breakpoints(); st,hh=kd.write_bp(BP_RT)
    print(f"[s2] bp@0x{BP_RT:08x} h={hh}",flush=True)

def send_continue():
    """advance EIP past int3, send Continue at send_id, return (outcome, sc_or_none).
       outcome in {'rebreak','running','resend','timeout'}."""
    ctx=kd.get_context()
    eipmoved=False
    if ctx and ctx['regs']:
        eip=ctx['regs']['Eip']; st,mem=kd.read_vmem(eip,1)
        if mem and mem[0]==0xCC:
            pat=bytearray(ctx['ctx']); struct.pack_into("<I",pat,0xB8,(eip+1)&0xffffffff)
            kd.set_context(bytes(pat)); eipmoved=True
    pid=kd.send_id
    payload=kd._build_manip(API_CONTINUE,struct.pack("<i",DBG_CONTINUE))
    kd._send_data(PKT_MANIPULATE,payload,pid)
    acked=False; t=time.time()
    while time.time()-t<6:
        kd.pump(0.2)
        for lead,ptype,rpid,data in kd.parse_buffered():
            if lead==CONTROL_PACKET_LEADER:
                if ptype==PKT_ACKNOWLEDGE and (rpid&~SYNC_PACKET_ID)==(pid&~SYNC_PACKET_ID):
                    if not acked: kd.send_id^=1; acked=True
                elif ptype==PKT_RESEND:
                    kd._send_data(PKT_MANIPULATE,payload,pid)
                elif ptype==PKT_RESET:
                    kd.send_id=INITIAL_PACKET_ID; pid=kd.send_id
                    kd._send_data(PKT_MANIPULATE,payload,pid)
                continue
            kd.ack(rpid)
            if ptype in (PKT_STATE_CHANGE32,PKT_STATE_CHANGE64):
                if not acked: kd.send_id^=1; acked=True
                return ('rebreak', parse_state_change64(data))
        if acked and time.time()-t>1.5:
            break
    # ambiguous: ACK but no re-break. PROBE: is the kernel still halted?
    probe=kd.get_context()
    if probe and probe.get('regs'):
        return ('still_halted', None)   # kernel answered a query => halted => continue NOT applied
    return ('running', None)

count=0; t0=time.time()
cur=sc
while time.time()-t0<MAXS:
    if cur is None:
        cur=kd.wait_state_change(2.0)
        if cur is None: continue
    pc=cur['pc']&0xffffffff; count+=1
    if BP_RT is not None and pc==BP_RT:
        print(f"[s2] *** TARGET HIT #{count} pc=0x{pc:08x} ***",flush=True); break
    sidb=kd.send_id
    outcome,nxt=send_continue()
    if count<=40 or count%20==0 or outcome!='rebreak':
        print(f"[s2] #{count} pc=0x{pc:08x} send_id 0x{sidb:08x}->0x{kd.send_id:08x} outcome={outcome} cpu={cpu()}",flush=True)
    if outcome=='still_halted':
        print(f"[s2] !! continue NOT applied at #{count}; kernel still halted. send_id=0x{kd.send_id:08x}",flush=True)
        # try the OTHER id
        kd.send_id^=1
        outcome2,nxt2=send_continue()
        print(f"[s2]    retry other id -> outcome={outcome2} send_id=0x{kd.send_id:08x}",flush=True)
        cur=nxt2
        continue
    cur=nxt   # rebreak-> next sc; running/timeout-> None (wait)
print(f"[s2] end: {count} serviced cpu={cpu()}",flush=True)
