#!/usr/bin/env python3
# Log NewState (+ module base for load-symbols) for each KD state change, to identify the ~28 wedge.
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
NS={1:"EXCEPTION",2:"?2",3:"LOADSYM",4:"?4"}
kd=KD("vm/kd.sock")
print("[sl] break-in (single byte)...",flush=True)
pidd,data=kd.break_in(secs=300,cadence=400)
print(f"[sl] broke in pc=0x{parse_state_change64(data)['pc']&0xffffffff:08x}",flush=True)
v=kd.get_version(); slide=((v['kernbase']&0xffffffff)-0x400000) if v else 0
kd.clear_breakpoints(); st,h=kd.write_bp(BP_RT)
print(f"[sl] armed h={h}",flush=True)
def decode(d):
    ns=struct.unpack_from("<I",d,0)[0]
    pc=struct.unpack_from("<Q",d,0x18)[0]&0xffffffff
    extra=""
    if ns==3 and len(d)>=0x48:
        base=struct.unpack_from("<Q",d,0x28)[0]&0xffffffff
        unld=d[0x40]
        pathlen=struct.unpack_from("<I",d,0x20)[0]
        extra=f" base=0x{base:08x} unload={unld} pathlen={pathlen}"
    elif ns==1 and len(d)>=0x24:
        exc=struct.unpack_from("<I",d,0x20)[0]
        extra=f" excCode=0x{exc:08x}"
    return ns,pc,extra
def cont1():
    ctx=kd.get_context()
    if ctx and ctx['regs']:
        eip=ctx['regs']['Eip']; s,mem=kd.read_vmem(eip,1)
        if mem and mem[0]==0xCC:
            pat=bytearray(ctx['ctx']); struct.pack_into("<I",pat,0xB8,(eip+1)&0xffffffff); kd.set_context(bytes(pat))
    pid=kd.send_id; payload=kd._build_manip(API_CONTINUE,struct.pack("<i",DBG_CONTINUE))
    kd._send_data(PKT_MANIPULATE,payload,pid); acked=False; t=time.time()
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
                return d
        if acked and time.time()-t>1.5: break
    return None
cur=data; count=0; t0=time.time()
while time.time()-t0<400:
    if cur is None:
        # wedge detection: no state change; probe
        c=cpu()
        got=kd.wait_state_change(2.0)
        if got is None:
            if c>50:
                print(f"[sl] NO STATE CHANGE, cpu={c} -> probing kernel...",flush=True)
                probe=kd.get_context()
                print(f"[sl] probe get_context -> {'HALTED(responds)' if (probe and probe.get('regs')) else 'RUNNING/silent'}; cpu={cpu()}",flush=True)
                time.sleep(3)
            continue
        # got is parsed dict; but we need raw -> re-handle: wait_state_change returns parsed, not raw
        ns=got['new_state']; pc=got['pc']&0xffffffff; count+=1
        if pc==BP_RT: print(f"[sl] REFLECT HIT #{count}",flush=True); break
        print(f"[sl] #{count} NewState={ns}({NS.get(ns,ns)}) pc=0x{pc:08x} (via wait) cpu={cpu()}",flush=True)
        cur=cont1(); continue
    ns,pc,extra=decode(cur); count+=1
    if pc==BP_RT: print(f"[sl] REFLECT HIT #{count}",flush=True); break
    print(f"[sl] #{count:3d} NewState={ns}({NS.get(ns,ns)}) pc=0x{pc:08x}{extra} cpu={cpu()}",flush=True)
    cur=cont1()
print(f"[sl] end: {count} cpu={cpu()}",flush=True)
