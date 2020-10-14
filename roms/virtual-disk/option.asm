; virtual disk option rom
    bits 16
    org 0x0000
    section .text

    ; header
    db 0x55
    db 0xaa
    db 16
    jmp rom_init

%include "stringformat.asm"

rom_init:
    ; backup registers we clobber
    push ax
    push bx
    push cx
    push dx
    push si
    push bp
    push ds
    push es

    ; set DS to local data (we cheat and use our internal memory because we are a vm)
    mov ax, cs
    mov ds, ax

    ; backup existing int 13h and int 19h vectors
    xor ax, ax
    mov es, ax
    mov ax, WORD [es:13h*4]
    mov WORD [ds:bios_int13h_offset], ax
    mov ax, WORD [es:13h*4+2]
    mov WORD [ds:bios_int13h_segment], ax
    mov ax, WORD [es:19h*4]
    mov WORD [ds:bios_int19h_offset], ax
    mov ax, WORD [es:19h*4+2]
    mov WORD [ds:bios_int19h_segment], ax

    ; overwrite int 19h with our vector
    mov WORD [es:13h*4], int13h_handler
    mov WORD [es:13h*4+2], cs
    mov WORD [es:19h*4], int19h_handler
    mov WORD [es:19h*4+2], cs

    ; up the fixed disk count, get our drive number
    mov ax, 0x0040
    mov es, ax
    mov al, BYTE [es:0x75]
    mov bl, al
    add bl, 0x80
    mov BYTE [ds:vdisk_drivenum], bl
    inc al
    mov BYTE [es:0x75], al

    ; select the zero lba
    mov dx, 0xD000
    mov ax, 0x0000
    out dx, ax
    add dx, 2
    out dx, ax
    add dx, 2
    out dx, ax

    ; get the current video mode
    mov ah, 0x0F
    int 10h
    ; print messages
    mov ah, 0x0E
    mov si, rom_message
    lodsb
.rom_init_print_message:
    int 10h
    lodsb
    test al, al
    jnz .rom_init_print_message

    ; restore registers
    pop es
    pop ds
    pop bp
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    retf

    ; if this chains to another handler, we need to *replace* ourself with the other call
int19h_handler:
    sti

    ; set a stack frame
    push bp
    mov bp, sp
    sub sp, 8

%if 1
    ; replace call with old bios call, todo: overwrite our stack frame
    mov WORD [ss:bp-2], ax
    mov WORD [ss:bp-4], bx
    mov WORD [ss:bp-6], ds
    mov WORD [ss:bp-8], es

    xor ax, ax
    mov es, ax
    mov ax, cs
    mov ds, ax

    mov ax, WORD [es:13h*4]
    mov bx, WORD [es:13h*4+2]

    push WORD [ds:bios_int19h_segment]
    push WORD [ds:bios_int19h_offset]
    mov ax, WORD [ss:bp-2]
    mov bx, WORD [ss:bp-4]
    mov ds, WORD [ss:bp-6]
    mov es, WORD [ss:bp-8]
    retf
    ; this call SHOULD NOT return
%endif

%if 0
    ; map LBA zero into the virtual disk
    mov dx, 0xd000
    mov ax, 2048
    out dx, ax
    add dx, 2
    xor ax, ax
    out dx, ax
    add dx, 2
    out dx, ax

    ; set the data segment to the code segment, extra segment to 0
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax

    ; copy the IPL and jump to it
    mov si, 0x1000
    mov di, 0x7c00
    mov cx, 0x100
    rep movsw
    jmp WORD 0x0000:0x7c00
%endif

int13h_handler:
    sti
    ; set a stack frame
    push bp
    mov bp, sp
    sub sp, 12

    ; save some registers, setup vars pointer
    mov WORD [ss:bp-2], ax
    mov WORD [ss:bp-4], ds
    mov WORD [ss:bp-6], cx
    mov WORD [ss:bp-8], di
    mov WORD [ss:bp-10], dx
    mov WORD [ss:bp-12], si
    mov ax, cs
    mov ds, ax
    mov ax, WORD [ss:bp-2]

    ; skip disk reset for now
    cmp ah, 0x00
    je .BIOSInt13h
    ; skip if not our disk
    cmp dl, BYTE [ds:vdisk_drivenum]
    jne .BIOSInt13h
    ; we only support disk parameter check at the moment
    cmp ah, 0x08
    je .HandleAH08
    cmp ah, 0x02
    je .HandleAH02
    cmp ah, 0x03
    je .HandleAH03
    ;int 3
    jmp .BIOSInt13h

