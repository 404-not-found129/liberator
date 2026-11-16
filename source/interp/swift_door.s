// swift_door.s — the interpreter's call into a native swift function.
//
// Why a second door. Once the universal trampoline was in place, every remaining crash whose callee
//    could be named was a Swift-mangled symbol — $s7Combine9PublishedV12initialValueACyxGx_tcfC,
//    $s11Observation0A9RegistrarVACycfC, $s8Dispatch0A3QoSV11unspecifiedACvgZ. Those use `swiftcc`, and
//    swiftcc's context registers are not the same on the two architectures:
//
//        role          arm64     x86-64
//        swiftself      x20       r13
//        swifterror     x21       r12
//        swiftasync     x22       r14
//
//    The C door carries x0-x7/v0-v7 and nothing else, so a native Swift method read `self` out of whatever
//    r13 happened to hold. That is one register's worth of distance between a crash and a method call.
//
// Why asm and not IR. r12, r13 and r14 are callee-saved under SysV, so no C or LLVM-IR call can be made
//    to place a value in them: the compiler owns them. (swiftcc in IR would, but it refuses to be variadic
//    and pins where a swiftself parameter may sit, which does not fit one door for unknown callees.) Being
//    callee-saved also means this function must save and restore all three for its own caller — Swift
//    treats them as argument registers and the callee will clobber them.
//
// No __arm2x86_cbarg on the arguments here, deliberately: guest pages carry no PROT_EXEC, so a guest
//    function pointer handed to native code faults on the instruction fetch and recover_guest_call() in
//    host.c interprets it. The fault path covers every argument this door does not wrap.
//
// void __arm2x86_swift_door(unsigned long tgt, unsigned long *xs, unsigned char *vs, unsigned long asp,
//                          unsigned long nint)   nint = 6, 7 or 8 live integer argument registers
//   in : xs[0..5] -> rdi,rsi,rdx,rcx,r8,xs[20..22] -> r13,r12,vs -> xmm0..7
//        64 words from the arm64 stack at asp -> the outgoing stack-argument area
//   out: xs[0]=rax, xs[1]=rdx, xs[21]=r12 (the error Swift returns), vs[0..31]=xmm0,xmm1
.text
.p2align 4
.globl ___arm2x86_swift_door
___arm2x86_swift_door:
  .cfi_startproc
  pushq %rbp
  .cfi_def_cfa_offset 16
  .cfi_offset %rbp, -16
  movq %rsp, %rbp
  .cfi_def_cfa_register %rbp
  pushq %rbx
  .cfi_offset %rbx, -24
  pushq %r12
  .cfi_offset %r12, -32
  pushq %r13
  .cfi_offset %r13, -40
  pushq %r14
  .cfi_offset %r14, -48
  pushq %r15
  .cfi_offset %r15, -56
  // The outgoing stack-argument area is 64 words. 16 was too few: a call with 8 register arguments and 15
  //    stack words (SwiftUI, Font Book: x6/x7 plus [sp+0..0x70]) needs 17, so the 17th argument read [rsp+128] =
  //    the target itself — native r14 = 0x56415741e5894855 = the first 8 bytes of x16 (push rbp; mov rbp,rsp;
  //    push r15; push r14), and areWitnessTablesEqual dereferenced it as a witness table. The extra
  //    words copied from the guest stack are unread by a callee that takes fewer.
  // 536, not 528 (512 area + target + nint = 528, and the frame must be 8 mod 16): five pushes plus the return address leave rsp at 8 mod 16, and the call itself
  //    requires 16-byte alignment. 0..127 is the outgoing stack-argument area, 128 holds the target.
  // The explicit stack alignment is kept together with the bounded 64-word argument area.
  // Realign unconditionally (entry can be 8 off: ControlCenter took a movaps #GP in swift getOrInsert);
  //   the epilogue restores rsp from rbp, so the and-mask is undone exactly. 160 keeps 0..127 args + 128/136 slots.
  andq $-16, %rsp
  subq $544, %rsp

  movq %rsi, %rbx                  // xs  — callee-saved, so it survives the call
  movq %rdx, %r15                  // vs  — likewise
  movq %rdi, 512(%rsp)             // the target
  movq %r8,  520(%rsp)             // nint: how many integer argument registers this call really used

  // arm64 has eight integer argument registers and x86-64 has six, so arm64's 7th and 8th
  //    register arguments belong in the first two stack slots here — but only if they are arguments at all.
  //    A stack argument also appears when the float pool runs out (3 ints and 9 doubles puts the 9th double
  //    at [sp] with x6/x7 unused), and inserting them there moves every stack argument two slots.
  //    Neither fixed choice is correct: always inserting keeps Image Playground alive and breaks Automator
  //    and Console, never inserting does the reverse (both deterministic over repeated runs).
  // nint is the answer, from liveness rather than a signature: x0-x7 are caller-saved on arm64, so the
  //    interpreter poisons x6/x7 wherever a real callee would have destroyed them (a64_rt.c
  //    A64_POISON_ARGREGS) and a surviving poison means the guest never wrote that register for this call.
  movq  %rcx, %r9                  // the guest's stack-argument area
  xorq  %r10, %r10                 // byte offset into the outgoing area
  movq  520(%rsp), %rax
  cmpq  $7, %rax
  jl    L_sd_copy
  movq  48(%rbx), %rdx             // x6 is live -> the 7th integer argument
  movq  %rdx, 0(%rsp)
  movq  $8, %r10
  cmpq  $8, %rax
  jl    L_sd_copy
  movq  56(%rbx), %rdx             // x7 is live -> the 8th
  movq  %rdx, 8(%rsp)
  movq  $16, %r10
  // the guest's own stack words follow, in order, filling the rest of the 512-byte area
