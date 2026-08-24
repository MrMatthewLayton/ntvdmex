# Doom: the INT-site patcher was corrupting the game (session 21, 2026-08-24)

`build/` is gitignored, so the runs the session-21 conclusions rest on are kept here,
gzipped. Everything below was measured on the bare-metal XP box.

## The claim

Sessions 16–20 hunted a silent VDM teardown inside Doom's `R_ExecuteSetViewSize`.
It was not Doom. `dpmi_patch_code_region()` rewrites `CD nn` to a BOP (`C4 C4`) because a
raw protected-mode `INT` is the one fault XP will not reflect — and it matched the BYTE
PAIR, with no idea where instructions start. At `obj1+0x3593f` Doom's code reads

```
39 fa   7e cd   31 c9        cmp edx,edi / jle -51 / xor ecx,ecx
        ^^ ^^^^^^^
```

The `cd` is the `jle`'s **displacement** and the `31` is the `xor`'s opcode. Patched, it
became `7e c4  c4 c9`: `R_InitTextureMapping`'s loop-2 back edge jumped to `obj1+0x35905`,
the middle of a `jl`, where the guest executed `cmp ecx,[ebx+0x034fe02d]` with an *angle*
in `ebx`. Wild read, `#PF` at CPL 3, VDM gone — no VEH, no watchdog line, nothing after
the log's last byte.

Two more sites were being corrupted the same way and nobody had noticed:
`obj1+0x0ae0f` (a `call rel32` displacement) and `obj1+0x0512d` (a word in a data table).

## The logs

| file | what it shows |
|---|---|
| `loop2-rep-bp-catches-the-corruption.log.gz` | The run that caught it. Two repeating `pmbp` breakpoints in loop 2 alternate correctly (session 20 believed a `rep` breakpoint could not re-arm inside a BOP-free stretch — with **two** they re-arm each other). The tell is the arm line: `DPMI-BP: armed at linear 0x03b0593f..0x03b05940 (displaced 7e c4)`, where `DOOM.EXE` says `7e cd`. Guest memory did not match the file, and we were the only writer. |
| `demo-plays-with-sound.log.gz` | Doom's attract demo running: full 3D at 320×200 unchained, `sb_blocks=0xc46` at 11025 Hz, `p3da_reads=0x16880` (frames being presented). |
| `keyboard-12-of-12.log.gz` | Scripted keys reaching the client: `sc_push=0xc`, `p60=0xc`, `sc_drop=0`, `sc_left=0`, twelve `IRQ0->PM INT 0x00000009` injections. |
| `doom-menu.png` | The result, off the physical screen: Doom's menu open over the running demo. |

## How the fix was validated

`src/host/x86len.h` decodes instruction *lengths* (16- and 32-bit) and decides whether a
candidate byte pair is an instruction start by decoding forward from each of the preceding
48 bytes and counting how many streams land on it.

Ground truth was `objdump` over Doom's 32-bit code object and DOS/4GW's two 16-bit modules
— 242 candidate `CD nn` byte pairs. To reproduce:

```
python3 -c "d=open('/path/DOOM.EXE','rb').read(); open('d4g16.bin','wb').write(d[0x1DD0:0x1DD0+0x9000])"
i686-w64-mingw32-objdump -D -b binary -m i386 -M intel build/doom_obj1.bin | grep -E 'cd (21|31) .*int'
```

The rule that ships rejects a site **only** when a confirmed instruction covers it *and*
that instruction is a relative branch — five rejections across the three images, every one
hand-checked as a `jmp`/`jle`/`call` displacement, and no real `INT` lost.

⚠ **A stricter rule was tried and is wrong.** "Reject anything a confirmed instruction
covers" also rejects DOS/4GW's `mov ah,30h / int 21h` DOS-version check, which sits
directly after the string `"requires DOS/16M\n\r$"`: every backward anchor decodes ASCII,
so it scores 1 vote in 48 — by votes alone indistinguishable from Doom's `jle` at 3 in 48.
Refusing it left a raw `int 21h` in protected mode and ended the run inside the extender's
own startup, 54,000 log lines earlier than the bug this all exists to fix. **The two errors
are not symmetric**; see the commentary in `x86len.h`.

`tools/dostest/x86len_test.c` pins all four real-world sites byte-for-byte, both widths,
and the truncation cases. It runs in the off-VM battery.
