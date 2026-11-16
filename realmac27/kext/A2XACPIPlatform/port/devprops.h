// devprops.h - parser for the EFI device-properties database (IODeviceTree:/efi "device-properties").
//
// WHY: on macOS 27.2 bare metal the platform expert is ours (A2XACPIPlatformExpert); Apple's AppleACPIPlatform,
// whose job it is to apply this blob to IORegistry nodes, is not loaded. Without this, every OpenCore
// DeviceProperties entry is dropped (built-in, layout-id, model ...): en0 reads IOBuiltin=No, the NVMe reads
// External, Apple-Account sign-in fails -44006 (measured).
//
// Format (Apple EFI_DEVICE_PATH_PROPERTY_DATABASE GetPropertyBuffer, as OpenCore produces it), little endian:
//   buffer : u32 size (whole buffer), u32 version (1), u32 node_count, nodes...
//   node   : u32 size (whole node incl. this header), u32 prop_count, EFI device path (ends with 7F FF 04 00),
//            then prop_count x { key: u32 size (incl. the 4), UTF-16LE NUL-terminated; value: u32 size, bytes }
//
// Pure C++ with no kernel or libc dependencies beyond fixed-width ints, so the same file is compiled into the kext
// and into the host selftest. Parsing is ALL-OR-NOTHING: any structural defect refuses the whole blob and nothing
// is applied (a boot-path kext must fail closed, never half-apply and never read past the buffer).
#ifndef A2X_DEVPROPS_H
#define A2X_DEVPROPS_H

#include <stdint.h>
#include <stddef.h>

// Bounds chosen by us, generous for any real config (a typical one has about 4 nodes): a bigger blob is refused, not truncated.
#define A2X_DP_MAX_BLOB    (1u << 20)   // 1 MiB
#define A2X_DP_MAX_NODES   64u
#define A2X_DP_MAX_PROPS   256u         // per node
#define A2X_DP_MAX_PCI     8u           // PCI hops below the root (bridges nest; 8 is deeper than any board)
#define A2X_DP_MAX_DPNODES 16u          // device-path nodes per path
#define A2X_DP_MAX_KEY     128u         // ASCII chars incl. NUL
#define A2X_DP_MAX_VALUE   (64u << 10)  // 64 KiB per value

struct a2x_dp_pci { uint8_t dev, fn; };

struct a2x_dp_node {
    uint32_t   supported;            // 1 = PciRoot(HID PNP0A03/PNP0A08, UID)/Pci(...)... ; 0 = some other path, skipped
    uint32_t   uid;                  // PciRoot _UID
    uint32_t   npci;
    a2x_dp_pci pci[A2X_DP_MAX_PCI];  // root first
    uint32_t   props_off;            // offset of the first property inside the blob
    uint32_t   nprops;
    uint32_t   node_off, node_size;
};

struct a2x_dp_db {
    uint32_t    nnodes;
    uint32_t    trailing;            // bytes after header.size (ignored, reported)
    a2x_dp_node node[A2X_DP_MAX_NODES];
};

struct a2x_dp_prop {
    char           key[A2X_DP_MAX_KEY];
    uint32_t       key_ok;           // 0 = key not printable ASCII / too long: caller must skip it
    const uint8_t *val;
    uint32_t       vlen;
};

// Returns 0 and fills db when the whole blob is well formed; otherwise a negative code, *why names the defect,
// and db->nnodes == 0.
int a2x_dp_parse(const uint8_t *blob, size_t len, a2x_dp_db *db, const char **why);

// Iterates the properties of a node that a2x_dp_parse accepted. *cursor starts at node->props_off.
// Returns 1 with *p filled, 0 at the end. Re-checks every bound (never trusts the earlier pass).
int a2x_dp_next_prop(const uint8_t *blob, size_t len, const a2x_dp_node *node, uint32_t *index,
                     uint32_t *cursor, a2x_dp_prop *p);

// "PciRoot(0x2)/Pci(0x1D,0x0)/Pci(0x0,0x0)" (or "<unsupported path>"), always NUL-terminated.
void a2x_dp_path_str(const a2x_dp_node *node, char *out, size_t cap);

// First node at index >= start whose PciRoot UID and PCI chain (root first) equal the given ones; -1 if none.
int a2x_dp_find(const a2x_dp_db *db, uint32_t uid, const a2x_dp_pci *pci, uint32_t npci, uint32_t start);

// ACPI _UID as our nubs publish it (a string): decimal or 0x-hex, <= 0xFFFFFFFF. Returns 0 for a name-like or
// empty _UID ("GigaLPCRS"), which can never be a PciRoot UID.
int a2x_dp_parse_uid(const char *s, uint32_t *uid);

// Keys that describe the device's own resources; a blob may not overwrite them (it would re-map BARs or IRQs).
int a2x_dp_key_denied(const char *key);

#endif