L_sd_copy:
  xorq  %r11, %r11
L_sd_loop:
  movq  %r10, %rdx
  leaq  (%rdx,%r11,8), %rdx
  cmpq  $512, %rdx
  jae   L_sd_done
  movq  (%r9,%r11,8), %rax
  movq  %rax, (%rsp,%rdx,1)
  incq  %r11
  jmp   L_sd_loop
L_sd_done:

  movdqu    0(%r15), %xmm0
  movdqu   16(%r15), %xmm1
  movdqu   32(%r15), %xmm2
  movdqu   48(%r15), %xmm3
  movdqu   64(%r15), %xmm4
  movdqu   80(%r15), %xmm5
  movdqu   96(%r15), %xmm6
  movdqu  112(%r15), %xmm7

  movq  160(%rbx), %r13            // x20 -> swiftself
  movq  168(%rbx), %r12            // x21 -> swifterror
  movq  176(%rbx), %r14            // x22 -> swiftasync
  movq    0(%rbx), %rdi
  movq    8(%rbx), %rsi
  movq   16(%rbx), %rdx
  movq   24(%rbx), %rcx
  movq   32(%rbx), %r8
  movq   40(%rbx), %r9
  // rax is the swift indirect-result register on x86-64, so it cannot hold
  //    the call target. arm64 swiftcc puts the indirect result in x8; x86-64 swiftcc puts it in RAX (not
  //    rdi — a swiftcc sret does not shift the integer arguments the way a C sret does). With the target in
  //    rax, every guest call to a native Swift function that returns indirectly handed the callee its own
  //    entry address as the place to store the result.
  //    Example, Maps and Weather identically: guest `blraa x16, x17` into
  //    $s2os6LoggerV9subsystem8categoryACSS_SStcfC with x0..x3 = the small Strings "com.apple.Maps" and
  //    "WarmingSheet" (correct) and x8 = 0x1016dea58 (the result buffer). The native disassembly reads
  //    `movq %rax, %rbx` at entry and `movq %r14, (%rbx)` at +0x63 — and the run died with SIGBUS whose
  //    faulting address was 0x7ff90ad5cf40, the function's own first byte. Journal died the same way in
  //    $s2os12OSSignposterVACycfC.
  // The target lives in r11, which is caller-saved under SysV and is not an argument register, so
  //    nothing else in this frame changes. x8 -> rax mirrors host.c's `xs[8] = ss->__rax` on the way in;
  //    this is the same rule in the outbound direction.
  // Harmless when the callee returns directly: x8 is a scratch register on arm64 and rax is a scratch
  //    register on x86-64, so a callee with no indirect result simply overwrites it — which is also why
  //    the result read-back below still takes rax.
  movq  512(%rsp), %r11
  movq   64(%rbx), %rax            // x8 -> the Swift indirect result pointer
  callq *%r11

  movq %rax, 0(%rbx)
  movq %rdx, 8(%rbx)
  // swiftcc returns up to four integer registers on both architectures: rax,rdx,rcx,r8 <-> x0,x1,x2,x3.
  // Caller-saved on arm64, so a callee that returns fewer simply leaves junk the guest cannot rely on.
  movq %rcx, 16(%rbx)
  movq %r8,  24(%rbx)
  movq %r12, 168(%rbx)             // the error Swift hands back, into x21
  movdqu %xmm0,  0(%r15)
  movdqu %xmm1, 16(%r15)
  movdqu %xmm2, 32(%r15)
  movdqu %xmm3, 48(%r15)

  // An async callee returns with retq $8 (the x86-64 swiftasync convention pops one extra word), so rsp
  // here is 8 higher than it was after the callq. Restore it from rbp instead of trusting the callee:
  // with addq $168 every pop below was one slot off and the final retq jumped to a stack address.
  leaq -40(%rbp), %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  retq
  .cfi_endproc
