#!/usr/bin/env python3
"""
xp.py - run command(s) in the XP test VM over telnet, from the host.

The VM forwards host localhost:2323 -> guest:23 (see xp-vm.sh). Enable the guest
telnet server once with D:\\enable-telnet.cmd, then drive XP from here:

    ./scripts/xp.py "reg query \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\WOW\" /v cmdline"
    ./scripts/xp.py "type C:\\ntvdmex\\vdmhost.log"
    echo -e "cmd1\\ncmd2" | ./scripts/xp.py        # one command per stdin line

Python 3.14 removed telnetlib, so this is a minimal client: it refuses all telnet
options (IAC negotiation) and reads each command's output until the stream goes
quiet. Creds default to ntvdmex/ntvdmex (override via XP_USER/XP_PASS env).
"""
import socket, sys, os, time

HOST = os.environ.get("XP_HOST", "127.0.0.1")
PORT = int(os.environ.get("XP_PORT", "2323"))
USER = os.environ.get("XP_USER", "ntvdmex")
PASS = os.environ.get("XP_PASS", "ntvdmex")

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

def _filter_iac(sock, data, out):
    """Strip telnet IAC sequences from data into out, refusing every option."""
    i = 0; resp = bytearray()
    while i < len(data):
        b = data[i]
        if b == IAC and i + 1 < len(data):
            c = data[i + 1]
            if c in (DO, DONT, WILL, WONT) and i + 2 < len(data):
                opt = data[i + 2]
                reply = {DO: WONT, DONT: WONT, WILL: DONT, WONT: DONT}[c]
                resp += bytes([IAC, reply, opt])
                i += 3; continue
            if c == SB:
                j = i + 2
                while j + 1 < len(data) and not (data[j] == IAC and data[j + 1] == SE):
                    j += 1
                i = j + 2; continue
            i += 2; continue
        out.append(b); i += 1
    if resp:
        sock.sendall(bytes(resp))

def read_quiet(sock, quiet=1.5, hard=40.0, until=None):
    """Read until a `quiet`s gap (or `until` substring appears, or `hard` cap)."""
    out = bytearray(); start = time.time()
    sock.settimeout(quiet)
    while time.time() - start < hard:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            break
        if not data:
            break
        _filter_iac(sock, data, out)
        if until and until in bytes(out).decode("latin1", "replace"):
            break
    return bytes(out).decode("latin1", "replace")

def run(sock, cmd, quiet=1.5, hard=60.0):
    sock.sendall((cmd + "\r\n").encode("latin1", "replace"))
    return read_quiet(sock, quiet=quiet, hard=hard)

def main():
    cmds = [sys.argv[1]] if len(sys.argv) > 1 else \
           [l for l in sys.stdin.read().splitlines() if l.strip()]
    if not cmds:
        print("usage: xp.py \"<cmd>\"  |  echo cmds | xp.py", file=sys.stderr); return 2
    try:
        s = socket.create_connection((HOST, PORT), timeout=10)
    except OSError as e:
        print(f"connect {HOST}:{PORT} failed: {e}\n"
              f"(is the VM up and telnet enabled? run D:\\enable-telnet.cmd)", file=sys.stderr)
        return 1
    read_quiet(s, until="login:", hard=20)
    s.sendall((USER + "\r\n").encode()); read_quiet(s, until="password:", hard=15)
    s.sendall((PASS + "\r\n").encode()); banner = read_quiet(s, hard=15)
    if "login:" in banner.lower() or "incorrect" in banner.lower():
        print("login failed:\n" + banner, file=sys.stderr); return 1
    for c in cmds:
        sys.stdout.write(run(s, c))
        sys.stdout.flush()
    try:
        s.sendall(b"exit\r\n"); s.close()
    except OSError:
        pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
