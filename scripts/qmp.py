#!/usr/bin/env python3
"""Tiny QMP client for the dev VM. Usage:
    qmp.py screendump /tmp/x.ppm
    qmp.py cd <abs-iso-path> [label]      # hot-swap the CD (ide1-cd0)
    qmp.py eject                          # eject the CD
    qmp.py cmd '{"execute":"query-status"}'
"""
import json, socket, sys, time

SOCK = __file__.rsplit("/", 2)[0] + "/vm/qmp.sock"

def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    f = s.makefile("rwb", buffering=0)
    f.readline()                                   # greeting
    f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush()
    f.readline()                                   # caps reply
    return s, f

def do(f, obj):
    f.write((json.dumps(obj) + "\n").encode()); f.flush()
    while True:
        line = f.readline()
        if not line:
            return None
        d = json.loads(line)
        if "return" in d or "error" in d:
            return d                                # skip async events

def main():
    a = sys.argv[1:]
    s, f = connect()
    if a[0] == "screendump":
        print(do(f, {"execute": "screendump", "arguments": {"filename": a[1]}}))
    elif a[0] == "eject":
        print(do(f, {"execute": "eject", "arguments": {"device": "ide1-cd0", "force": True}}))
    elif a[0] == "cd":
        iso = a[1]
        print("eject:", do(f, {"execute": "eject", "arguments": {"device": "ide1-cd0", "force": True}}))
        time.sleep(3)
        print("change:", do(f, {"execute": "blockdev-change-medium",
              "arguments": {"device": "ide1-cd0", "filename": iso, "format": "raw"}}))
    elif a[0] == "key":
        keys = [{"type": "qcode", "data": k} for k in a[1:]]
        print(do(f, {"execute": "send-key", "arguments": {"keys": keys}}))
    elif a[0] == "type":                                # type an ASCII string
        m = {":": ("shift", "semicolon"), "\\": ("backslash",), ".": ("dot",),
             "_": ("shift", "minus"), "-": ("minus",), " ": ("spc",), "/": ("slash",)}
        for ch in a[1]:
            if ch in m:
                ks = m[ch]
            elif ch.isupper():
                ks = ("shift", ch.lower())
            else:
                ks = (ch,)
            do(f, {"execute": "send-key", "arguments":
                   {"keys": [{"type": "qcode", "data": k} for k in ks]}})
            time.sleep(0.15)
        print("typed", repr(a[1]))
    elif a[0] == "click":                              # click at native px x y (screen 1024x768)
        W, H = 1024, 768
        px, py = int(a[1]), int(a[2])
        ax, ay = px * 32767 // W, py * 32767 // H
        do(f, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]}})
        do(f, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]}})
        do(f, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]}})
        print("clicked", px, py)
    elif a[0] == "drag":                               # drag x1 y1 -> x2 y2 (native px)
        W, H = 1024, 768
        def move(px, py):
            do(f, {"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": px * 32767 // W}},
                {"type": "abs", "data": {"axis": "y", "value": py * 32767 // H}}]}})
        x1, y1, x2, y2 = map(int, a[1:5])
        move(x1, y1)
        do(f, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]}})
        for k in range(1, 9):                          # glide so the WM tracks the move
            move(x1 + (x2 - x1) * k // 8, y1 + (y2 - y1) * k // 8)
            time.sleep(0.05)
        do(f, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]}})
        print("dragged", (x1, y1), "->", (x2, y2))
    elif a[0] == "cmd":
        print(do(f, json.loads(a[1])))
    s.close()

if __name__ == "__main__":
    main()
