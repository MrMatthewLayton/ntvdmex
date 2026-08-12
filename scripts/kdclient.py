#!/usr/bin/env python3
# kdclient.py -- a minimal KDCOM (serial kernel-debug) client for Windows XP SP3 (32-bit),
# spoken over the QEMU-exposed guest COM2 unix socket (vm/kd.sock).
#
# WHY (option B, GH #18): HVF denies QEMU's gdbstub, and radare2's winkd only speaks the
# 64-bit KD structures (it misparses XP's 32-bit context -> eip=0, "cannot retrieve pid").
# But r2 proved the KDCOM serial handshake over vm/kd.sock WORKS. So we implement XP's
# KD protocol ourselves, just enough to break in, read the kernel version (base),
# read/write memory + registers, and set breakpoints -- so we can breakpoint the VDM
# #GP-reflect chain (KiTrap0D -> 0x4f67f8 -> 0x4f6f67 -> 0x4f6e6f) and SEE which gate
# returns 0 for our HLT #GP (runs 65-69 could not observe this).
#
# QEMU must expose COM2 as a RE-ACCEPTING server socket (see xp-vm.sh: -chardev socket,
# id=kddbg,...,server=on,wait=off). A bare `-serial unix:...,server` is single-use.
#
# PROTOCOL (verified against ReactOS drivers/base/kdcom/kddll.c + sdk/include/.../windbgkd.h):
#   * KD_PACKET = <IHHII> = PacketLeader, PacketType, ByteCount, PacketId, Checksum, then
#     `ByteCount` data bytes, then a single PACKET_TRAILING_BYTE (0xAA). Checksum = sum of
#     the data bytes (mod 2^32).
#   * INITIAL_PACKET_ID = 0x80800000, SYNC_PACKET_ID = 0x800. The kernel's very first data
#     packet carries CurrentPacketId = INITIAL|SYNC = 0x80800800 (this is what we saw on
#     the wire). After the first send is ACKed the SYNC bit is cleared, so steady-state
#     ids alternate 0x80800000 <-> 0x80800001 (XOR 1 per acknowledged send).
#   * WE (the debugger) mirror the kernel: our OUTGOING data-packet id starts at
#     INITIAL_PACKET_ID (0x80800000, NO sync bit) because the kernel's RemotePacketId
#     starts at INITIAL and it accepts a data packet ONLY if PacketId == RemotePacketId
#     (kddll.c:290, strict equality). Sending 0x80800800 -> the kernel ACKs but IGNORES it
#     (the bug that stalled the earlier scaffold). After each of our sends is ACKed we
#     toggle the low bit.
#   * When we ACK an inbound data packet we echo its id with the SYNC bit STRIPPED
#     (kddll.c:166 compares the ack id to CurrentPacketId & ~SYNC).
#   * DBGKD_MANIPULATE_STATE64: ApiNumber@0(U32), ProcessorLevel@4(U16), Processor@6(U16),
#     ReturnStatus@8(U32), pad@0xC, union u@0x10. sizeof = 56 (0x38). Appended payload
#     (a CONTEXT, or read-memory bytes) follows at offset 56.
#
# Usage:
#   python3 scripts/kdclient.py version  vm/kd.sock                 # break in, print KernBase
#   python3 scripts/kdclient.py break    vm/kd.sock                 # break in, print halt state
#   python3 scripts/kdclient.py raw      vm/kd.sock                 # dump raw wire bytes
#   python3 scripts/kdclient.py session  vm/kd.sock 0x4f67f8 ...    # persistent bp session (GH#18)
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

INITIAL_PACKET_ID = 0x80800000
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
API_CONTINUE2   = 0x313C
API_GET_VERSION = 0x3146
DBGKD_MANIP_SIZE = 56    # sizeof(DBGKD_MANIPULATE_STATE64); union u begins at offset 0x10

