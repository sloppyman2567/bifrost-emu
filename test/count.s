; count.s - prints numbers 1 through 5, one per line, then exits.

_start:
    mov  x19, #1                 ; counter

loop:
    ; convert counter to ASCII digit (works for 0-9)
    add  w20, w19, #48           ; '0' + counter

    ; store the digit followed by newline in buf
    adr  x21, buf
    strb w20, [x21]
    mov  w22, #10                ; '\n'
    strb w22, [x21, #1]

    ; write(1, buf, 2)
    mov  x0, #1
    mov  x1, x21
    mov  x2, #2
    mov  x8, #64
    svc  #0

    ; counter++
    add  x19, x19, #1

    ; if counter <= 5, loop
    cmp  x19, #6
    b.le loop

    ; exit(0)
    mov  x0, #0
    mov  x8, #93
    svc  #0

buf:
    .byte 0
    .byte 0
