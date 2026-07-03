; =============================================================================
;  loader_stub.asm - position-independent user-mode loader stub (x64).
;
;  Assembled by MASM (ml64.exe). The kernel driver copies these bytes into a
;  RWX page in the target process and runs them via RtlCreateUserThread with
;  a pointer to a StubData block as the thread StartParameter (-> rcx).
;
;  Win64 ABI: rcx = arg1 = StubData*. StubData layout (see modern_injector.cpp):
;    +0x00 LoadLibraryA    +0x08 GetProcAddress   +0x10 ExitThread
;    +0x18 ImageBase       +0x20 SizeOfImage      +0x24 ImportDirRva
;    +0x28 ImportDirSize   +0x2C EntryPointRva
;
;  What it does:
;    1. Walk IMAGE_IMPORT_DESCRIPTOR[] at (ImageBase + ImportDirRva).
;    2. For each: LoadLibraryA(ImageBase + desc.Name); resolve every function
;       via GetProcAddress (by name or ordinal) and store into the IAT.
;    3. Call DllMain(ImageBase, DLL_PROCESS_ATTACH=1, NULL).
;    4. ExitThread(0) so the remote thread ends cleanly.
;
;  NOTES:
;    - The stub calls only kernel32 functions resolved at injection time; it has
;      no fixed addresses and is fully position-independent.
;    - "ImageBase" below is an EQU (constant offset into [rbp]), NOT a label.
; =============================================================================

PUBLIC LoaderStubStart
PUBLIC LoaderStubEnd

.DATA
; (no data; everything is position-independent code)

.CODE

LoaderStubStart PROC

    ; ---- prologue: save clobbered nonvolatile regs, reserve shadow space ----
    push    rbp
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 28h

    mov     rbp, rcx                 ; rbp = StubData*

    ; ---- rsi = ImageBase + ImportDirRva  (current IMAGE_IMPORT_DESCRIPTOR*) ----
    mov     eax, DWORD PTR [rbp + 24h]      ; ImportDirRva
    add     rax, QWORD PTR [rbp + 18h]      ; + ImageBase
    mov     rsi, rax

next_import:
    ; if desc.Name (rsi+0Ch) == 0 -> imports done
    cmp     DWORD PTR [rsi + 0Ch], 0
    je      imports_done

    ; LoadLibraryA(ImageBase + desc.Name) -> r13
    mov     eax, DWORD PTR [rsi + 0Ch]      ; Name RVA
    add     rax, QWORD PTR [rbp + 18h]      ; + ImageBase
    mov     rcx, rax                        ; arg1 = LPCSTR dll name
    mov     rax, QWORD PTR [rbp + 0]        ; LoadLibraryA
    call    rax                             ; rax = HMODULE (or 0)
    test    rax, rax
    je      skip_import                     ; load failed -> next import
    mov     r13, rax                        ; r13 = module handle

    ; r14 = ImageBase + (OriginalFirstThunk ?: FirstThunk)   ; INT
    mov     eax, DWORD PTR [rsi + 10h]      ; OriginalFirstThunk
    test    eax, eax
    jnz     have_int
    mov     eax, DWORD PTR [rsi + 0]        ; fallback to FirstThunk (bound)
have_int:
    add     rax, QWORD PTR [rbp + 18h]      ; + ImageBase
    mov     r14, rax                        ; r14 = INT pointer

    ; r15 = ImageBase + FirstThunk                              ; IAT
    mov     eax, DWORD PTR [rsi + 0]
    add     rax, QWORD PTR [rbp + 18h]
    mov     r15, rax                        ; r15 = IAT pointer

next_thunk:
    mov     rax, QWORD PTR [r14]            ; INT entry (thunk)
    test    rax, rax
    je      import_done                     ; null terminator -> next import

    ; "imported by ordinal" iff bit 63 is set. x64 thunks set 0x8000000000000000.
    ; MASM cannot encode a 64-bit immediate in `test`, so use `bt` (sets CF
    ; from bit 63) followed by `jc`.
    bt      rax, 63
    jc      by_ordinal

    ; by name: GetProcAddress(module, ImageBase + thunk + 2)
    add     rax, QWORD PTR [rbp + 18h]      ; RVA -> VA (Hint+Name struct)
    add     rax, 2                          ; skip 2-byte hint -> name string
    mov     rdx, rax                        ; arg2 = function name
    mov     rcx, r13                        ; arg1 = module handle
    mov     rax, QWORD PTR [rbp + 8]        ; GetProcAddress
    jmp     call_resolve

by_ordinal:
    ; GetProcAddress(module, ordinal-with-high-bit). The thunk value already
    ; has the high bit set; pass it as the LPCSTR argument (GPA's documented
    ; by-ordinal convention).
    mov     rdx, rax                        ; arg2 = ordinal
    mov     rcx, r13                        ; arg1 = module handle
    mov     rax, QWORD PTR [rbp + 8]        ; GetProcAddress

call_resolve:
    call    rax                             ; rax = function pointer

store_iat:
    mov     QWORD PTR [r15], rax            ; write resolved ptr into IAT
    add     r14, 8                          ; next INT thunk (QWORD on x64)
    add     r15, 8                          ; next IAT slot
    jmp     next_thunk

import_done:
skip_import:
    add     rsi, 14h                        ; sizeof(IMAGE_IMPORT_DESCRIPTOR) = 20
    jmp     next_import

imports_done:
    ; ---- DllMain(ImageBase, DLL_PROCESS_ATTACH=1, NULL) ----
    xor     r9d, r9d                        ; arg4 reserved (unused)
    xor     r8d, r8d                        ; arg3 lpvReserved = NULL
    mov     edx, 1                          ; arg2 fdwReason = DLL_PROCESS_ATTACH
    mov     eax, DWORD PTR [rbp + 2Ch]      ; EntryPointRva
    add     rax, QWORD PTR [rbp + 18h]      ; + ImageBase -> DllMain VA
    mov     rcx, QWORD PTR [rbp + 18h]      ; arg1 hInstance = ImageBase
    call    rax                             ; DllMain(...) -> BOOL (ignored)

    ; ---- ExitThread(0) ----
    xor     ecx, ecx                        ; arg1 = exit code 0
    mov     rax, QWORD PTR [rbp + 10h]      ; ExitThread
    call    rax                             ; does not return

    ; (unreachable) fallback in case ExitThread somehow returns
    add     rsp, 28h
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbp
    ret

LoaderStubStart ENDP

; One-past-end marker so the driver can compute the stub length as
; (LoaderStubEnd - LoaderStubStart). Must be a separate PROC so it gets its
; own address; using a label inside the PROC would be valid too.
LoaderStubEnd PROC
    ret
LoaderStubEnd ENDP

END