# Exception codes we care about in a STATE_CHANGE
STATUS_BREAKPOINT     = 0x80000003
STATUS_SINGLE_STEP    = 0x80000004
# Continue statuses
DBG_EXCEPTION_HANDLED    = 0x00010001
DBG_CONTINUE             = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001

def cksum(data): return sum(data) & 0xFFFFFFFF

class KD:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.buf = b""
        self.send_id = INITIAL_PACKET_ID   # our outgoing data-packet id
        self.verbose = False

    # ---- raw wire -----------------------------------------------------------
    def send_control(self, ptype, pid):
        self.s.sendall(struct.pack("<IHHII", CONTROL_PACKET_LEADER, ptype, 0, pid, 0))

    def send_breakin(self):
        self.s.sendall(bytes([BREAKIN_BYTE]))

    def _send_data(self, ptype, payload, pid):
        hdr = struct.pack("<IHHII", PACKET_LEADER, ptype, len(payload), pid, cksum(payload))
        self.s.sendall(hdr + payload + bytes([PACKET_TRAILING_BYTE]))

    def ack(self, pid):
        # ACK echoes the received id with the SYNC bit stripped (kddll.c:166).
        self.send_control(PKT_ACKNOWLEDGE, pid & ~SYNC_PACKET_ID)

    def pump(self, timeout=0.3):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(65536)
            if d:
                self.buf += d
                return len(d)
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

    # ---- high-level ---------------------------------------------------------
    def resync(self, secs=4, tries=3):
        """Resync packet IDs to INITIAL by sending a RESET control packet (kddll.c:176: the
        kernel resets CurrentPacketId/RemotePacketId to INITIAL and echoes RESET). Because KD
        state PERSISTS across socket connections, ALWAYS resync on connect -- otherwise our
        send_id is stale and every manipulate is ACKed-but-ignored. Returns the most recent
        STATE_CHANGE (pid, data) the kernel (re)sends if it was already halted, else None.

        We drain any buffered noise first (stray breakin echoes etc.) and retry the RESET a few
        times, because a single short window can miss the echo when the link has backlog."""
        pending = None
        for _ in range(tries):
            self.pump(0.2); self.buf = b""          # flush stale bytes
            self.send_control(PKT_RESET, 0)
            self.send_id = INITIAL_PACKET_ID
            got_reset = False
            deadline = time.time() + secs
            while time.time() < deadline:
                self.pump(0.25)
                for lead, ptype, pid, data in self.parse_buffered():
                    if lead == CONTROL_PACKET_LEADER:
                        if ptype in (PKT_RESET,): self.send_id = INITIAL_PACKET_ID; got_reset = True
                        continue
                    self.ack(pid)
                    if ptype in (PKT_STATE_CHANGE32, PKT_STATE_CHANGE64):
                        pending = (pid, data)
                if pending: return pending
            if got_reset and not pending:
                return None   # link is synced and the kernel is running (no pending halt)
        return pending

    def break_in(self, secs=260, verbose=None, cadence=90.0):
        """Ensure the kernel is halted and return its (pid, data) STATE_CHANGE. First resync
        (which, if the kernel is already halted, hands back its pending state change without a
        single breakin byte). Only if the kernel is running do we send breakin bytes -- and
        SPARINGLY (one every `cadence` s), because each buffered breakin byte the guest later
        drains triggers a fresh break at RtlpBreakWithStatusInstruction.

        CADENCE MATTERS ON A SLOW VM: this throttled HVF guest polls the break-in byte only
        ~once per second of (heavily-dilated) GUEST time, so a real break-in lands ~100-120s
        after the FIRST byte -- sending more bytes does NOT speed it up (the first queued byte
        halts as soon as KdPollBreakIn next runs). At the old 0.4s cadence that is ~275 queued
        bytes, each of which re-breaks on resume; piled on repeated RESETs it WEDGES KDCOM into
        an unresumable halt (kernel spins at 100% CPU, RESET no longer echoes -> reboot needed).
        Only the FIRST byte matters (it halts ~110s later); extra bytes just pile into the
        backlog and, drained over minutes, the churn CRASHED QEMU (session 3). So keep the
        cadence VERY SPARSE (default 90s -> ~1 queued byte over a 110s wait) and be patient."""
        if verbose is None: verbose = self.verbose
        # If the kernel is already halted, resync hands back its pending state change at once.
        pending = self.resync(secs=1, tries=1)
        if pending: return pending
        # Otherwise it is running: GENTLE, WinDbg-style break-in. Send a SINGLE breakin byte at a
        # SPARSE cadence and pump patiently for the STATE_CHANGE. NO byte-bursts and NO repeated
        # RESET packets in this loop -- those churn/corrupt XP's KDCOM state machine and eventually
        # bugcheck the kernel over an extended session (observed: box rebooted from break-in churn
        # alone). The kernel RETRANSMITS an un-ACKed state change on its own retry timer, so if we
        # miss the first we simply catch a retransmit -- no RESET needed to reclaim it.
        deadline = time.time() + secs; last_break = 0.0
        while time.time() < deadline:
            now = time.time()
            if now - last_break > cadence:
                self.send_breakin(); last_break = now      # ONE byte, sparse cadence
            self.pump(0.2)
            for lead, ptype, pid, data in self.parse_buffered():
                if lead == CONTROL_PACKET_LEADER:
                    if ptype == PKT_RESET: self.send_id = INITIAL_PACKET_ID
                    continue
                self.ack(pid)
                if ptype in (PKT_STATE_CHANGE32, PKT_STATE_CHANGE64):
                    return pid, data
        raise TimeoutError("no STATE_CHANGE after breakin")

    def _build_manip(self, api, body=b""):
        msg = bytearray(DBGKD_MANIP_SIZE)
        struct.pack_into("<IHHI", msg, 0, api, 0, 0, 0)   # ApiNumber, ProcLevel, Processor, Status
        msg[0x10:0x10 + len(body)] = body                 # union u begins at offset 0x10
        return bytes(msg)

    def manipulate(self, api, body=b"", extra=b"", timeout=8):
        """Send a DBGKD_MANIPULATE_STATE64 request, wait for our ACK, then read the kernel's
        manipulate response (which we must ACK). Returns the response bytes (56-byte header
        + any appended payload), or None on timeout."""
        payload = self._build_manip(api, body) + extra
        pid = self.send_id
        self._send_data(PKT_MANIPULATE, payload, pid)
        deadline = time.time() + timeout
        resp = None; acked = False
        while time.time() < deadline and resp is None:
            self.pump(0.3)
            for lead, ptype, rpid, rdata in self.parse_buffered():
                if lead == CONTROL_PACKET_LEADER:
                    if ptype == PKT_ACKNOWLEDGE and (rpid & ~SYNC_PACKET_ID) == (pid & ~SYNC_PACKET_ID):
                        acked = True
                    elif ptype in (PKT_RESEND, PKT_RESET):
                        if ptype == PKT_RESET: self.send_id = INITIAL_PACKET_ID; pid = self.send_id
                        self._send_data(PKT_MANIPULATE, payload, pid)   # resend
                    continue
                # inbound data packet: ACK it, and capture a MANIPULATE response
                self.ack(rpid)
                if ptype == PKT_MANIPULATE:
                    resp = rdata
        if acked or resp is not None:
            # If we got a response the kernel accepted (and advanced its RemotePacketId),
            # so we MUST advance too -- even if we never cleanly saw the transport ACK.
            self.send_id ^= 1
        return resp

    def cont(self, status=DBG_CONTINUE):
        """DbgKdContinueApi -- MUST be sent before disconnecting or XP freezes (the kernel
        stays halted). No manipulate reply is expected, so send-and-advance best-effort."""
        try:
            pid = self.send_id
            self._send_data(PKT_MANIPULATE, self._build_manip(API_CONTINUE, struct.pack("<i", status)), pid)
            # drain a possible ACK so the id stays in sync; ignore timeouts
            end = time.time() + 1.0
            while time.time() < end:
                self.pump(0.2)
                for lead, ptype, rpid, _ in self.parse_buffered():
                    if lead == CONTROL_PACKET_LEADER and ptype == PKT_ACKNOWLEDGE:
                        self.send_id ^= 1; return
        except Exception:
            pass

    # ---- typed manipulate helpers ------------------------------------------
    def get_version(self):
        resp = self.manipulate(API_GET_VERSION)
        if not resp or len(resp) < DBGKD_MANIP_SIZE: return None
        u = resp[0x10:]
        (majv, minv, protv, secv, flags, machine, maxpkt, maxsc, maxman, sim, unused,
         kernbase, psload, ddata) = struct.unpack_from("<HHBBHHBBBBHQQQ", u, 0)
        return dict(major=majv, minor=minv, machine=machine, kernbase=kernbase & 0xffffffffffffffff,
                    psloaded=psload, ddata=ddata, status=struct.unpack_from("<I", resp, 8)[0])

    def read_vmem(self, addr, n):
        """DbgKdReadVirtualMemoryApi. DBGKD_READ_MEMORY64 = TargetBaseAddress(Q), TransferCount(I),
        ActualBytesRead(I). Response appends the bytes at offset 56."""
        body = struct.pack("<QII", addr & 0xffffffffffffffff, n, 0)
        resp = self.manipulate(API_READ_VMEM, body)
        if not resp: return None, None
        status = struct.unpack_from("<I", resp, 8)[0]
        actual = struct.unpack_from("<I", resp, 0x10 + 12)[0] if len(resp) >= 0x1c else 0
        data = resp[DBGKD_MANIP_SIZE:DBGKD_MANIP_SIZE + actual]
        return status, data

    def get_context(self):
        """DbgKdGetContextApi. Response appends a full x86 CONTEXT (0x2CC bytes) at offset 56."""
        resp = self.manipulate(API_GET_CONTEXT)
        if not resp: return None
        status = struct.unpack_from("<I", resp, 8)[0]
        ctx = resp[DBGKD_MANIP_SIZE:]
        return dict(status=status, ctx=ctx, regs=parse_x86_context(ctx))

    def set_context(self, ctx_bytes):
        """DbgKdSetContextApi. The kernel asserts Data->Length == sizeof(CONTEXT) and requires
        a prior GetContext (KdpContextSent). ctx_bytes must be the full 716-byte x86 CONTEXT."""
        body = struct.pack("<I", struct.unpack_from("<I", ctx_bytes, 0)[0])  # ContextFlags (informational)
        resp = self.manipulate(API_SET_CONTEXT, body, extra=ctx_bytes)
        if not resp: return None
        return struct.unpack_from("<I", resp, 8)[0]   # ReturnStatus

    def resume(self):
        """Resume the guest. The initial break-in lands the trap frame's EIP directly on the
        0xCC of RtlpBreakWithStatusInstruction, so a plain Continue just re-executes it and
        re-breaks. Detect that (byte@EIP == 0xCC) and step EIP past it via SetContext before
        continuing -- exactly what WinDbg does."""
        ctx = self.get_context()
        if ctx and ctx['regs']:
            eip = ctx['regs']['Eip']
            st, mem = self.read_vmem(eip, 1)
            if mem and mem[0] == 0xCC:
                patched = bytearray(ctx['ctx'])
                struct.pack_into("<I", patched, 0xB8, (eip + 1) & 0xffffffff)   # Eip += 1
                self.set_context(bytes(patched))
        self.cont()

    def write_bp(self, addr):
        """DbgKdWriteBreakPointApi. DBGKD_WRITE_BREAKPOINT64 = BreakPointAddress(Q), Handle(I).
        Returns the breakpoint handle (needed to restore it)."""
        body = struct.pack("<QI", addr & 0xffffffffffffffff, 0)
        resp = self.manipulate(API_WRITE_BP, body)
        if not resp: return None, None
        status = struct.unpack_from("<I", resp, 8)[0]
        handle = struct.unpack_from("<I", resp, 0x10 + 8)[0]
        return status, handle

    def restore_bp(self, handle):
        body = struct.pack("<I", handle)
        return self.manipulate(API_RESTORE_BP, body)

    def clear_breakpoints(self):
        """Restore every KD breakpoint slot (1..32). KdpAddBreakpoint fails (handle 0 ->
        STATUS_UNSUCCESSFUL) when the table is full of stale entries, so clear it before arming."""
        n = 0
        for h in range(1, 33):
            r = self.manipulate(API_RESTORE_BP, struct.pack("<I", h), timeout=1.5)
            if r and struct.unpack_from("<I", r, 8)[0] == 0: n += 1
        return n

    def cont2(self, status=DBG_CONTINUE, trace=False, dr7=0):
        """DbgKdContinueApi2 with an X86 control set: TraceFlag=1 single-steps one instruction.
        DBGKD_CONTINUE2: ContinueStatus@0x10, then X86_DBGKD_CONTROL_SET {TraceFlag, Dr7,
        CurrentSymbolStart, CurrentSymbolEnd}."""
        body = struct.pack("<iIIII", status, 1 if trace else 0, dr7, 0, 0)
        pid = self.send_id
        self._send_data(PKT_MANIPULATE, self._build_manip(API_CONTINUE2, body), pid)
        end = time.time() + 1.0
        while time.time() < end:
            self.pump(0.2)
            for lead, ptype, rpid, _ in self.parse_buffered():
                if lead == CONTROL_PACKET_LEADER and ptype == PKT_ACKNOWLEDGE:
                    self.send_id ^= 1; return

    def wait_state_change(self, secs=8):
        """Block until the kernel reports a STATE_CHANGE (e.g. after a single-step or a
        breakpoint hit); ACK it and return the parsed dict, or None on timeout."""
        deadline = time.time() + secs
        while time.time() < deadline:
            self.pump(0.3)
            for lead, ptype, pid, data in self.parse_buffered():
                if lead == CONTROL_PACKET_LEADER: continue
                self.ack(pid)
                if ptype in (PKT_STATE_CHANGE32, PKT_STATE_CHANGE64):
                    return parse_state_change64(data)
        return None

    def single_step(self, secs=8):
        """Step one instruction (via TraceFlag) and return (state_change_dict, regs)."""
        self.cont2(trace=True)
        sc = self.wait_state_change(secs)
        if not sc: return None, None
        ctx = self.get_context()
        return sc, (ctx['regs'] if ctx else None)


