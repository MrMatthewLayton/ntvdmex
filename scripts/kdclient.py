#!/usr/bin/env python3
# kdclient.py -- a minimal KDCOM (serial kernel-debug) client for Windows XP SP3 (32-bit),
# spoken over the QEMU-exposed guest COM2 unix socket (vm/kd.sock).
#
# WHY (option B, GH #18): HVF denies QEMU's gdbstub, and radare2's winkd only speaks the
# 64-bit KD structures (it misparses XP's 32-bit context -> eip=0, "cannot retrieve pid").
# But r2 proved the KDCOM serial handshake over vm/kd.sock WORKS. So we implement XP's
# 32-bit KD protocol ourselves, just enough to break in, read the kernel version (base),
# read/write memory + registers, and set breakpoints -- so we can breakpoint the VDM
# #GP-reflect chain (KiTrap0D -> 0x4f67f8 -> 0x4f6f67 -> 0x4f6e6f) and SEE which gate
# returns 0 for our HLT #GP (runs 65-69 could not observe this).
#
# QEMU must expose COM2 as a RE-ACCEPTING server socket (see xp-vm.sh: -chardev socket,
# id=kddbg,...,server=on,wait=off). A bare `-serial unix:...,server` is single-use.
#
# Protocol refs: ReactOS windbgkd.h / kd64 / kdcom. XP 32-bit uses DBGKD_*32 + STATE_CHANGE32.
#
# Usage:  python3 scripts/kdclient.py raw    vm/kd.sock   # dump raw wire bytes after breakin
#         python3 scripts/kdclient.py break  vm/kd.sock   # break in, print halt state
import socket, struct, sys, time

PACKET_LEADER         = 0x30303030   # data packet ("0000")
CONTROL_PACKET_LEADER = 0x69696969   # control packet ("iiii")
PACKET_TRAILING_BYTE  = 0xAA
BREAKIN_BYTE          = 0x62         # 'b' -- request a break-in

PKT_STATE_CHANGE32 = 1
PKT_MANIPULATE     = 2
PKT_DEBUG_IO       = 3
PKT_ACKNOWLEDGE    = 4
PKT_RESEND         = 5
PKT_RESET          = 6
PKT_STATE_CHANGE64 = 7    # XP+ uses the "64" state-change variant (confirmed on the wire)

INITIAL_PACKET_ID = 0x80800800
SYNC_PACKET_ID    = 0x00000800

# DbgKd*Api numbers (state-manipulate)
API_READ_VMEM   = 0x3130
API_WRITE_VMEM  = 0x3131
API_GET_CONTEXT = 0x3132
API_SET_CONTEXT = 0x3133
API_WRITE_BP    = 0x3134
API_RESTORE_BP  = 0x3135
API_CONTINUE    = 0x3136
API_READ_CTRL   = 0x3137
API_GET_VERSION = 0x3146
DBGKD_MANIP_SIZE = 56    # sizeof(DBGKD_MANIPULATE_STATE64)

def cksum(data): return sum(data) & 0xFFFFFFFF

