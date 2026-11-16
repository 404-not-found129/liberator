/* abi_audit.c — audit_token_t BY VALUE for imports sigdb never saw. #included by objc_bridge.c right after
   abi_structs_gen.c, which defines audit_token_t, A2X_RX, a2x_need and __arm_sp for both.

   Without this the menu bar refuses every app. MenuBarAgent's menu-items server logs, for each connecting app,
      "Cannot identify app for connection from pid: N" after _LSCopyMatchingApplications(LSAuditToken=$0000…0000):
      the peer's token reaches LaunchServices as 32 zero bytes, so no app registers its menus and the bar
      shows only its built-in search icon. A native x86_64 AppKit control is refused identically, so the
      fault is on the interpreted server's side, not in any app.
   The mechanism: a 32-byte struct by value is a pointer in one x register on arm64 (AAPCS64 composite > 16
      bytes) but is copied onto the stack on x86-64. sigdb is built from public headers; libbsm's audit_token_to_*
      family and the private LaunchServices / TCC / sandbox calls are not in it, so they fell to the generic
      adapter, which passes the pointer as a scalar and leaves the native callee reading its stack. The calls, as
      the unmodified MenuBarAgent makes them: _LSCopyLSASNForAuditToken(w0 = -2, x1 = &token),
      audit_token_to_pid(x0 = &token), tcc_credential_create_for_process_with_audit_token(x0 = &token).
   Every handler is keyed on the type (audit_token_t by value), never on a caller: any interpreted daemon that
      identifies its peers through these calls would get the same zero token. */
#define A2X_AT(i) (*(const audit_token_t *)(A2X_RX(i)))
#define A2X_AT_TO(name, T) \
    void __arm2x86_abi_at_##name(void) { \
        static T (*f)(audit_token_t); if (!f) f = a2x_need(#name); \
        A2X_RX(0) = (unsigned long)f(A2X_AT(0)); }
A2X_AT_TO(audit_token_to_auid, unsigned int)
A2X_AT_TO(audit_token_to_euid, unsigned int)
A2X_AT_TO(audit_token_to_egid, unsigned int)
A2X_AT_TO(audit_token_to_ruid, unsigned int)
A2X_AT_TO(audit_token_to_rgid, unsigned int)
A2X_AT_TO(audit_token_to_pid, int)
A2X_AT_TO(audit_token_to_asid, int)
A2X_AT_TO(audit_token_to_pidversion, int)
A2X_AT_TO(tcc_credential_create_for_process_with_audit_token, void *)
/* void audit_token_to_au32(audit_token_t, uid_t *auid, uid_t *euid, gid_t *egid, uid_t *ruid, gid_t *rgid,
   pid_t *pid, au_asid_t *asid, au_tid_t *tid): token pointer in x0, seven pointers in x1..x7, the ninth
   argument in the first arm64 stack slot. */
void __arm2x86_abi_at_audit_token_to_au32(void) {
    static void (*f)(audit_token_t, void *, void *, void *, void *, void *, void *, void *, void *);
    if (!f) f = a2x_need("audit_token_to_au32");
    f(A2X_AT(0), (void *)A2X_RX(1), (void *)A2X_RX(2), (void *)A2X_RX(3), (void *)A2X_RX(4),
      (void *)A2X_RX(5), (void *)A2X_RX(6), (void *)A2X_RX(7), (void *)*(unsigned long *)(__arm_sp + 0));
}
/* LSASNRef _LSCopyLSASNForAuditToken(LSSessionID, audit_token_t) — private; shape read from MenuBarAgent's
   four call sites (w0 = kLSDefaultSessionID -2, x1 = &token). */
void __arm2x86_abi_at__LSCopyLSASNForAuditToken(void) {
    static void *(*f)(int, audit_token_t); if (!f) f = a2x_need("_LSCopyLSASNForAuditToken");
    A2X_RX(0) = (unsigned long)f((int)A2X_RX(0), A2X_AT(1));
}
/* int sandbox_check_by_audit_token(audit_token_t, const char *operation, enum sandbox_filter_type, ...).
   Variadic: on Apple arm64 the unnamed arguments are on the stack, so the filter argument (at most one for
      every filter type) is read from the first stack slot; a second slot is forwarded too, harmlessly, since a
      callee reads only what the filter type asks for. */
void __arm2x86_abi_at_sandbox_check_by_audit_token(void) {
    static int (*f)(audit_token_t, const char *, int, ...); if (!f) f = a2x_need("sandbox_check_by_audit_token");
    A2X_RX(0) = (unsigned long)(unsigned int)f(A2X_AT(0), (const char *)A2X_RX(1), (int)A2X_RX(2),
                                               *(unsigned long *)(__arm_sp + 0), *(unsigned long *)(__arm_sp + 8));
}
#undef A2X_AT_TO
#undef A2X_AT