# x86 CONTEXT (winnt.h): ContextFlags@0, Dr0..Dr7@4, FloatSave@0x1C(0x70),
# SegGs@0x8C SegFs@0x90 SegEs@0x94 SegDs@0x98, Edi@0x9C Esi@0xA0 Ebx@0xA4 Edx@0xA8
# Ecx@0xAC Eax@0xB0 Ebp@0xB4 Eip@0xB8 SegCs@0xBC EFlags@0xC0 Esp@0xC4 SegSs@0xC8.
def parse_x86_context(ctx):
    if len(ctx) < 0xCC: return None
    g = lambda off: struct.unpack_from("<I", ctx, off)[0]
    return dict(ContextFlags=g(0x00), Dr6=g(0x14), Dr7=g(0x18),
                Gs=g(0x8C), Fs=g(0x90), Es=g(0x94), Ds=g(0x98),
                Edi=g(0x9C), Esi=g(0xA0), Ebx=g(0xA4), Edx=g(0xA8), Ecx=g(0xAC), Eax=g(0xB0),
                Ebp=g(0xB4), Eip=g(0xB8), Cs=g(0xBC), EFlags=g(0xC0), Esp=g(0xC4), Ss=g(0xC8))


def parse_state_change64(data):
    # DBGKD_ANY_WAIT_STATE_CHANGE (XP+): NewState:u32@0, ProcLevel:u16@4, Proc:u16@6,
    # NumberProcessors:u32@8, Thread:u64@0x10, ProgramCounter:u64@0x18, then the
    # exception union @0x20 (DBGKM_EXCEPTION64: EXCEPTION_RECORD64 then FirstChance;
    # EXCEPTION_RECORD64.ExceptionCode is @0x20).
    new_state, plevel, proc, nproc = struct.unpack_from("<IHHI", data, 0)
    thread, pc = struct.unpack_from("<QQ", data, 0x10)
    exc_code = struct.unpack_from("<I", data, 0x20)[0] if len(data) >= 0x24 else 0
    return dict(new_state=new_state, processor=proc, nproc=nproc,
                thread=thread & 0xffffffffffffffff, pc=pc & 0xffffffffffffffff,
                exception=exc_code)


