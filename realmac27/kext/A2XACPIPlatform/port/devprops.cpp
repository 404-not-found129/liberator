// devprops.cpp - see devprops.h. No allocation, no libc: every read goes through rd32()/bounds checks.
#include "devprops.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// EISA IDs as EFI device paths store them: PNP0A03 (PCI host bridge) and PNP0A08 (PCI Express host bridge).
#define A2X_EISA_PNP0A03 0x0A0341D0u
#define A2X_EISA_PNP0A08 0x0A0841D0u

static int fail(const char **why, const char *msg, int code) { if (why) *why = msg; return code; }

// Parses one EFI device path that must end inside [p, end). Sets *used to its length incl. the end node.
// A path that is well formed but not PciRoot/Pci... is accepted with supported = 0 (skipped, not an error).
static int parse_path(const uint8_t *p, const uint8_t *end, a2x_dp_node *n, uint32_t *used, const char **why)
{
    const uint8_t *s = p;
    n->supported = 1; n->uid = 0; n->npci = 0;
    for (uint32_t k = 0;; k++) {
        if (k >= A2X_DP_MAX_DPNODES) return fail(why, "device path has too many nodes", -20);
        if (end - p < 4) return fail(why, "device path runs past its node", -21);
        uint8_t type = p[0], sub = p[1];
        uint16_t nl = rd16(p + 2);
        if (nl < 4 || (size_t)(end - p) < nl) return fail(why, "device path node length out of bounds", -22);
        if (type == 0x7F && sub == 0xFF) {                       // end of entire path
            if (nl != 4) return fail(why, "device path end node has a bad length", -23);
            p += 4; break;
        }
        if (type == 0x7F) return fail(why, "device path has an end-instance node", -24);   // multi-instance: not a property path
        if (k == 0) {
            if (type == 2 && sub == 1 && nl == 12) {
                uint32_t hid = rd32(p + 4);
                if (hid != A2X_EISA_PNP0A03 && hid != A2X_EISA_PNP0A08) n->supported = 0;
                n->uid = rd32(p + 8);
            } else n->supported = 0;
        } else if (type == 1 && sub == 1 && nl == 6) {
            if (n->npci >= A2X_DP_MAX_PCI) n->supported = 0;
            else { n->pci[n->npci].fn = p[4]; n->pci[n->npci].dev = p[5]; n->npci++; }
            if (p[5] > 31 || p[4] > 7) n->supported = 0;          // not a PCI device/function: never matches
        } else n->supported = 0;
        p += nl;
    }
    if (n->supported && n->npci == 0) n->supported = 0;           // PciRoot alone names the bridge, not a device
    *used = (uint32_t)(p - s);
    return 0;
}

// Checks one key/value pair at [p, end); sets *used. Structural defects refuse the blob.
static int check_prop(const uint8_t *p, const uint8_t *end, uint32_t *used, const char **why)
{
    if (end - p < 4) return fail(why, "property key header past its node", -30);
    uint32_t ks = rd32(p);
    if (ks < 4 + 2 || ks > (uint32_t)(end - p) || (ks & 1)) return fail(why, "property key size out of bounds", -31);
    if (rd16(p + ks - 2) != 0) return fail(why, "property key is not NUL-terminated", -32);
    const uint8_t *v = p + ks;
    if (end - v < 4) return fail(why, "property value header past its node", -33);
    uint32_t vs = rd32(v);
    if (vs < 4 || vs > (uint32_t)(end - v)) return fail(why, "property value size out of bounds", -34);
    if (vs - 4 > A2X_DP_MAX_VALUE) return fail(why, "property value too large", -35);
    *used = ks + vs;
    return 0;
}

int a2x_dp_parse(const uint8_t *blob, size_t len, a2x_dp_db *db, const char **why)
{
    if (why) *why = "ok";
    if (!db) return -1;
    db->nnodes = 0; db->trailing = 0;
    if (!blob) return fail(why, "no blob", -2);
    if (len < 12) return fail(why, "blob shorter than its header", -3);
    if (len > A2X_DP_MAX_BLOB) return fail(why, "blob larger than 1 MiB", -4);
    uint32_t size = rd32(blob), ver = rd32(blob + 4), count = rd32(blob + 8);
    if (size < 12 || size > len) return fail(why, "header size disagrees with the blob length", -5);
    if (ver != 1) return fail(why, "unknown database version", -6);
    if (count > A2X_DP_MAX_NODES) return fail(why, "too many device nodes", -7);
    const uint8_t *p = blob + 12, *end = blob + size;
    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 8) return fail(why, "node header past the buffer", -10);
        uint32_t ns = rd32(p), np = rd32(p + 4);
        if (ns < 8 + 4 || ns > (uint32_t)(end - p)) return fail(why, "node size out of bounds", -11);
        if (np > A2X_DP_MAX_PROPS) return fail(why, "too many properties in a node", -12);
        const uint8_t *nend = p + ns;
        a2x_dp_node *n = &db->node[i];
        uint32_t used = 0;
        int r = parse_path(p + 8, nend, n, &used, why);
        if (r) { db->nnodes = 0; return r; }
        n->node_off = (uint32_t)(p - blob); n->node_size = ns;
        n->props_off = (uint32_t)(p + 8 + used - blob); n->nprops = np;
        const uint8_t *q = p + 8 + used;
        for (uint32_t j = 0; j < np; j++) {
            r = check_prop(q, nend, &used, why);
            if (r) { db->nnodes = 0; return r; }
            q += used;
        }
        if (q != nend) { db->nnodes = 0; return fail(why, "node size disagrees with its contents", -13); }
        p = nend;
    }
    if (p != end) { db->nnodes = 0; return fail(why, "buffer size disagrees with its nodes", -14); }
    db->nnodes = count;
    db->trailing = (uint32_t)(len - size);
    return 0;
}

