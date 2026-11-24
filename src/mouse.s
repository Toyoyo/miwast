;
; mouse.s - the mouse, kept by the program itself.
;
; Line-A's mouse variables are kept by the VDI's handler, which GEM puts on
; the IKBD's mouse vector when it opens its workstation. Started from the
; AUTO folder, before GEM, there is none: the pointer never moved and the
; buttons were never seen. This one is put on the vector by gfx.c in either
; case, and passes every packet on to the handler it found, so under GEM
; the VDI and the AES see the mouse as before.
;
; Called by TOS's IKBD interrupt with a0 at the packet: a relative one is a
; header $F8-$FB (bit 1 the left button, bit 0 the right), then dx and dy,
; signed bytes. ms_bt is bit 0 left, bit 1 right, as Line-A's MOUSE_BT was;
; ms_x, ms_y stay on the screen, 640x400.

        XDEF    ms_handler
        XDEF    ms_x
        XDEF    ms_y
        XDEF    ms_bt
        XDEF    ms_old

        TEXT

ms_handler:
        movem.l d0-d1,-(sp)
        move.b  (a0),d0
        cmp.b   #$F8,d0
        bcs.s   .done                   ; not a relative packet
        moveq   #0,d1
        btst    #1,d0
        beq.s   .noleft
        moveq   #1,d1
.noleft:
        btst    #0,d0
        beq.s   .noright
        addq.w  #2,d1
.noright:
        move.w  d1,ms_bt

        move.b  1(a0),d0
        ext.w   d0
        add.w   ms_x,d0
        bpl.s   .xpos
        moveq   #0,d0
.xpos:
        cmp.w   #639,d0
        ble.s   .xin
        move.w  #639,d0
.xin:
        move.w  d0,ms_x

        move.b  2(a0),d0
        ext.w   d0
        add.w   ms_y,d0
        bpl.s   .ypos
        moveq   #0,d0
.ypos:
        cmp.w   #399,d0
        ble.s   .yin
        move.w  #399,d0
.yin:
        move.w  d0,ms_y
.done:
        movem.l (sp)+,d0-d1
        move.l  ms_old,-(sp)            ; on to the handler found, a0 intact
        rts

        BSS

ms_x:   ds.w    1
ms_y:   ds.w    1
ms_bt:  ds.w    1
ms_old: ds.l    1

        END