# ---------------------------------------------------------------------------
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
        pid, data = kd.break_in()
        sc = parse_state_change64(data)
        print(f"[kd] HALTED: state=0x{sc['new_state']:x} proc={sc['processor']} "
              f"thread=0x{sc['thread']:016x} PC=0x{sc['pc']:016x} exc=0x{sc['exception']:08x} "
              f"datalen={len(data)}")
    finally:
        kd.resume()   # EIP-step past the break-in int3, else the guest stays frozen

def do_version(path):
    kd = KD(path); print(f"[kd] connected {path}")
    try:
        pid, data = kd.break_in()
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
        kd.resume()   # EIP-step past the break-in int3, else the guest stays frozen

def _dump_regs(kd, r, indent="      "):
    print(f"{indent}EIP=0x{r['Eip']:08x} CS=0x{r['Cs']:04x} EFLAGS=0x{r['EFlags']:08x} "
          f"ESP=0x{r['Esp']:08x}")
    print(f"{indent}EAX=0x{r['Eax']:08x} EBX=0x{r['Ebx']:08x} ECX=0x{r['Ecx']:08x} "
          f"EDX=0x{r['Edx']:08x}")
    print(f"{indent}ESI=0x{r['Esi']:08x} EDI=0x{r['Edi']:08x} EBP=0x{r['Ebp']:08x}")
    print(f"{indent}DS=0x{r['Ds']:04x} ES=0x{r['Es']:04x} FS=0x{r['Fs']:04x} SS=0x{r['Ss']:04x}")

