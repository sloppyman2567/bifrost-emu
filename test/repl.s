; repl.s - tiny REPL: reads a line, prints "got: <line>", repeats.
; Exits on 'q' alone or EOF.
;
; Tests character-at-a-time input with line buffering.

_start:
    ; print prompt
    mov  x0, #1
    adr  x1, prompt
    mov  x2, #2
    mov  x8, #64
    svc  #0

    ; clear line index
    mov  x19, #0                ; x19 = line length

read_char:
    ; read(0, charbuf, 1)
    mov  x0, #0
    adr  x1, charbuf
    mov  x2, #1
    mov  x8, #63
    svc  #0

    ; if EOF, exit
    cmp  x0, #0
    b.eq exit_repl

    ; load the char
    adr  x20, charbuf
    ldrb w21, [x20]

    ; if 'q' with empty line, exit
    cmp  w21, #113              ; 'q'
    b.ne not_q
    cmp  x19, #0
    b.eq exit_repl
not_q:

    ; if newline, flush line
    cmp  w21, #10               ; '\n'
    b.eq flush_line

    ; otherwise, append to linebuf (max 63 chars)
    cmp  x19, #63
    b.ge read_char              ; line full, discard
    adr  x22, linebuf
    add  x22, x22, x19
    strb w21, [x22]
    add  x19, x19, #1
    b read_char

flush_line:
    ; print "got: "
    mov  x0, #1
    adr  x1, gotstr
    mov  x2, #5
    mov  x8, #64
    svc  #0

    ; print the line (length x19)
    mov  x0, #1
    adr  x1, linebuf
    mov  x2, x19
    mov  x8, #64
    svc  #0

    ; print newline
    mov  x0, #1
    adr  x1, newline
    mov  x2, #1
    mov  x8, #64
    svc  #0

    ; reset line length and print prompt
    mov  x19, #0
    mov  x0, #1
    adr  x1, prompt
    mov  x2, #2
    mov  x8, #64
    svc  #0
    b read_char

exit_repl:
    mov  x0, #0
    mov  x8, #93
    svc  #0

prompt:
    .ascii "> "
charbuf:
    .byte 0
gotstr:
    .ascii "got: "
newline:
    .byte 10
linebuf:
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