int a2x_dp_next_prop(const uint8_t *blob, size_t len, const a2x_dp_node *node, uint32_t *index,
                     uint32_t *cursor, a2x_dp_prop *p)
{
    if (!blob || !node || !index || !cursor || !p || *index >= node->nprops) return 0;
    if ((size_t)node->node_off + node->node_size > len || *cursor < node->props_off ||
        *cursor >= node->node_off + node->node_size) return 0;
    const uint8_t *q = blob + *cursor, *nend = blob + node->node_off + node->node_size;
    uint32_t used = 0;
    if (check_prop(q, nend, &used, NULL)) return 0;
    uint32_t ks = rd32(q), nch = (ks - 4) / 2;          // UTF-16 code units incl. the NUL
    p->key_ok = nch <= A2X_DP_MAX_KEY;
    uint32_t i = 0;
    for (; p->key_ok && i + 1 < nch; i++) {
        uint16_t c = rd16(q + 4 + 2 * i);
        if (c < 0x20 || c > 0x7E) { p->key_ok = 0; break; }   // only printable ASCII becomes an OSSymbol
        p->key[i] = (char)c;
    }
    if (!p->key_ok || i == 0) { p->key_ok = 0; i = 0; }
    p->key[i] = 0;
    p->val = q + ks + 4;
    p->vlen = rd32(q + ks) - 4;
    *cursor += used;
    (*index)++;
    return 1;
}

static void put(char *out, size_t cap, size_t *o, const char *s) { while (*s && *o + 1 < cap) out[(*o)++] = *s++; out[*o] = 0; }
static void puthex(char *out, size_t cap, size_t *o, uint32_t v)
{
    char b[11]; int n = 0; b[n++] = '0'; b[n++] = 'x';
    int started = 0;
    for (int sh = 28; sh >= 0; sh -= 4) {
        uint32_t d = (v >> sh) & 0xF;
        if (d || started || sh == 0) { b[n++] = "0123456789ABCDEF"[d]; started = 1; }
    }
    b[n] = 0; put(out, cap, o, b);
}

void a2x_dp_path_str(const a2x_dp_node *n, char *out, size_t cap)
{
    if (!out || !cap) return;
    size_t o = 0; out[0] = 0;
    if (!n || !n->supported) { put(out, cap, &o, "<unsupported path>"); return; }
    put(out, cap, &o, "PciRoot("); puthex(out, cap, &o, n->uid); put(out, cap, &o, ")");
    for (uint32_t i = 0; i < n->npci; i++) {
        put(out, cap, &o, "/Pci("); puthex(out, cap, &o, n->pci[i].dev); put(out, cap, &o, ",");
        puthex(out, cap, &o, n->pci[i].fn); put(out, cap, &o, ")");
    }
}

int a2x_dp_find(const a2x_dp_db *db, uint32_t uid, const a2x_dp_pci *pci, uint32_t npci, uint32_t start)
{
    if (!db || !pci) return -1;
    for (uint32_t i = start; i < db->nnodes; i++) {
        const a2x_dp_node *n = &db->node[i];
        if (!n->supported || n->uid != uid || n->npci != npci) continue;
        uint32_t k = 0;
        while (k < npci && n->pci[k].dev == pci[k].dev && n->pci[k].fn == pci[k].fn) k++;
        if (k == npci) return (int)i;
    }
    return -1;
}

int a2x_dp_parse_uid(const char *s, uint32_t *uid)
{
    if (!s || !uid) return 0;
    uint32_t base = 10; uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    if (!*s) return 0;
    for (; *s; s++) {
        uint32_t d = (*s >= '0' && *s <= '9') ? (uint32_t)(*s - '0') : (*s >= 'a' && *s <= 'f') ? (uint32_t)(*s - 'a' + 10)
                   : (*s >= 'A' && *s <= 'F') ? (uint32_t)(*s - 'A' + 10) : 99;
        if (d >= base) return 0;
        v = v * base + d;
        if (v > 0xffffffffull) return 0;
    }
    *uid = (uint32_t)v;
    return 1;
}

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int a2x_dp_key_denied(const char *key)
{
    static const char *const deny[] = {
        "reg", "assigned-addresses", "ranges", "#address-cells", "#size-cells",
        "IOInterruptSpecifiers", "IOInterruptControllers", "IODeviceMemory",
        "IOName", "IOClass", "IOProviderClass", "IOMatchCategory", "IOPCIMSIMode",
    };
    for (size_t i = 0; i < sizeof deny / sizeof deny[0]; i++) if (streq(key, deny[i])) return 1;
    return 0;
}
