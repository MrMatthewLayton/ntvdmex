#!/usr/bin/env python3
# Deep-instrumented desktop trace: full register dump at EVERY break to catch the ~28 wall corruption.
import sys,time,struct,subprocess
sys.path.insert(0,"scripts")
from kdclient import (KD,parse_state_change64,CONTROL_PACKET_LEADER,PKT_ACKNOWLEDGE,PKT_RESEND,
    PKT_RESET,PKT_STATE_CHANGE32,PKT_STATE_CHANGE64,PKT_MANIPULATE,API_CONTINUE,DBG_CONTINUE,
    INITIAL_PACKET_ID,SYNC_PACKET_ID)
BP_RT=0x805cd7f8
def cpu():
    o=subprocess.run("ps aux|grep qemu-system|grep -v grep|awk '{print $3}'",shell=True,capture_output=True,text=True).stdout.strip().split("\n")[0]
    try:return float(o)
    except:return -1
kd=KD("vm/kd.sock")
print("[di] breaking in...",flush=True)
pidd,data=kd.break_in(secs=280,cadence=90)
print(f"[di] broke in pc=0x{parse_state_change64(data)['pc']&0xffffffff:08x}",flush=True)
v=kd.get_version(); slide=((v['kernbase']&0xffffffff)-0x400000) if v else 0
kd.clear_breakpoints(); st,h=kd.write_bp(BP_RT)
print(f"[di] armed h={h}; deep-logging each break",flush=True)
def cont1():
    """advance EIP past int3 (needed), then continue; return (next_sc, before_regs, moved)."""
    ctx=kd.get_context(); r=ctx['regs'] if ctx else None; moved=False
    if r:
        eip=r['Eip']; s,mem=kd.read_vmem(eip,1)
        if mem and mem[0]==0xCC:
            pat=bytearray(ctx['ctx']); struct.pack_into("<I",pat,0xB8,(eip+1)&0xffffffff)
            kd.set_context(bytes(pat)); moved=True
    pid=kd.send_id; payload=kd._build_manip(API_CONTINUE,struct.pack("<i",DBG_CONTINUE))
    kd._send_data(PKT_MANIPULATE,payload,pid); acked=False; t=time.time()
    nxt=None
    while time.time()-t<6:
        kd.pump(0.2)
        for lead,ptype,rpid,d in kd.parse_buffered():
            if lead==CONTROL_PACKET_LEADER:
                if ptype==PKT_ACKNOWLEDGE and (rpid&~SYNC_PACKET_ID)==(pid&~SYNC_PACKET_ID):
                    if not acked: kd.send_id^=1; acked=True
                elif ptype==PKT_RESEND: kd._send_data(PKT_MANIPULATE,payload,pid)
                elif ptype==PKT_RESET: kd.send_id=INITIAL_PACKET_ID; pid=kd.send_id; kd._send_data(PKT_MANIPULATE,payload,pid)
                continue
            kd.ack(rpid)
            if ptype in (PKT_STATE_CHANGE32,PKT_STATE_CHANGE64):
                if not acked: kd.send_id^=1
                nxt=parse_state_change64(d); return nxt,r,moved
        if acked and time.time()-t>1.5: break
    return None,r,moved
cur=parse_state_change64(data)
count=0;t0=time.time()
while time.time()-t0<400:
    if cur is None:
        cur=kd.wait_state_change(2.0)
        if cur is None: continue
    pc=cur['pc']&0xffffffff; count+=1
    if pc==BP_RT: print(f"[di] REFLECT HIT #{count}",flush=True); break
    nxt,r,moved=cont1()
    if r:
        s,mem=kd.read_vmem(r['Eip'],6)
        print(f"[di] #{count:3d} pc=0x{pc:08x} EIP=0x{r['Eip']:08x} CS=0x{r['Cs']:04x} SS=0x{r['Ss']:04x} "
              f"ESP=0x{r['Esp']:08x} EBP=0x{r['Ebp']:08x} EFL=0x{r['EFlags']:08x} adv={int(moved)} "
              f"b@EIP={mem.hex() if mem else '??'} nxt={'Y' if nxt else 'N'} cpu={cpu()}",flush=True)
    else:
        print(f"[di] #{count} pc=0x{pc:08x} NO CONTEXT cpu={cpu()}",flush=True)
    cur=nxt
print(f"[di] end: {count} cpu={cpu()}",flush=True)
