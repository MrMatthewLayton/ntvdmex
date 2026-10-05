#ifndef NTVDMEX_VDD_NET_H
#define NTVDMEX_VDD_NET_H
/*
 * vdd_net.h -- NetBIOS through INT 5Ch: the network interface a DOS program under NT
 * has.  GH #8, s91.
 *
 * WHY NetBIOS AND NOT A PACKET DRIVER. #8 named "packet driver / NDIS-era interface
 * as appropriate". The reference decides what is appropriate: stock XP NTVDM answers
 * INT 5Ch by handing the program's NCB to the NT NetBIOS driver (tests/probes/dos/p_netb
 * measured it), and it offers no packet driver -- that needs raw Ethernet frames, which
 * a user-mode process on XP cannot send without a capture driver. NetBIOS is what DOS
 * network programs of the era (and LAN Manager / Novell NetBIOS clients) speak.
 *
 * SHAPE. The device owns the DOS side: it reads the 64-byte NCB at ES:BX, resolves its
 * buffer, hands a host-neutral netb_ncb to a BACKEND, and writes the answer back where
 * DOS keeps it (retcode, lsn, num, length, callname, cmd_cplt). The backend is the
 * host's (main.c: Win32 Netbios() in netapi32.dll); the off-VM test gives it a fake.
 *
 * The DOS NCB (IBM NetBIOS Technical Reference; identical in every DOS NetBIOS):
 *   +00 command   +01 retcode  +02 lsn  +03 num  +04 buffer (far)  +08 length
 *   +0A callname[16]  +1A name[16]  +2A rto  +2B sto  +2C post (far)  +30 lana
 *   +31 cmd_cplt  +32 reserved[14]
 * A command with bit 7 set is NO-WAIT: INT 5Ch returns at once with AL = the
 * IMMEDIATE code (0 = accepted), and the final answer arrives in retcode/cmd_cplt
 * (FFh while pending), with the POST routine called if one is given.
 * ⚠ THIS DEVICE COMPLETES A NO-WAIT COMMAND BEFORE RETURNING. The final answer is in
 *   the NCB by the time the program looks, which every polling program accepts. A POST
 *   routine is recorded (post_pending) and the HOST runs it as INT 5Ch returns: the
 *   stub's IRET goes to the POST routine with ES:BX = the NCB, and its IRET to the
 *   caller -- as if the command had completed the instant it was issued.
 */
#include "vdd_bus.h"

#define NETB_NCB_SIZE   64
#define NETB_ILLCMD     0x03      /* retcode: invalid command                    */
#define NETB_ILLLANA    0x23      /* retcode: invalid adapter (lana) number      */
#define NETB_BADBUF     0x01      /* retcode: illegal buffer length / address    */
#define NETB_PENDING    0xFF

typedef struct {
    uint8_t  command;              /* wait form: bit 7 stripped                   */
    uint8_t  retcode, lsn, num;
    uint8_t *buffer;               /* host address of the guest buffer, or NULL   */
    uint16_t length;
    uint8_t  callname[16], name[16];
    uint8_t  rto, sto, lana;
} netb_ncb;

/* The host's NetBIOS. Synchronous; returns the final retcode (also in n->retcode). */
typedef uint8_t (*netb_submit_fn)(void *ctx, netb_ncb *n);

typedef struct net_state {
    vdd_bus       *bus;
    netb_submit_fn submit;   void *ctx;
    uint32_t calls, nowait, posts_owed, no_backend;
    /* s91: a no-wait command's POST routine, owed to the guest as soon as INT 5Ch
       returns. The HOST delivers it (it alone can edit the guest's return frame --
       see v86_bios_bop) and clears post_pending; posts_owed counts the ones that
       could not be delivered. */
    int      post_pending;
    uint16_t post_seg, post_off;
    uint8_t  last_cmd, last_ret;
} net_state;

int  vdd_net_init(vdd_bus *b, void *self);
void vdd_net_reset(void *self);
void vdd_net_set_backend(net_state *st, netb_submit_fn fn, void *ctx);

/* The whole INT 5Ch service on an NCB already mapped to host memory: exposed for the
   off-VM test. `buf` = the NCB's buffer resolved by the caller (NULL if none).
   Returns AL. */
uint8_t vdd_net_service(net_state *st, uint8_t *ncb, uint8_t *buf);

static inline ntvdd vdd_net_device(net_state *st)
{ ntvdd d; d.name = "netbios"; d.init = vdd_net_init; d.reset = vdd_net_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_NET_H */
