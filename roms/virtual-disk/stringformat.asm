	section .text
; convert (un)signed dword to string with arbitrary output precision
;  arguments:
;   eax - dword to convert
;   es:edi - pointer to memory
;  returns:
;   edi - incremented by characters written
;  modifies:
;   eax
;   ecx
;   edi
put_dec_dword_signed:
	; bail if number isn't negative
	test eax, 0x80000000
	jz put_dec_dword
	; output '-' if negative and take the two's complient
	mov byte [es:edi], '-'
	not eax
	inc eax
	inc edi
	; fallthrough to unsigned print function
put_dec_dword:
	; preserve modified registers
	push edx
	xor edx, edx
	; find starting offset into divisor table
	mov ecx, dec_divisor_table-4
.get_extent:
	cmp ecx, dec_divisor_table_end-4
	je .final_digit
	add ecx, 4
	cmp eax, [ds:ecx]
	jb .get_extent
.compute_digit:
	div dword [ds:ecx]
	add eax, '0'
	a32 stosb
	mov eax, edx
	xor edx, edx
	add ecx, 4
	cmp ecx, dec_divisor_table_end
	jne .compute_digit
.final_digit:
	add eax, '0'
	a32 stosb
	pop edx
	ret

; convert dword into hexadecimal string with variable output precision
;  arguments:
;   eax - dword to convert
;   cl - bits to show (must be a multiple of 4)
;   es:edi - pointer to memory
;  returns:
;   edi - incremented by outputed digit
;  modifies:
;   eax
;   cl
;   edi
put_hex_dword:
	push ebx
	push edx
	mov ebx, hex_lookup_table
	mov edx, eax
.output_digit:
	sub cl, 4
	mov eax, edx
	shr eax, cl
	and al, 0xF
	a32 xlat
	a32 stosb
	test cl, 0xFF
	jnz .output_digit
	pop edx
	pop ebx
	ret

; prints a formatted string to a file
;  format tokens are the following
;   decimal         hex    binary    escape
;    %ub - u8        %xb    %bb       %%% - print '%'
;    %uw - u16       %xw    %bw
;    %ud - u32       %xd    %bd
;    %sb - s8
;    %sw - s16
;    %sd - s32
; arguments
;  bx - file handle
;  ebp - stack frame pointing at first byte after the end of the arguments block
;  ecx - length of format string
;  es:edi - pointer to format string 
; modifies:
;  esi
;  edi
;  ecx
; stack arguments
;  2 byte alignment (for 16 bit mode), in reverse order in memory (higher address = earlier argument)
fprintf:
	cld
	push ebp
	sub esp, 10  ; largest string output (negative s32 is 10 characters, with '-' sign)
	mov esi, edi
.next:
	mov edi, esi
	mov al, '%'
	a32 repne scasb
	; stash remaining string length
	push ecx
	mov ecx, edi
	jne .eos
	dec ecx
.eos:
	sub ecx, esi
	mov edx, esi
	mov ah, 0x40
	int 0x21
	pop ecx
	cmp ecx, 2 ; if there isn't enough string for format arguments, skip
	jb .end

	; read format arguments
	mov esi, edi
	a32 lodsw
	sub ecx, 2

	; setup window into temporary memory
	mov edx, esp
	mov edi, esp

	; decide on format arguments
	push ecx
	cmp ax, 0x2525
	je .format_escape
	push .print
	sub ebp, 2
	mov ecx, 8
	cmp ax, 0x6268
        je .format_h8
	cmp ax, 0x6273
        je .format_s8
        cmp ax, 0x6275
        je .format_u8
	mov ecx, 16
	cmp ax, 0x7768
        je .format_h16
	cmp ax, 0x7773
        je .format_s16
        cmp ax, 0x7775
        je .format_u16
	sub ebp, 2
	mov ecx, 32
	cmp ax, 0x6468
	je .format_h32
	cmp ax, 0x6473
        je .format_s32
        cmp ax, 0x6475
        je .format_u32
	add ebp, 4
	; note - this is pulling off the manual return address push
	pop cx
	pop ecx
	jmp .next
.format_u8:
	movzx eax, byte [ss:ebp]
	jmp put_dec_dword
.format_s8:
	movsx eax, byte [ss:ebp]
	jmp put_dec_dword_signed
.format_u16:
	movzx eax, word [ss:ebp]
	jmp put_dec_dword
.format_s16:
	movsx eax, word [ss:ebp]
	jmp put_dec_dword_signed
.format_u32:
	mov eax, dword [ss:ebp]
	jmp put_dec_dword
.format_s32:
	mov eax, dword [ss:ebp]
	jmp put_dec_dword_signed
.format_h8:
	movzx eax, byte [ss:ebp]
	jmp put_hex_dword
.format_h16:
	movzx eax, word [ss:ebp]
	jmp put_hex_dword
.format_h32:
	mov eax, dword [ss:ebp]
	jmp put_hex_dword
.format_escape:
	mov al, '%'
	a32 stosb
.print:
	sub edi, edx
	mov ecx, edi
	mov ah, 0x40
	int 0x21
	pop ecx
	jmp .next
.end:
	add esp, 10
	pop ebp
	ret

; tables for conversion functions
	section .data align=4
dec_divisor_table:
	dd 1000000000
	dd 100000000
	dd 10000000
	dd 1000000
	dd 100000
	dd 10000
	dd 1000
	dd 100
	dd 10
dec_divisor_table_end:

hex_lookup_table:
	db '0', '1', '2', '3'
	db '4', '5', '6', '7'
	db '8', '9', 'A', 'B'
	db 'C', 'D', 'E', 'F'
hex_lookup_table_end:
