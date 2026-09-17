/* nodev.dylib -- make opendev() fail instantly, by DYLD interposition.
 *
 * PCem's wx frontend enumerates host optical drives in wx_initmenu() to build its
 * CD-ROM menu, and on this Mac one of those open() calls never returns (raw device
 * access is gated behind Full Disk Access). `sample` showed the main thread parked in
 * wx_initmenu -> opendev -> __open_nocancel forever, which is why every launch sat at
 * 0% CPU with no nvr written. The machine we want to emulate has no CD-ROM at all
 * (cdrom_drive = 0), so refusing the probe costs us nothing and unblocks start-up
 * WITHOUT needing the permission granted.
 */
#include <stdio.h>

typedef struct { const void *replacement; const void *replacee; } interpose_t;

extern int opendev(char *path, int oflags, int dflags, char **realpath);

static int nodev_opendev(char *path, int oflags, int dflags, char **realpath)
{
    (void)path; (void)oflags; (void)dflags;
    if (realpath) *realpath = 0;
    return -1;                      /* "no such device", immediately */
}

__attribute__((used))
static const interpose_t interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)nodev_opendev, (const void *)opendev },
};
