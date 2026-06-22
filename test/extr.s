; extr_test.s — Verify EXTR decoding works in the new decoder.
; v0 would have mis-decoded EXTR as SBFM (bit 23 was ignored).
;
; Strategy:
;   EXTR Xd, Xn, Xm, #0  →  Xd = Xm  (the low 64 bits of Xn:Xm)
;   This is the cleanest EXTR test: lsb=0 means no shift, just take Xm.
;
;   x1 = 0xDEADBEEF (Rn — irrelevant for lsb=0)
;   x0 = 0xCAFEBABE (Rm)
;   extr x2, x1, x0, #0
;     → (x1 << 64) | x0, then >> 0, then take low 64 bits
;     → x0 = 0xCAFEBABE
;   x3 = 0xCAFEBABE
;   cmp x2, x3 → equal → print "OK\n" and exit 0
;   (else -> print "NO\n" and exit 1)

_start:
    mov  x0, #0xCAFE
    movk x0, #0xBABE, lsl #16
    mov  x1, #0xDEAD
    movk x1, #0xBEEF, lsl #16
    extr x2, x1, x0, #0

    mov  x3, #0xCAFE
    movk x3, #0xBABE, lsl #16
    cmp  x2, x3
    b.ne  fail

    ; write(1, ok_msg, 3) -> "OK\n"
    mov  x0, #1
    adr  x1, ok_msg
    mov  x2, #3
    mov  x8, #64
    svc  #0

    mov  x0, #0
    mov  x8, #93
    svc  #0

fail:
    mov  x0, #1
    adr  x1, no_msg
    mov  x2, #3
    mov  x8, #64
    svc  #0
    mov  x0, #1
    mov  x8, #93
    svc  #0

ok_msg:
    .ascii "OK\n"
no_msg:
    .ascii "NO\n"
