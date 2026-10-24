; QueryPerformanceCounter hook entry. Forwards the caller's return address and the state the
; render thread's 1 ms wait keeps in callee-saved registers (rbx = wait start ticks, xmm6 = wait
; length in seconds) to QpcHookC, then tail-jumps so QpcHookC returns straight to the caller.
EXTERN QpcHookC:PROC
.code
QpcHookStub PROC
    mov     rdx, [rsp]          ; return address
    mov     r8, rbx             ; wait start ticks
    movd    r9d, xmm6           ; wait length (float bits)
    jmp     QpcHookC
QpcHookStub ENDP
END
