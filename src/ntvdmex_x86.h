/* ntvdmex_x86.h -- the x86 architecture's fixed layout, as the host uses it (#333).
 *
 * One name per meaning, defined once (docs/STYLE.md, section 3). Included by
 * ntvdmex_types.h, so every file that has the base types has these too.
 */
#ifndef NTVDMEX_X86_H
#define NTVDMEX_X86_H

/* Real-mode addressing: linear = segment * 16 + offset. */
#define PARAGRAPH_SHIFT     4

/* Paging: a 4 KB page. */
#define PAGE_SHIFT          12

/* The real-mode interrupt vector table: at 0000:0000, one 4-byte entry per vector -- the
   handler's offset word, then its segment word. */
#define IVT_BASE_SEGMENT    0
#define IVT_ENTRY_SIZE      4
#define IVT_ENTRY_SIZE_U    4u
#define IVT_SEGMENT_OFFSET  2
/* The linear addresses of vector `vector`'s offset word and segment word. */
#define IVT_OFFSET_ADDRESS(vector)   ((vector) * IVT_ENTRY_SIZE)
#define IVT_SEGMENT_ADDRESS(vector)  ((vector) * IVT_ENTRY_SIZE + IVT_SEGMENT_OFFSET)

#endif /* NTVDMEX_X86_H */
