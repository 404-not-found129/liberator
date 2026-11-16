/* vtail.h — is this native x86-64 function variadic? Read from its own prologue, never from a selector list.
 * A type encoding never marks a method variadic, and Apple arm64 puts every variadic argument on the stack,
 *    so a native variadic method sent through the bridge got its tail from whatever x86 registers held (revisiond:
 *    -[PQLConnection execute:] faulted in -[PQLStatement translate:hasInjections:arguments:] with x3 = 0).
 *    clang's x86-64 prologue for a function that calls va_start saves xmm0-7 into the register save area, guarded by
 *    the caller's vector count: `test al,al ; je/jz ; movaps|movups|vmovaps [..], xmm0`. Only a variadic function needs
 *    that guard, so its presence is the property. Checked on 12 methods, both directions, 0 wrong.
 * A false positive is harmless by the SysV ABI (a fixed-argument callee ignores extra integer arguments); a false
 *    negative is exactly the old behaviour. Every read is bounded by the mapped region so it can never run off the end.
 * The implementation needs <mach/mach.h>, which objc_bridge.c cannot include (abi_structs_gen.c defines its own
 *    audit_token_t). It lives in vararg.c (A2X_VTAIL_IMPL); everyone else sees the prototypes only. */
#ifndef A2X_VTAIL_H
#define A2X_VTAIL_H
#include <stdint.h>

int a2x_imp_is_variadic(const void *imp);
/* how many INTEGER arguments are fixed (0..5) for a native C variadic, read from its register save area; -1 if unknown */
int a2x_c_vtail_nfixed(const void *fn);
/* bytes from addr to the end of the mapped, readable region holding it; 0 if it is not mapped */
uint64_t a2x_region_room(uint64_t addr);

#ifdef A2X_VTAIL_IMPL
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dlfcn.h>

uint64_t a2x_region_room(uint64_t addr) {
    mach_vm_address_t a = (mach_vm_address_t)addr; mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t bi; mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ob = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &n, &ob) != KERN_SUCCESS)
        return 0;
    if (ob != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), ob);
    if (a > addr || !(bi.protection & VM_PROT_READ)) return 0;
    return a + sz - addr;
}

int a2x_imp_is_variadic(const void *imp) {
    if (!imp) return 0;
    uint64_t lim = a2x_region_room((uint64_t)(uintptr_t)imp);
    int win = lim < 192 ? (int)lim : 192;
    const unsigned char *b = (const unsigned char *)imp;
    for (int i = 0; i + 12 <= win; i++) {
        /* 0xc3 and 0xcc are also ModRM bytes: `mov %ecx,%r12d` is 41 89 cc and `mov %rax,%rbx` is 48 89 c3, and
              either sits in the prologue of a variadic method that keeps its 4th argument in r12 (Podcasts:
              -[IMLogger logFile:lineNumber:format:] was judged fixed-argument and its %@ argument arrived as a
              garbage r9). Only a ret or int3 FOLLOWED BY what ends a function counts: int3 padding, a nop, or the
              next function`s push %rbp. A miss costs nothing (a false positive is harmless by the ABI). */
        if ((b[i] == 0xcc && b[i + 1] == 0xcc)
            || (b[i] == 0xc3 && (b[i + 1] == 0xcc || b[i + 1] == 0x90 || b[i + 1] == 0x55
                                 || (b[i + 1] == 0x66 && b[i + 2] == 0x90) || (b[i + 1] == 0x0f && b[i + 2] == 0x1f))))
            break;                                           /* a ret or padding: the prologue is behind us */
        if (b[i] != 0x84 || b[i + 1] != 0xc0) continue;      /* test al,al */
        int j = i + 2;
        if (b[j] == 0x74) j += 2;                            /* je rel8 */
        else if (b[j] == 0x0f && b[j + 1] == 0x84) j += 6;   /* je rel32 */
        else continue;
        if (j + 3 > win) break;
        int mr = -1;
        if (j + 3 <= win && b[j] == 0x0f && (b[j + 1] == 0x29 || b[j + 1] == 0x11)) mr = j + 2;
        if (j + 4 <= win && b[j] == 0x66 && b[j + 1] == 0x0f &&
            (b[j + 2] == 0x29 || b[j + 2] == 0x11 || b[j + 2] == 0x7f)) mr = j + 3;
        if (j + 4 <= win && b[j] == 0xf3 && b[j + 1] == 0x0f && b[j + 2] == 0x7f) mr = j + 3;
        if (j + 4 <= win && b[j] == 0xc5 && (b[j + 1] & 0xfc) == 0xf8) {
            unsigned pp = b[j + 1] & 3, op = b[j + 2];
            if (((op == 0x29 || op == 0x11) && pp <= 1) || (op == 0x7f && (pp == 1 || pp == 2))) mr = j + 3;
        }
        /* Current compilers also use movdqa/movdqu for the same XMM save area.
           Require the first guarded store to be xmm0 to memory, not a register move. */
        if (mr >= 0 && (b[mr] >> 6) != 3 && ((b[mr] >> 3) & 7) == 0) {
            /* A short syscall stub can end before a neighboring variadic
               prologue. A known symbol boundary invalidates that match. */
            Dl_info start, guard;
            if (dladdr(imp, &start) && dladdr(b + i, &guard) &&
                start.dli_saddr && guard.dli_saddr &&
                start.dli_saddr != guard.dli_saddr) return 0;
            return 1;
        }
    }
    return 0;
}