class KD:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.buf = b""
        self.next_id = INITIAL_PACKET_ID

    def send_control(self, ptype, pid):
        self.s.sendall(struct.pack("<IHHII", CONTROL_PACKET_LEADER, ptype, 0, pid, 0))

    def send_breakin(self):
        self.s.sendall(bytes([BREAKIN_BYTE]))

    def send_data(self, ptype, data):
        pid = self.next_id
        hdr = struct.pack("<IHHII", PACKET_LEADER, ptype, len(data), pid, cksum(data))
        self.s.sendall(hdr + data + bytes([PACKET_TRAILING_BYTE]))
        return pid

    def ack(self, pid): self.send_control(PKT_ACKNOWLEDGE, pid)

    def pump(self, timeout=0.3):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(65536)
            if d: self.buf += d; return len(d)
        except socket.timeout:
            pass
        return 0

    def parse_buffered(self):
        """Yield complete packets already buffered: (leader, ptype, pid, data)."""
        out = []
        while True:
            idx = -1
            for cand in (PACKET_LEADER, CONTROL_PACKET_LEADER):
                p = self.buf.find(struct.pack("<I", cand))
                if p != -1 and (idx == -1 or p < idx): idx = p
            if idx == -1:
                if len(self.buf) > 3: self.buf = self.buf[-3:]
                break
            if idx > 0: self.buf = self.buf[idx:]
            if len(self.buf) < 16: break
            lead, ptype, bytecount, pid, chk = struct.unpack_from("<IHHII", self.buf, 0)
            if lead == CONTROL_PACKET_LEADER:
                out.append((lead, ptype, pid, b"")); self.buf = self.buf[16:]; continue
            need = 16 + bytecount + 1
            if len(self.buf) < need: break
            data = self.buf[16:16 + bytecount]; self.buf = self.buf[need:]
            out.append((lead, ptype, pid, data))
        return out

    def break_in(self, secs=25, verbose=True):
        """Break into a RUNNING kernel: spam breakin while pumping until a STATE_CHANGE
        arrives (WinDbg approach). ACK it; return (pid, data)."""
        deadline = time.time() + secs; last = 0
        while time.time() < deadline:
            now = time.time()
            if now - last > 0.5: self.send_breakin(); last = now
            self.pump(0.3)
            for lead, ptype, pid, data in self.parse_buffered():
                if verbose:
                    print(f"  <- {'CTRL' if lead==CONTROL_PACKET_LEADER else 'DATA'} "
                          f"type={ptype} pid=0x{pid:08x} len={len(data)}")
                if lead == CONTROL_PACKET_LEADER:
                    if ptype == PKT_RESET: self.next_id = INITIAL_PACKET_ID
                    continue
                self.ack(pid)
                if ptype in (PKT_STATE_CHANGE32, PKT_STATE_CHANGE64):
                    self.cur_id = pid
                    return pid, data
        raise TimeoutError("no STATE_CHANGE after breakin")

    def manipulate(self, api, body=b"", extra=b"", timeout=8):
        """Send a DBGKD_MANIPULATE_STATE64 request (api + body padded to 56 bytes, then
        `extra` payload), wait for the ACK, then read the kernel's manipulate response.
        Returns the response data (56-byte header + any trailing payload)."""
        hdr = struct.pack("<IHHI", api, 0, 0, 0) + body           # ApiNumber, ProcLevel, Proc, Status, body
        hdr = hdr[:DBGKD_MANIP_SIZE].ljust(DBGKD_MANIP_SIZE, b"\x00")
        payload = hdr + extra
        # KDCOM shares one packet-id sequence; reuse the id the kernel last used, toggling low bit.
        pid = self.next_id
        self.s.sendall(struct.pack("<IHHII", PACKET_LEADER, PKT_MANIPULATE, len(payload), pid, cksum(payload))
                       + payload + bytes([PACKET_TRAILING_BYTE]))
        # collect: expect an ACK for our packet, then a MANIPULATE response we must ACK.
        deadline = time.time() + timeout; resp = None
        while time.time() < deadline and resp is None:
            self.pump(0.3)
            for lead, ptype, rpid, rdata in self.parse_buffered():
                if lead == CONTROL_PACKET_LEADER:
                    if ptype == PKT_RESEND:      # id mismatch: flip and resend
                        self.next_id ^= 1
                        pid = self.next_id
                        self.s.sendall(struct.pack("<IHHII", PACKET_LEADER, PKT_MANIPULATE, len(payload), pid, cksum(payload))
                                       + payload + bytes([PACKET_TRAILING_BYTE]))
                    continue
                self.ack(rpid)
                if ptype == PKT_MANIPULATE:
                    resp = rdata
                    break
        self.next_id ^= 1
        return resp

    def cont(self, status=0x00010002):
        """DbgKdContinueApi -- MUST be sent before disconnecting or XP freezes (the kernel
        stays halted). ContinueStatus = DBG_CONTINUE. No reply is expected; best-effort."""
        try:
            self.manipulate(API_CONTINUE, struct.pack("<I", status), timeout=1)
        except Exception:
            pass

    def get_version(self):
        resp = self.manipulate(API_GET_VERSION)
        if not resp or len(resp) < DBGKD_MANIP_SIZE: return None
        # DBGKD_GET_VERSION64 sits in the union at offset 0x10 of the manipulate struct.
        u = resp[0x10:]
        (majv, minv, protv, secv, flags, machine, maxpkt, maxsc, maxman, sim, unused,
         kernbase, psload, ddata) = struct.unpack_from("<HHBBHHBBBBHQQQ", u, 0)
        return dict(major=majv, minor=minv, machine=machine, kernbase=kernbase,
                    psloaded=psload, ddata=ddata, status=struct.unpack_from("<I", resp, 8)[0])

def parse_state_change64(data):
    # DBGKD_ANY_WAIT_STATE_CHANGE (XP+): NewState:u32, ProcLevel:u16, Proc:u16,
    # NumberProcessors:u32, (pad), Thread:u64@0x10, ProgramCounter:u64@0x18, ...
    new_state, plevel, proc, nproc = struct.unpack_from("<IHHI", data, 0)
    thread, pc = struct.unpack_from("<QQ", data, 0x10)
    return dict(new_state=new_state, processor=proc, nproc=nproc,
                thread=thread & 0xffffffffffffffff, pc=pc & 0xffffffffffffffff)

def do_raw(path):
    kd = KD(path); print(f"[kd] connected {path}; spamming breakin, dumping raw 12s")
    end = time.time() + 12; last = 0; total = b""
    while time.time() < end:
        if time.time() - last > 0.5: kd.send_breakin(); last = time.time()
        n = kd.pump(0.3)
        if n: total += kd.buf; kd.buf = b""
    print(f"[kd] received {len(total)} bytes"); print(total[:400].hex())

def do_break(path):
    kd = KD(path); print(f"[kd] connected {path}")
    try:
        pid, data = kd.break_in(verbose=False)
        sc = parse_state_change64(data)
        print(f"[kd] HALTED: state=0x{sc['new_state']:x} proc={sc['processor']} "
              f"thread=0x{sc['thread']:016x} PC=0x{sc['pc']:016x} datalen={len(data)}")
    finally:
        kd.cont()   # ALWAYS continue or XP freezes

def do_version(path):
    kd = KD(path); print(f"[kd] connected {path}")
    try:
        pid, data = kd.break_in(verbose=False)
        sc = parse_state_change64(data)
        print(f"[kd] HALTED at PC=0x{sc['pc']:016x}")
        v = kd.get_version()
        if not v:
            print("[kd] GetVersion: no/short response"); return
        print(f"[kd] GetVersion: status=0x{v['status']:08x} major={v['major']} minor={v['minor']} "
              f"machine=0x{v['machine']:x}")
        print(f"[kd] KernBase=0x{v['kernbase']:016x}  PsLoadedModuleList=0x{v['psloaded']:016x}")
        if v['kernbase']:
            base = v['kernbase'] & 0xffffffff
            print(f"[kd] ntoskrnl runtime base = 0x{base:08x}  =>  0x4f67f8 (reflect) at "
                  f"0x{base + (0x4f67f8 - 0x400000):08x}")
    finally:
        kd.cont()   # ALWAYS continue or XP freezes

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "break"
    path = sys.argv[2] if len(sys.argv) > 2 else "vm/kd.sock"
    {"raw": do_raw, "break": do_break, "version": do_version}.get(mode, do_break)(path)
