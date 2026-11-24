; Streaming STE DMA replay: the DMA loops over a ring buffer; every 50 Hz
; tick the play routine reads the DMA address counter and decodes just enough
; samples to keep the ring full. Assemble with -DCODEC=1 (Rice, lossless) or
; -DCODEC=2 (4-bit ADPCM); data comes from convert_codec.py. YM stays silent.
; All code is PC-relative.

	include	"codec.inc"

RBSZ	equ	4096			; ring buffer, bytes (~0.33 s)
GUARD	equ	64			; never write this close behind the DMA

	bra.w	init
	bra.w	exit
	bra.w	play

	incbin	"header.bin"		; SNDH tags, written by convert_codec.py

YM	equ	$ffff8800
DMA	equ	$ffff8900

init:	movem.l	d0-a6,-(sp)
	lea	vars(pc),a6
	bsr	silence
	lea	DMA.w,a1
	clr.b	1(a1)			; DMA stop
	move.b	#$81,$21(a1)		; mono, 12517 Hz
	bsr	restart
	lea	ring(pc),a0
	move.l	a0,wptr-vars(a6)
	move.w	#RBSZ-GUARD,d7		; prefill
	bsr	decode
	lea	ring(pc),a0		; frame = whole ring, looped
	move.l	a0,d3
	lea	RBSZ(a0),a0
	move.l	a0,d4
	lea	DMA.w,a1
	move.l	d3,d5
	swap	d5
	move.b	d5,3(a1)
	rol.l	#8,d5
	move.b	d5,5(a1)
	rol.l	#8,d5
	move.b	d5,7(a1)
	move.l	d4,d5
	swap	d5
	move.b	d5,$f(a1)
	rol.l	#8,d5
	move.b	d5,$11(a1)
	rol.l	#8,d5
	move.b	d5,$13(a1)
	move.b	#3,1(a1)		; play + loop
	movem.l	(sp)+,d0-a6
	rts

exit:	movem.l	d0-a6,-(sp)
	clr.b	DMA+1.w
	bsr	silence
	movem.l	(sp)+,d0-a6
	rts

silence:
	lea	YM.w,a1
	move.w	sr,-(sp)
	or.w	#$0700,sr
	move.b	#7,(a1)
	move.b	#$ff,2(a1)		; all off, ports A/B stay outputs
	moveq	#8,d0
.v:	move.b	d0,(a1)
	clr.b	2(a1)
	addq.b	#1,d0
	cmp.b	#11,d0
	bne.s	.v
	move.w	(sp)+,sr
	rts

play:	movem.l	d0-a6,-(sp)
	lea	vars(pc),a6
.r:	moveq	#0,d0			; DMA counter, read until stable
	move.b	DMA+9.w,d0
	lsl.l	#8,d0
	move.b	DMA+$b.w,d0
	lsl.l	#8,d0
	move.b	DMA+$d.w,d0
	moveq	#0,d1
	move.b	DMA+9.w,d1
	lsl.l	#8,d1
	move.b	DMA+$b.w,d1
	lsl.l	#8,d1
	move.b	DMA+$d.w,d1
	cmp.l	d0,d1
	bne.s	.r
	; free = RBSZ - GUARD - ((wptr - counter) mod RBSZ)
	move.l	wptr-vars(a6),d7
	sub.l	d0,d7
	and.w	#RBSZ-1,d7
	neg.w	d7
	add.w	#RBSZ-GUARD,d7
	ble.s	.done
	bsr	decode
.done:	movem.l	(sp)+,d0-a6
	rts

; ------------------------------------------------------------------ Rice
	if	CODEC=1

; restart the bitstream (song start / loop)
restart:	lea	packed(pc),a0
	move.l	a0,bitptr-vars(a6)
	move.l	#$80000000,bitbuf-vars(a6)	; empty: only the sentinel bit
	clr.l	x1-vars(a6)		; x1.w, x2.w
	clr.w	blkleft-vars(a6)
	move.w	#NBLOCKS,blocks-vars(a6)
	rts

; get one bit into C/X (d6 = bit buffer with sentinel, a0 = data)
GETBIT	macro
	add.l	d6,d6
	bne.s	.\@
	move.l	(a0)+,d6
	addx.l	d6,d6
.\@:
	endm

; decode d7.w samples into the ring at wptr
decode:	move.l	wptr-vars(a6),a1
	lea	ring+RBSZ(pc),a2
	move.l	bitptr-vars(a6),a0
	move.l	bitbuf-vars(a6),d6
	movem.w	x1-vars(a6),d2-d5	; d2 = x1, d3 = x2, d4 = k, d5 = left
	subq.w	#1,d7
