; cat.s - reads a file path from argv[1] and prints it to stdout.
; Usage: cat.elf /etc/hostname
;
; Tests: openat, read, write, close, exit, plus a few instructions
; we haven't exercised yet (LDUR/STUR with negative offsets).

_start:
    ; argc is at [sp]. argv[0] is at [sp, #8]. argv[1] is at [sp, #16].
    ldr  x20, [sp, #16]         ; argv[1] = file path

    ; openat(AT_FDCWD, path, O_RDONLY, 0)
    mov  x0, #-100              ; AT_FDCWD
    mov  x1, x20
    mov  x2, #0                 ; O_RDONLY
    mov  x3, #0
    mov  x8, #56                ; __NR_openat
    svc  #0

    ; if fd < 0, print error and exit
    cmp  x0, #0
    b.ge open_ok

    ; print "open failed\n"
    mov  x0, #1
    adr  x1, errmsg
    mov  x2, #14
    mov  x8, #64
    svc  #0
    mov  x0, #1
    mov  x8, #93
    svc  #0

open_ok:
    mov  x20, x0                ; save fd

read_loop:
    ; read(fd, buf, 256)
    mov  x0, x20
    adr  x1, buf
    mov  x2, #256
    mov  x8, #63
    svc  #0

    cmp  x0, #0
    b.le read_done

    ; write(1, buf, n)
    mov  x2, x0
    mov  x0, #1
    adr  x1, buf
    mov  x8, #64
    svc  #0
    b    read_loop

read_done:
    ; close(fd)
    mov  x0, x20
    mov  x8, #57
    svc  #0

    ; exit(0)
    mov  x0, #0
    mov  x8, #93
    svc  #0

errmsg:
    .ascii "open failed\n\n"
    .byte 0
buf:
    .byte 0
    .byte 0
    .byte 0
    .byte 0
