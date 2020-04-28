    org 0x100
    bits 16

; program to dump the flash chip of the TS-3100 single board computer

    cli
    ; save the real mode segment registers
    push ds
    push es

    ; global descriptor table requires absolute addresses
    ; patch the gdtinfo field for the gdt address
    xor  eax, eax
    mov  ax, ds
    shl  eax, 4
    add  [gdtinfo+2], eax
    mov  eax, [gdtinfo+2]
    ; load the global descriptor table
    lgdt [gdtinfo]

    ; switch to protected mode
go_protected:
    mov  eax, cr0
    or   al, 1
    mov  cr0, eax
    mov  bx, 0x8
    mov  ds, bx
    mov  es, bx

    ; switch back to real mode
go_unreal:
    mov  eax, cr0
    and  al, 0xFE    
    mov  cr0, eax
    ; restore real mode segment registers
    ;  - note that 32 bit operations use the gdt entry but the 16 bit
    ;    operations use the real mode segment registers. this is called
    ;    unreal mode.
    pop  es           
    pop  ds
    sti               

unreal:
    ; allocate stack space for 1 KiB buffer
    push bp
    mov  bp, sp
    sub  sp, 0x400

    ; create dump file
    mov  dx, filename
    mov  cx, 0
    mov  ah, 0x3c
    int  0x21
    ; ax now contains the file handle

    ; copy 512 KiB flash image at 52 MiB mark
    cld
    mov esi, 0x3400000
next_block:
    ; setup block copy of 1 KiB (256 blocks of 4 bytes)
    mov ecx, 0x100
    movzx edi, bp
    sub edi, 0x400

    ; movsd moves ds:esi -> es:edi
    ; ds = 0 (uses unreal offsets)
    ; es = ss (stack buffer)
    push ds
    xor bx, bx
    mov ds, bx
    mov bx, ss
    mov es, bx
 
    ; copy data
    a32 rep movsd

    ; setup call to file write
    mov bx, ss
    mov ds, bx
    mov bx, ax
    mov cx, 0x400
    mov dx, bp
    sub dx, 0x400
    mov ah, 0x40
    int 0x21
    mov ax, bx
    pop ds

    cmp esi, 0x3480000
    jne next_block

    ; close file, BX must contain handle
    mov  ah, 0x3c
    int  0x21

    ; restore bp
    mov sp, bp
    pop bp

    ; exit to dos
    mov  ah, 0x4C
    int  0x21

gdtinfo:
     dw  gdt_end - gdt - 1
     dd  gdt 
gdt:
     dd 0
     dd 0
flatdesc:
     dw 0xffff
     dw 0
     db 0
     db 0x92
     db 0xcf
     db 0
gdt_end:

filename db 'C:\FLASH.BIN', 0x00 ; nul-terminated message
