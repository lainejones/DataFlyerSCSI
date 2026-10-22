| DataFlyerSCSI - ROMTag and strings for dataflyer.device.
| GPL-3.0-only, see LICENSE.
|
| No RTF_AUTOINIT: rt_Init points at Init, which calls the C initialiser
| (df_init: MakeLibrary + AddDevice). Exec passes the segment list in a0 for a
| non-autoinit resident (some loaders also in d0).

    .text

| Executing the device as a program just returns -1.
Start:
    moveq   #-1,d0
    rts
    .even

RomTag:
    .word   0x4AFC              | RTC_MATCHWORD
    .long   RomTag              | rt_MatchTag
    .long   EndSkip             | rt_EndSkip
    .byte   0                   | rt_Flags (no RTF_AUTOINIT)
    .byte   1                   | rt_Version
    .byte   3                   | rt_Type = NT_DEVICE
    .byte   0                   | rt_Pri
    .long   _df_devname         | rt_Name
    .long   _df_idstring        | rt_IdString
    .long   Init                | rt_Init
EndSkip:
    .even

    .globl  _df_devname
_df_devname:
    .ascii  "dataflyer.device"
    .byte   0
    .even

    .globl  _df_idstring
_df_idstring:
    .ascii  "dataflyer.device 1.0.3 (22.10.2026)"
    .byte   13,10,0
    .ascii  "$VER: dataflyer.device 1.0.3 (22.10.2026) DataFlyer 1200 SCSI+ driver"
    .byte   0
    .even

| d0/a0 = segment list, a6 = ExecBase
Init:
    tst.l   d0
    bne.s   1f
    move.l  a0,d0
1:
    jmp     _df_init

| ---- pseudo-DMA bursts, as tight as ExpXDS's own loop ------------------
| Per byte: test DMA REQUEST in bus-and-status ($DA0016 bit 6), move one
| byte through the card's port ($DA1002). They stop at the first byte the
| chip is not ready for and return how many moved; the C side then waits
| (timed) or sees the phase change, and calls again.
|
| Reads go 256 bytes per DMA REQUEST test, as ExpXDS does: once the chip
| has a byte the card holds each further port read until the next one has
| come in, so only the first byte of a run needs the test. The rest (under
| 256) goes a byte at a time.
|
| ULONG df_dmain(UBYTE *p, ULONG n)
    .globl  _df_dmain
_df_dmain:
    move.l  4(sp),a0
    move.l  8(sp),d0
    move.l  d0,d1
    beq.w   2f
    movem.l d2/a2,-(sp)
    lea     0xDA0016,a1
    lea     0xDA1002,a2
3:  cmp.l   #256,d1
    bcs.w   1f
    move.b  (a1),d2
    and.b   #0x48,d2
    cmp.b   #0x48,d2
    bne.w   4f
    .rept   256
    move.b  (a2),(a0)+
    .endr
    sub.l   #256,d1
    bne.w   3b
    bra.s   4f
1:  move.b  (a1),d2
    and.b   #0x48,d2
    cmp.b   #0x48,d2
    bne.s   4f
    move.b  (a2),(a0)+
    subq.l  #1,d1
    bne.s   1b
4:  movem.l (sp)+,d2/a2
2:  sub.l   d1,d0
    rts

| A byte at a time, DMA REQUEST tested before each: for everything that is
| not whole blocks.
|
| ULONG df_dmain_byte(UBYTE *p, ULONG n)
    .globl  _df_dmain_byte
_df_dmain_byte:
    move.l  4(sp),a0
    move.l  8(sp),d0
    move.l  d0,d1
    beq.s   2f
    movem.l d2/a2,-(sp)
    lea     0xDA0016,a1
    lea     0xDA1002,a2
1:  move.b  (a1),d2
    and.b   #0x48,d2
    cmp.b   #0x48,d2
    bne.s   3f
    move.b  (a2),(a0)+
    subq.l  #1,d1
    bne.s   1b
3:  movem.l (sp)+,d2/a2
2:  sub.l   d1,d0
    rts

| ULONG df_dmaout(const UBYTE *p, ULONG n)
    .globl  _df_dmaout
_df_dmaout:
    move.l  4(sp),a0
    move.l  8(sp),d0
    move.l  d0,d1
    beq.s   2f
    movem.l d2/a2,-(sp)
    lea     0xDA0016,a1
    lea     0xDA1002,a2