.HandleAH08:
    ; use 255x63 virtual mappings
    ; bits [15:8] = head count = 1 (255 heads here)
    ; bits [7:0] = drive count (1 drive here)
    mov dx, 0xfe01
    ; bits [15:6] = cylinder count - 1 (8 cylinders here)
    ; bits [5:0] = sectors/track count (63 sectors/track)
    mov cx, 0x073f

    ; no error (clear CF, zero AH)
    xor ah, ah
    clc
    mov WORD [ss:bp-2], ax
    mov WORD [ss:bp-6], cx
    mov WORD [ss:bp-10], dx
    jmp .FinishInt13hHandler

.HandleAH02:
    ;int 3
    cld
    ; put BX in DI (the destination address)
    mov di, bx

    ; convert chs to lba (dx:ax = 512 bytes/sector lba)
    mov al, 255
    mul ch
    mov dl, dh
    xor dh, dh
    add ax, dx
    mov dx, 63
    mul dx
    xor ch, ch
    sub cl, 1
    add ax, cx
    adc dx, 0

    ; check the current lba
    push ax
    mov bx, ax
    mov cx, dx
    and bx, 0xFFF8
    mov dx, 0xD000
    in ax, dx
    cmp ax, bx
    jne .HandleAH02_OutputLBALowWord
    add dx, 2
    in ax, dx
    cmp ax, cx
    jne .HandleAH02_OutputLBAHighWord
    jmp .HandleAH02_SkipLBA

    ; write out the lba (and select it)
.HandleAH02_OutputLBALowWord:
    mov ax, bx
    out dx, ax
    add dx, 2
.HandleAH02_OutputLBAHighWord:
    mov ax, cx
    out dx, ax
    add dx, 2
    out dx, ax
.HandleAH02_SkipLBA:
    pop ax

    ; figure out the offset
    mov bx, [ss:bp-2] ; get old ax value (bl contains sector count to move)
    and ax, 0x0007
    je .HandleAH02_SkipAlignment

    ; get blocks to move
    xor cl, cl
    mov ch, 8  ; "effective" multiply by 256 (512 bytes, cx is in words)
    sub ch, al
    cmp ch, bl ; do we terminate early?
    jng .HandleAH02_StartCopy
    ; so we aren't moving a whole block
    mov ch, bl

.HandleAH02_StartCopy:
    sub bl, ch
    ; compute offset into memory
    mov ah, al ; "effective" multiply by 512
    shl ah, 1
    xor al, al
    mov si, 0x1000
    add si, ax
    rep movsw
    test bl, bl
    jz .HandleAH02_FinishCopy
    ; increment the LBA before continuing to main loop
    call IncrementLBA

.HandleAH02_SkipAlignment:
    test bl, 0xF8
    jz .HandleAH02_LastCopy
    mov cx, 2048
    mov si, 0x1000
    rep movsw
    sub bl, 8
    jz .HandleAH02_FinishCopy
    ; increment the LBA before the next block
    ; can the following be replaced by (push SkipAlignment, jmp IncrementLBA ?)
    call IncrementLBA
    jmp .HandleAH02_SkipAlignment

.HandleAH02_LastCopy:
    xor cl, cl
    mov ch, bl
    mov si, 0x1000
    rep movsw

.HandleAH02_FinishCopy:
    ; setup results
    mov ax, WORD [ss:bp-2]
    xor ah, ah
    clc
    mov WORD [ss:bp-2], ax
    jmp .FinishInt13hHandler

.HandleAH03:
    ;int 3
    cld
    ; put BX in SI (the source address)
    mov si, bx

    ; convert chs to lba (dx:ax = 512 bytes/sector lba)
    mov al, 255
    mul ch
    mov dl, dh
    xor dh, dh
    add ax, dx
    mov dx, 63
    mul dx
    xor ch, ch
    sub cl, 1
    add ax, cx
    adc dx, 0

    ; check the current lba
    push ax
    mov bx, ax
    mov cx, dx
    and bx, 0xFFF8
    mov dx, 0xD000
    in ax, dx
    cmp ax, bx
    jne .HandleAH03_OutputLBALowWord
    add dx, 2
    in ax, dx
    cmp ax, cx
    jne .HandleAH03_OutputLBAHighWord
    jmp .HandleAH03_SkipLBA

    ; write out the lba (and select it)
.HandleAH03_OutputLBALowWord:
    mov ax, bx
    out dx, ax
    add dx, 2
.HandleAH03_OutputLBAHighWord:
    mov ax, cx
    out dx, ax
    add dx, 2
    out dx, ax
