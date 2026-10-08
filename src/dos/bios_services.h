/* bios_services.h -- the BIOS and multiplex services the host answers: function numbers,
 * status codes and register formats, as the dispatchers' comments name them (#333).
 * Defines only.
 */
#ifndef NTVDMEX_DOS_BIOS_SERVICES_H
#define NTVDMEX_DOS_BIOS_SERVICES_H

/* INT 13h, disk (AH). */
#define BIOS_DISK_RESET                 0x00
#define BIOS_DISK_GET_STATUS            0x01
#define BIOS_DISK_READ                  0x02
#define BIOS_DISK_WRITE                 0x03
#define BIOS_DISK_VERIFY                0x04
#define BIOS_DISK_GET_PARAMETERS        0x08
#define BIOS_DISK_GET_TYPE              0x15
#define BIOS_DISK_STATUS_BAD_COMMAND    0x01
#define BIOS_DISK_STATUS_SECTOR_NOT_FOUND 0x04
#define BIOS_DISK_STATUS_NOT_READY      0x80
#define BIOS_DISK_TYPE_FLOPPY_NO_CHANGE_LINE 0x01
/* CX's CHS packing: sector in bits 0-5, cylinder bits 8-9 in bits 6-7. */
#define BIOS_CHS_SECTOR_MASK            0x3F
#define BIOS_CHS_CYLINDER_HIGH_MASK     0xC0
#define BIOS_CHS_CYLINDER_HIGH_SHIFT    2

/* INT 15h, system services (AH), and the AH it answers with. */
#define BIOS_SYSTEM_KEYBOARD_INTERCEPT  0x4F
#define BIOS_SYSTEM_EVENT_WAIT          0x83
#define BIOS_SYSTEM_JOYSTICK            0x84
#define BIOS_SYSTEM_SYSREQ              0x85
#define BIOS_SYSTEM_WAIT                0x86
#define BIOS_SYSTEM_MOVE_BLOCK          0x87
#define BIOS_SYSTEM_EXTENDED_MEMORY     0x88
#define BIOS_SYSTEM_GET_CONFIGURATION   0xC0
#define BIOS_SYSTEM_GET_EBDA            0xC1
#define BIOS_SYSTEM_STATUS_BUSY         0x83    /* a wait is already counting      */
#define BIOS_SYSTEM_STATUS_UNSUPPORTED  0x86
#define BIOS_EVENT_WAIT_CANCEL          0x01    /* AH=83h AL                       */
#define BIOS_JOYSTICK_READ_BUTTONS      0x0000  /* AH=84h DX                       */
#define BIOS_JOYSTICK_READ_AXES         0x0001

/* INT 16h, keyboard (AH). */
#define BIOS_KEYBOARD_READ              0x00
#define BIOS_KEYBOARD_READ_EXTENDED     0x10

/* INT 17h, printer (AH), and its status byte: bit 7 not busy, 6 acknowledge, 5 out of
   paper, 4 selected, 3 I/O error, 0 timeout. */
#define BIOS_PRINTER_PRINT              0x00
#define BIOS_PRINTER_INITIALIZE         0x01
#define BIOS_PRINTER_GET_STATUS         0x02
#define BIOS_PRINTER_READY              0x90    /* not busy + selected             */
#define BIOS_PRINTER_FAILED             0x28    /* out of paper + I/O error        */

/* INT 2Fh, the multiplex (AX). */
#define MULTIPLEX_XMS_INSTALLATION_CHECK 0x4300
#define MULTIPLEX_XMS_GET_ENTRY          0x4310
#define MULTIPLEX_XMS_INSTALLED          0x80    /* AL                              */
#define MULTIPLEX_DOS_TABLES             0x122E  /* DL selects: see DOS_INT2F_TBL_* */
#define MULTIPLEX_DEVICE_API_ENTRY       0x1684
#define MULTIPLEX_DPMI_INSTALLATION_CHECK 0x1687
#define MULTIPLEX_DPMI_32BIT_SUPPORTED   1       /* BX bit 0                        */

#endif /* NTVDMEX_DOS_BIOS_SERVICES_H */