def do_session(path, addrs, steps=0):
    """The GH#18 frontier: ONE persistent session. Break in, get KernBase, set breakpoints at
    the reflect chain (static RVAs off 0x400000 -> runtime = KernBase + RVA), resume, then on
    each hit dump the CPU state + code so we SEE which gate rejects our PM #GP. If `steps` > 0,
    single-step that many instructions after a hit (logging rebased EIP+EAX each step) to trace
    the exact path through 0x4f67f8 -> 0x4f6f67 -> ... and spot the `return 0`.

    `addrs` are STATIC VAs (r2 base 0x400000); rebased to the live kernel. Trigger the fault in
    the VM (double-click D:\\pfrun.bat) after 'breakpoints armed'."""
    kd = KD(path); print(f"[kd] connected {path}")
    slide = 0
    handles = {}   # runtime addr -> breakpoint handle
    def rb(rt):  # runtime VA -> static VA for readable logging
        return (rt - slide) & 0xffffffff
    try:
        pid, data = kd.break_in()
        print(f"[kd] HALTED at PC=0x{parse_state_change64(data)['pc']:08x}")
        v = kd.get_version()
        if not v or not v['kernbase']:
            print("[kd] GetVersion failed; cannot rebase breakpoints"); return
        base = v['kernbase'] & 0xffffffff
        slide = base - 0x400000
        print(f"[kd] KernBase=0x{base:08x} (slide=0x{slide:08x} vs r2 base 0x400000)")
        cleared = kd.clear_breakpoints()
        print(f"[kd] cleared {cleared} stale breakpoint slot(s)")
        for a in addrs:
            rt = (a + slide) & 0xffffffff
            status, h = kd.write_bp(rt)
            print(f"[kd] bp @ static 0x{a:06x} -> runtime 0x{rt:08x}  status=0x{status:08x} handle={h}")
            if h is not None: handles[rt] = h
        print(f"[kd] {len(handles)} breakpoint(s) armed. Resuming.")
        print("[kd] >>> NOW trigger the fault in the VM: double-click D:\\pfrun.bat <<<")
        kd.resume()
        deadline = time.time() + 600
        hits = 0
        while time.time() < deadline:
            sc = kd.wait_state_change(1.0)
            if not sc: continue
            # Ignore spurious breaks that are NOT our reflect breakpoint -- chiefly the break-in
            # int3 at RtlpBreakWithStatusInstruction from leftover breakin bytes. Just step past
            # them (this also drains the breakin backlog) and keep waiting for the real hit.
            if sc['pc'] not in handles:
                kd.resume()
                continue
            hits += 1
            static_pc = rb(sc['pc'])
            print(f"\n[kd] *** HIT #{hits}  PC=0x{sc['pc']:08x} (static 0x{static_pc:06x})  "
                  f"exc=0x{sc['exception']:08x} ***")
            ctx = kd.get_context()
            if ctx and ctx['regs']:
                _dump_regs(kd, ctx['regs'])
                st, mem = kd.read_vmem(ctx['regs']['Eip'], 16)
                if mem: print(f"      code@EIP: {mem.hex()}")
            # To move past this KD breakpoint, restore it (puts the original byte back), then
            # optionally single-step to trace the path; finally resume.
            if sc['pc'] in handles:
                kd.restore_bp(handles.pop(sc['pc']))
            for i in range(steps):
                ssc, sr = kd.single_step()
                if not sr: print(f"      step {i}: no state change"); break
                print(f"      step {i:3d}: EIP=0x{sr['Eip']:08x} (static 0x{rb(sr['Eip']):06x}) "
                      f"EAX=0x{sr['Eax']:08x} exc=0x{ssc['exception']:08x}")
            kd.resume()
        print(f"\n[kd] session window elapsed; {hits} hit(s).")
    finally:
        # remove any surviving breakpoints, then resume cleanly (else the guest freezes).
        for rt, h in list(handles.items()):
            try: kd.restore_bp(h)
            except Exception: pass
        kd.resume()


# Default = ONLY the reflect ENTRY 0x4f67f8 (KiTrap0D's VDM non-BOP #GP reflect, session 4/5),
# then single-step into the body. DANGER (learned the hard way -> instant bugcheck+reboot): DO NOT
# breakpoint the gate subroutines 0x4f6d3c / 0x4f6dc0 (via 0x45dd5f) -- they are called constantly
# by NORMAL kernel selector validation, so a 0xCC there floods KD at raised IRQL and bugchecks XP.
# 0x4f67f8 is only reached for a VDM fault (EPROCESS.VdmObjects!=0), so it stays quiet until pfrun.
DEFAULT_REFLECT = [0x4f67f8]

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "break"
    path = sys.argv[2] if len(sys.argv) > 2 else "vm/kd.sock"
    if mode == "session":
        # optional trailing "steps=N" to single-step-trace after each hit
        steps = 0
        rest = []
        for x in sys.argv[3:]:
            if x.startswith("steps="): steps = int(x.split("=", 1)[1], 0)
            else: rest.append(x)
        addrs = [int(x, 0) for x in rest] or DEFAULT_REFLECT
        do_session(path, addrs, steps=steps)
    else:
        {"raw": do_raw, "break": do_break, "version": do_version}.get(mode, do_break)(path)
