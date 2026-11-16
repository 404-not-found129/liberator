/* abi_cfdate.c - CoreFoundation's Gregorian date calls. CFGregorianDate is 16 bytes, four integers then a double:
   arm64 carries it in two integer registers, x86-64 in one integer register and one floating register.
   CFGregorianUnits is 32 bytes: a pointer for the caller, on the stack for the callee. */
typedef struct { int year; signed char month, day, hour, minute; double second; } a2x_gdate;
typedef struct { int years, months, days, hours, minutes; double seconds; } a2x_gunits;
void __arm2x86_abi_cf_CFAbsoluteTimeGetGregorianDate(void) {
    static a2x_gdate (*f)(double, void *); if (!f) f = a2x_need("CFAbsoluteTimeGetGregorianDate");
    a2x_gdate r = f(A2X_VD(0), (void *)A2X_RX(0));
    uint64_t w[2]; memcpy(w, &r, 16); A2X_RX(0) = w[0]; A2X_RX(1) = w[1]; A2X_XRX1(w[1]);
}
void __arm2x86_abi_cf_CFGregorianDateGetAbsoluteTime(void) {
    static double (*f)(a2x_gdate, void *); if (!f) f = a2x_need("CFGregorianDateGetAbsoluteTime");
    uint64_t w[2] = { A2X_RX(0), A2X_RX(1) }; a2x_gdate g; memcpy(&g, w, 16);
    double r = f(g, (void *)A2X_RX(2)); A2X_VD(0) = r;
}
void __arm2x86_abi_cf_CFGregorianDateIsValid(void) {
    static unsigned char (*f)(a2x_gdate, unsigned long); if (!f) f = a2x_need("CFGregorianDateIsValid");
    uint64_t w[2] = { A2X_RX(0), A2X_RX(1) }; a2x_gdate g; memcpy(&g, w, 16);
    A2X_RX(0) = (uint64_t)f(g, (unsigned long)A2X_RX(2));
}
void __arm2x86_abi_cf_CFAbsoluteTimeAddGregorianUnits(void) {
    static double (*f)(double, void *, a2x_gunits); if (!f) f = a2x_need("CFAbsoluteTimeAddGregorianUnits");
    double r = f(A2X_VD(0), (void *)A2X_RX(0), *(a2x_gunits *)A2X_RX(1)); A2X_VD(0) = r;
}