1:  move.b  (a1),d2
    and.b   #0x48,d2
    cmp.b   #0x48,d2
    bne.s   3f
    move.b  (a0)+,(a2)
    subq.l  #1,d1
    bne.s   1b
3:  movem.l (sp)+,d2/a2
2:  sub.l   d1,d0
    rts

| The same as reads: one DMA REQUEST test per 256 bytes (an experiment -
| ExpXDS tests every byte it writes; DF_OPTS bit 0 switches it on).
|
| ULONG df_dmaout_run(const UBYTE *p, ULONG n)
    .globl  _df_dmaout_run
_df_dmaout_run:
    move.l  4(sp),a0
    move.l  8(sp),d0
    move.l  d0,d1
    beq.w   2f
    movem.l d2/a2,-(sp)
    lea     0xDA0016,a1
    lea     0xDA1002,a2
3:  cmp.l   #256,d1
    bcs.w   1f
    move.b  (a1),d2
    and.b   #0x48,d2
    cmp.b   #0x48,d2
    bne.w   4f
    .rept   256
    move.b  (a0)+,(a2)
    .endr
    sub.l   #256,d1
    bne.w   3b
    bra.s   4f
1:  move.b  (a1),d2
    and.b   #0x48,d2
    cmp.b   #0x48,d2
    bne.s   4f
    move.b  (a0)+,(a2)
    subq.l  #1,d1
    bne.s   1b
4:  movem.l (sp)+,d2/a2
2:  sub.l   d1,d0
    rts

| ---- the built-in IDE port ----------------------------------------------
| One 512-byte sector through the data port, a long at a time: the Gayle
| splits a long access to the 16-bit port into two word cycles, both on the
| data register.
|
| void df_idein(APTR p)
    .globl  _df_idein
_df_idein:
    move.l  4(sp),a0
    lea     0xDA2000,a1
    .rept   128
    move.l  (a1),(a0)+
    .endr
    rts

| Writes go a word at a time, as scsi.device does: a long write to the
| port froze the real A1200 (reads by the long are fine).
|
| void df_ideout(const void *p)
    .globl  _df_ideout
_df_ideout:
    move.l  4(sp),a0
    lea     0xDA2000,a1
    .rept   256
    move.w  (a0)+,(a1)
    .endr
    rts

| ---- calling a device vector (scsi.device's old ones) --------------------
| ULONG df_callvec(APTR fn, APTR a1, APTR a6, ULONG d0, ULONG d1)
    .globl  _df_callvec
_df_callvec:
    movem.l d2-d7/a2-a6,-(sp)
    move.l  48(sp),a0
    move.l  52(sp),a1
    move.l  56(sp),a6
    move.l  60(sp),d0
    move.l  64(sp),d1
    jsr     (a0)
    movem.l (sp)+,d2-d7/a2-a6
    rts

| ---- interrupt probe (DF_IRQPROBE) ----------------------------------------
| Servers on INT2 (PORTS) and INT6 (EXTER): while _irq_on, look at the
| 53C80's bus-and-status; its interrupt bit (4) up = the card's interrupt.
| Count it, keep the status and Gayle's interrupt register, clear it by
| reading reset-parity/interrupt. Always "not ours" (Z set) so the chain
| goes on to the real owners of the line.
    .globl  _df_int2srv, _df_int6srv, _irq_on, _irq_cnt, _irq_bsr, _irq_gayle, _irq_other
_df_int2srv:
    lea     _irq_cnt,a0
    bra.s   1f
_df_int6srv:
    lea     _irq_cnt+4,a0
1:  tst.b   _irq_on
    beq.s   2f
    move.b  0xDA0016,d0
    move.b  d0,_irq_bsr
    move.b  0xDA9000,_irq_gayle
    btst    #4,d0
    beq.s   3f
    addq.l  #1,(a0)
    move.b  0xDA001E,d0
    bra.s   2f
3:  addq.l  #1,_irq_other
2:  moveq   #0,d0
    rts

    .data
_irq_on:    .byte 0
    .even
_irq_cnt:   .long 0,0
_irq_bsr:   .byte 0
_irq_gayle: .byte 0
    .even
_irq_other: .long 0
    .text
