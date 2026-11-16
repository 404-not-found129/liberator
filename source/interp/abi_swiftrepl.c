/* abi_swiftrepl.c - swift_getFunctionReplacement(slot, current function). The runtime compares the function stored
   in the slot with the caller's own address and returns null when they are the same. The fixed-shape door replaces
   an argument that is the start of a guest function with a native entry stub, so the two never compared equal, the
   runtime returned the function itself, and the caller jumped to its own start for ever. Both values go through
   unchanged here. */
void __arm2x86_abi_swift_getFunctionReplacement(void) {
    static char *(*f)(char **, char *); if (!f) f = a2x_need("swift_getFunctionReplacement");
    A2X_RX(0) = (uint64_t)f((char **)A2X_RX(0), (char *)A2X_RX(1));
}
