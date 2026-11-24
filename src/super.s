;
; super.s - back to user mode, on the stack we are on.
;
; main.c enters supervisor mode with Super(0), which makes the current
; stack the supervisor one. Super(old_ssp) would be the way back, but it
; returns to user mode on whatever USP held when Super(0) was called --
; one call frame up from shutdown(), which is where we leave from. Its
; `rts` then popped a return address from the wrong place: two bombs on
; every exit.
;
; to_user(old_ssp) makes the current stack the user stack again, puts the
; old supervisor stack back, and drops to user mode, from any call depth.
; Called from C: void to_user(long old_ssp).

        XDEF    to_user

        TEXT

to_user:
        move.l  4(sp),d0                ; the old supervisor stack
        move.l  (sp)+,a1                ; our return address
        move.l  sp,a0
        move.l  a0,usp                  ; this stack, as the user stack
        move.w  sr,d1
        or.w    #$0700,sr               ; no interrupts while the stacks change
        move.l  d0,sp                   ; the supervisor stack, as it was
        and.w   #$dfff,d1               ; user mode, interrupt mask as it was
        move.w  d1,sr                   ; a7 is the user stack from here
        jmp     (a1)

        END
