; echo.s - reads from stdin and echoes to stdout, char by char.
; Exits on EOF (Ctrl-D) or a single 'q' character.

_start:
    ; print a prompt
    mov  x0, #1                  ; stdout
    adr  x1, prompt
    mov  x2, #7
    mov  x8, #64                 ; write
    svc  #0

loop:
    ; read(0, buf, 1)
    mov  x0, #0                  ; stdin
    adr  x1, buf
    mov  x2, #1
    mov  x8, #63                 ; read
    svc  #0

    ; if read returned 0, exit
    cmp  x0, #0
    b.eq done

    ; check for 'q'
    adr  x1, buf
    ldrb w2, [x1]
    cmp  w2, #113                ; 'q'
    b.eq done

    ; echo the byte back: write(1, buf, 1)
    mov  x0, #1
    adr  x1, buf
    mov  x2, #1
    mov  x8, #64
    svc  #0

    b loop

done:
    ; print newline and exit
    mov  x0, #1
    adr  x1, newline
    mov  x2, #1
    mov  x8, #64
    svc  #0

    mov  x0, #0
    mov  x8, #93
    svc  #0

prompt:
    .ascii "echo> \n"
buf:
    .byte 0
newline:
    .byte 10