.samp:	tst.w	d5
	bne.s	.go
	subq.w	#1,blocks-vars(a6)	; new block: end of song?
	bpl.s	.hdr
	move.l	a1,-(sp)
	move.w	d7,-(sp)
	bsr	restart
	move.w	(sp)+,d7
	move.l	(sp)+,a1
	move.l	bitptr-vars(a6),a0
	move.l	bitbuf-vars(a6),d6
	moveq	#0,d2
	moveq	#0,d3
	subq.w	#1,blocks-vars(a6)
.hdr:	moveq	#0,d4			; k = 3 bits
	GETBIT
	addx.w	d4,d4
	GETBIT
	addx.w	d4,d4
	GETBIT
	addx.w	d4,d4
	move.w	#256,d5
.go:	moveq	#-1,d0			; unary: count zeros up to the one
.un:	addq.w	#1,d0
	GETBIT
	bcc.s	.un
	move.w	d4,d1			; then k bits
	bra.s	.kt
.kl:	GETBIT
	addx.w	d0,d0
.kt:	dbra	d1,.kl
	lsr.w	#1,d0			; unzigzag
	bcc.s	.pos
	not.w	d0
.pos:	move.w	d2,d1			; x = 2*x1 - x2 + r
	add.w	d1,d1
	sub.w	d3,d1
	add.w	d0,d1
	move.w	d2,d3
	move.w	d1,d2
	move.b	d1,(a1)+
	cmp.l	a2,a1
	bne.s	.nw
	lea	ring(pc),a1
.nw:	subq.w	#1,d5
	dbra	d7,.samp
	move.l	a1,wptr-vars(a6)
	move.l	a0,bitptr-vars(a6)
	move.l	d6,bitbuf-vars(a6)
	movem.w	d2-d5,x1-vars(a6)
	rts

	else
; ------------------------------------------------------------------ ADPCM

restart:	lea	packed(pc),a0
	move.l	a0,bitptr-vars(a6)
	clr.w	pred-vars(a6)
	clr.w	row-vars(a6)		; index 0
	move.l	#NBYTES,bytesleft-vars(a6)
	rts

; decode nibble d2 (0..15): d4 = predictor, a4 = row of the step table
NIBBLE	macro
	move.w	d2,d1
	and.w	#7,d1
	lsl.w	#2,d1
	move.w	0(a4,d1.w),d3		; diff
	btst	#3,d2
	beq.s	.p\@
	neg.w	d3
.p\@:	add.w	d3,d4
	bvc.s	.o\@
	move.w	#$7fff,d4		; saturate
	tst.w	d3
	bpl.s	.o\@
	move.w	#$8000,d4
.o\@:	move.w	2(a4,d1.w),d1		; next index row
	lea	0(a3,d1.w),a4
	move.w	d4,-(sp)		; high byte = 8-bit sample (byte pops
	move.b	(sp)+,(a1)+		; move SP by 2)
	cmp.l	a2,a1
	bne.s	.w\@
	lea	ring(pc),a1
.w\@:
	endm

; decode d7.w samples (rounded down to pairs) into the ring at wptr
decode:	move.l	wptr-vars(a6),a1
	lea	ring+RBSZ(pc),a2
	move.l	bitptr-vars(a6),a0
	lea	adpcmtab(pc),a3
	move.w	row-vars(a6),d0
	lea	0(a3,d0.w),a4
	move.w	pred-vars(a6),d4
	move.l	bytesleft-vars(a6),d5
	lsr.w	#1,d7
	subq.w	#1,d7
	bmi	.end
.pair:	subq.l	#1,d5
	bpl.s	.go
	move.l	a1,-(sp)
	bsr	restart
	move.l	(sp)+,a1
	move.l	bitptr-vars(a6),a0
	move.l	a3,a4
	moveq	#0,d4
	move.l	bytesleft-vars(a6),d5
	subq.l	#1,d5
.go:	moveq	#0,d0
	move.b	(a0)+,d0
	move.w	d0,d2
	lsr.w	#4,d2
	NIBBLE
	move.w	d0,d2
	and.w	#15,d2
	NIBBLE
	dbra	d7,.pair
.end:	move.l	a1,wptr-vars(a6)
	move.l	a0,bitptr-vars(a6)
	sub.l	a3,a4
	move.w	a4,row-vars(a6)
	move.w	d4,pred-vars(a6)
	move.l	d5,bytesleft-vars(a6)
	rts
	endif

	even
vars:
wptr:		dc.l	0
bitptr:		dc.l	0
	if	CODEC=1
bitbuf:		dc.l	0
x1:		dc.w	0		; x1, x2, k, blkleft: consecutive (movem)
x2:		dc.w	0
kbits:		dc.w	0
blkleft:	dc.w	0
blocks:		dc.w	0
	else
bytesleft:	dc.l	0
pred:		dc.w	0
row:		dc.w	0
adpcmtab:	incbin	"adpcmtab.bin"
	endif
	even
ring:		ds.b	RBSZ
	even
packed:		incbin	"packed.bin"
	even