.HandleAH03_SkipLBA:
    pop ax

    ; swap ds, es
    mov bx, es
    mov dx, ds
    mov es, dx
    mov ds, bx

    ; figure out the offset
    mov bx, [ss:bp-2] ; get old ax value (bl contains sector count to move)
    and ax, 0x0007
    je .HandleAH03_SkipAlignment

    ; get blocks to move
    xor cl, cl
    mov ch, 8  ; "effective" multiply by 256 (512 bytes, cx is in words)
    sub ch, al
    cmp ch, bl ; do we terminate early?
    jng .HandleAH03_StartCopy
    ; so we aren't moving a whole block
    mov ch, bl

.HandleAH03_StartCopy:
    sub bl, ch
    ; compute offset into memory
    mov ah, al ; "effective" multiply by 512
    shl ah, 1
    xor al, al
    mov di, 0x1000
    add di, ax
    rep movsw
    test bl, bl
    jz .HandleAH03_FinishCopy
    ; increment the LBA before continuing to main loop
    call IncrementLBA

.HandleAH03_SkipAlignment:
    test bl, 0xF8
    jz .HandleAH03_LastCopy
    mov cx, 2048
    mov di, 0x1000
    rep movsw
    sub bl, 8
    jz .HandleAH03_FinishCopy
    ; increment the LBA before the next block
    ; can the following be replaced by (push SkipAlignment, jmp IncrementLBA ?)
    call IncrementLBA
    jmp .HandleAH03_SkipAlignment

.HandleAH03_LastCopy:
    xor cl, cl
    mov ch, bl
    mov di, 0x1000
    rep movsw

.HandleAH03_FinishCopy:
    ; swap ds, es
    mov bx, es
    mov dx, ds
    mov es, dx
    mov ds, bx

    ; setup results
    mov ax, WORD [ss:bp-2]
    xor ah, ah
    clc
    mov WORD [ss:bp-2], ax
    jmp .FinishInt13hHandler

.BIOSInt13h:
    ; stack frame looks like
    ;        flags
    ;        segment
    ;        offset
    ; bp --> bp (caller's)

    ; call original int 13h call
    push bp
    ; simulate int instruction's return data
    push WORD [ss:bp+6]    ; flags
    push cs                ; segment
    push .ReturnFromInt13h ; offset
    ; push old handler and restore variables
    push WORD [ds:bios_int13h_segment]
    push WORD [ds:bios_int13h_offset]
    mov ax, WORD [ss:bp-2]
    mov ds, WORD [ss:bp-4]
    mov cx, WORD [ss:bp-6]
    mov di, WORD [ss:bp-8]
    mov dx, WORD [ss:bp-10]
    mov bp, WORD [ss:bp]
    retf

.ReturnFromInt13h:
    ;int 3
    pop bp

    ; overwrite registers which store return data from the old int 13h function
    mov WORD [ss:bp-2], ax
    mov WORD [ss:bp-6], cx
    mov WORD [ss:bp-8], di
    mov WORD [ss:bp-10], dx
    mov WORD [ss:bp-12], si

    ; setup vars pointer
    mov ax, cs
    mov ds, ax

.FinishInt13hHandler:
    ; restore registers
    mov ax, WORD [ss:bp-2]
    mov ds, WORD [ss:bp-4]
    mov cx, WORD [ss:bp-6]
    mov di, WORD [ss:bp-8]
    mov dx, WORD [ss:bp-10]
    mov si, WORD [ss:bp-12]

    ; restore stack frame
    mov sp, bp
    pop bp

    ; we want the current flags to be returned to the caller, but iret
    ; preserves the caller's flags. we can get around this by using
    ; a far return with a '2' passed so it skips over the flags entry
    ; on the stack
    retf 2

    ; clobbers ax, dx
IncrementLBA:
    push dx
    push ax
    mov dx, 0xD000
    in ax, dx
    add ax, 8
    out dx, ax
    add dx, 2
    in ax, dx
    adc ax, 0
    out dx, ax
    add dx, 2
    out dx, ax
    pop ax
    pop dx
    ret

    section .data align=4
indicator:           dq 0xefcdab8967452301
bios_int13h_segment: dw 0x0000
bios_int13h_offset:  dw 0x0000
bios_int19h_segment: dw 0x0000
bios_int19h_offset:  dw 0x0000
vdisk_drivenum:      db 0x00
rom_message:         db 13, 10, "-= Virtual Disk Driver =-", 13, 10
                     db "v0.0.1 (2020-05-06)", 13, 10
                     db "Released under GNU GPL v2", 13, 10, 0

;times 8191-($-$$) db 0x00
;    db 0x00 ; checksum
