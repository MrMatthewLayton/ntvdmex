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

def read_quiet(sock, quiet=2.0, hard=40.0, until=None):
    """Read until a `quiet`s gap (or `until` substring appears, or `hard` cap).

    When `until` is given we keep reading through quiet gaps until it shows up
    (or `hard` elapses): the XP telnet prompts can lag by several seconds under
    load, and returning early on the first gap desyncs the login handshake."""
    out = bytearray(); start = time.time()
    sock.settimeout(quiet)
    while time.time() - start < hard:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            if until:            # keep waiting for the prompt, don't give up on a gap
                continue
            break
        if not data:
            break
        _filter_iac(sock, data, out)
        if until and until in bytes(out).decode("latin1", "replace").lower():
            break
    return bytes(out).decode("latin1", "replace")

def run(sock, cmd, prompt=None, quiet=2.0, hard=60.0):
    """Send a command and read its output. If `prompt` (the shell's 'C:\\..>'
    string) is known we read until it reappears -- reliable even when output
    pauses; otherwise we fall back to a quiet-gap heuristic."""
    sock.sendall((cmd + "\r\n").encode("latin1", "replace"))
    return read_quiet(sock, quiet=quiet, hard=hard, until=prompt)

def login_once(cmds):
    """One connect+login+run attempt. Returns (ok, output_str)."""
    try:
        s = socket.create_connection((HOST, PORT), timeout=10)
    except OSError as e:
        return False, f"connect {HOST}:{PORT} failed: {e}"
    try:
        l1 = read_quiet(s, until="login:", hard=25)
        s.sendall((USER + "\r\n").encode()); l2 = read_quiet(s, until="password:", hard=20)
        # Wait for the shell's command prompt ('...>'), not a quiet gap: after auth
        # XP may send a burst of IAC (filtered to nothing) before the prompt, and a
        # gap-based read returns empty too early.
        s.sendall((PASS + "\r\n").encode()); banner = read_quiet(s, until=">", hard=20)
        if os.environ.get("XP_DEBUG"):
            sys.stderr.write(f"[login {len(l1)}B]={l1!r}\n[pwprompt {len(l2)}B]={l2!r}\n")
        bl = banner.lower()
        # Empty/login/incorrect => server refused or auth failed; retry.
        if not banner.strip() or "login:" in bl or "incorrect" in bl:
            return False, "login failed (empty/refused -- session limit?):\n" + banner
        # Extract the prompt (e.g. 'C:\\Documents and Settings\\ntvdmex>') and use it
        # as the end-marker for each command's output.
        prompt = banner.rstrip().splitlines()[-1].strip() if banner.rstrip() else ""
        if not prompt.endswith(">"):
            prompt = None
        if os.environ.get("XP_DEBUG"):
            sys.stderr.write(f"[banner {len(banner)}B]: {banner!r}\n[prompt]={prompt!r}\n")
        out = []
        for c in cmds:
            r = run(s, c, prompt=prompt)
            if os.environ.get("XP_DEBUG"):
                sys.stderr.write(f"[cmd {c!r} -> {len(r)}B]: {r!r}\n")
            out.append(r)
        try:
            s.sendall(b"exit\r\n")
        except OSError:
            pass
        return True, "".join(out)
    finally:
        try: s.close()
        except OSError: pass

def main():
    cmds = [sys.argv[1]] if len(sys.argv) > 1 else \
           [l for l in sys.stdin.read().splitlines() if l.strip()]
    if not cmds:
        print("usage: xp.py \"<cmd>\"  |  echo cmds | xp.py", file=sys.stderr); return 2
    last = ""
    for attempt in range(3):
        ok, out = login_once(cmds)
        if ok:
            sys.stdout.write(out); sys.stdout.flush(); return 0
        last = out
        time.sleep(1.5 * (attempt + 1))       # back off; XP telnet can refuse rapid reconnects
    print(last + "\n(is the VM up and telnet enabled? run D:\\enable-telnet.cmd)", file=sys.stderr)
    return 1

if __name__ == "__main__":
    sys.exit(main())