/* A native C variadic with no name rule read its tail from junk registers. identityservicesd crash-looped
      (exit 139, 30 runs in 25 min): IMInsertNSStringsToXPCDictionary(dict, key, value, ..., NULL) took its pairs
      from rdx..r9 while Apple arm64 had put every one of them on the stack, and objc_retain faulted on the junk. Each
      run first generated a fresh RSA key in the login keychain: 5,972 unlabeled private keys, 12 MB, 2.1 GB written.
   The fixed count is in the prologue. clang's va_start prologue stores exactly the integer argument registers that
      are not fixed into the register save area, consecutively (reg k at base + 8k), ending at r9, before the
      `test al,al` guard a2x_imp_is_variadic already finds. The first register of that run is nfixed. For example,
      IMInsertNSStringsToXPCDictionary saves rdx,rcx,r8,r9 at rbp-256..-232 → 2.
   Unlike a2x_imp_is_variadic, a wrong answer here is not harmless: it overwrites real register arguments. So the
      run must be exact (consecutive, 8 apart, ending at r9) or the answer is -1, the old behaviour. */
int a2x_c_vtail_nfixed(const void *fn) {
    if (!a2x_imp_is_variadic(fn)) return -1;
    uint64_t lim = a2x_region_room((uint64_t)(uintptr_t)fn);
    int win = lim < 192 ? (int)lim : 192;
    const unsigned char *b = (const unsigned char *)fn;
    /* The save area's base is not always rbp. IO80211's IO8Log2 (WiFiAgent: 218 runs, exit 139, a %s
          read from junk in __CFStringAppendFormatCore) does `lea r10,[rbp-208]; mov [r10+16],rdx ...`, libc printf
          does `lea rdi,[rbp-208]; mov [rdi+8],rsi ...`. So every store is recorded with its base register and only
          the stores sharing the r9 store's base form the run. */
    /* THE 192-BYTE window runs into the next function. A short non-variadic function whose neighbour is
          variadic was read as variadic with the neighbour's count: 41 of 197 hits in the 27 import dump, including
          BOMCopierCopy (3 arguments, "nfixed 2"), whose third argument would be overwritten. Every Apple x86
          function opens with push rbp; mov rbp,rsp, so meeting that sequence again ends this function. */
    int nst = 0, sk[24], sb[24], seen_test = 0; long sd[24];
    for (int i = 0; i + 8 <= win; i++) {
        if (i > 0 && b[i] == 0x55 && b[i + 1] == 0x48 && b[i + 2] == 0x89 && b[i + 3] == 0xe5) return -1;
        if (b[i] == 0x84 && b[i + 1] == 0xc0) { seen_test = 1; break; } /* test al,al: the save area is behind us */
        if ((b[i] & 0xfa) != 0x48 || b[i + 1] != 0x89) continue; /* REX.W with any R/B, no X; mov r/m64, r64 */
        unsigned m = b[i + 2], mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7, rexr = (b[i] >> 2) & 1, rexb = b[i] & 1;
        int k = -1;                                            /* SysV integer argument index of the source reg */
        if (!rexr) k = reg == 7 ? 0 : reg == 6 ? 1 : reg == 2 ? 2 : reg == 1 ? 3 : -1;
        else       k = reg == 0 ? 4 : reg == 1 ? 5 : -1;
        if (k < 0 || mod == 3 || (mod == 0 && rm == 5)) continue; /* register move, or rip-relative */
        int o = i + 3;
        if (rm == 4) { if (b[i + 3] != 0x24) continue; o++; }  /* SIB with no index: base rsp / r12 only */
        int bs = (int)(rm | rexb << 3);
        long d = mod == 0 ? 0 : mod == 1 ? (long)(signed char)b[o] : (long)(int32_t)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24);
        if (nst < 24) { sk[nst] = k; sb[nst] = bs; sd[nst] = d; nst++; }
    }
    if (!seen_test) return -1;
    int r9b = -1;
    for (int j = 0; j < nst; j++) if (sk[j] == 5) { r9b = sb[j]; break; }
    if (r9b < 0) return -1;                                    /* no r9 store: not a register save area we know */
    long disp[6]; int have[6] = {0, 0, 0, 0, 0, 0};
    for (int j = 0; j < nst; j++) if (sb[j] == r9b && !have[sk[j]]) { have[sk[j]] = 1; disp[sk[j]] = sd[j]; }
    int nf = 5;
    while (nf > 0 && have[nf - 1] && disp[nf - 1] == disp[5] - 8L * (5 - (nf - 1))) nf--;
    return nf;
}
#endif
#endif
