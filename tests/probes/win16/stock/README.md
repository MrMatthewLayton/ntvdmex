# Stock baselines for the Win16 probes (13)

Each file is what XP's own NTVDM/WOW answered (`tests/probes/win16/stock.sh <probe>`), on the
rig (192.168.1.29, XP SP3), with only the `#PROBE`/`CASE=`/`#END` lines kept. Compare a
fresh run of ours against one without a stock pass:

    tests/probes/win16/run.sh w_misc > /tmp/ours.txt
    diff <(grep CASE= /tmp/ours.txt) <(grep CASE= tests/probes/win16/stock/w_misc.txt)

⚠ ONE MACHINE'S ANSWER. Counts that depend on the box (fonts installed, drives, the
OEM code page -- `kbcp` is 0x352 = 850 here) are this rig's. Re-measure after changing
the rig, never edit by hand.

Recorded s90 (2026-10-03): w_kmem 16, w_kstr 18, w_kfile 22, w_user 23, w_gdi 20 (the
#271 debt, all equal to ours), w_kprof 12, w_sound 23, w_genum 28, w_misc 44, w_props 20,
w_gthunk 16. w_cwd and w_kernel are from earlier sessions.
