#ifndef A2X_BLOCK_SIGNATURE_H
#define A2X_BLOCK_SIGNATURE_H
#include <stddef.h>
#include <string.h>
/* Conservative preflight. Unhandled encodings keep the existing untyped callback route. */
static int a2x_block_enc_type(const char **p, const char *end, unsigned depth, size_t *units) {
    if (depth > 16) return 0;
    *units = 1;
    while (*p < end && strchr("rnNoORV", **p)) ++*p;
    if (*p >= end) return 0;
    char c = *(*p)++;
    if (strchr("cCsSiIlLqQBfdv*#:?", c)) return 1;
    if (c == '^') return a2x_block_enc_type(p, end, depth + 1, units);
    if (c == '@') {
        if (*p < end && **p == '?') ++*p;
        else if (*p < end && **p == '"') {
            ++*p; while (*p < end && **p != '"') ++*p;
            if (*p == end) return 0; ++*p;
        }
        return 1;
    }
    if (c == '[') {
        const char *n = *p; size_t count = 0;
        while (*p < end && **p >= '0' && **p <= '9') {
            unsigned digit = (unsigned)(**p - '0');
            if (count > (4096 - digit) / 10) return 0;
            count = count * 10 + digit; ++*p;
        }
        if (*p == n || !count || !a2x_block_enc_type(p, end, depth + 1, units)) return 0;
        if (*units > 4096 / count) return 0;
        *units *= count;
        if (*p >= end || *(*p)++ != ']') return 0;
        return 1;
    }
    if (c == '{') {
        while (*p < end && **p != '=' && **p != '}') ++*p;
        if (*p >= end || *(*p)++ != '=') return 0;
        unsigned fields = 0; size_t total = 0;
        while (*p < end && **p != '}') {
            if (++fields > 128 || !a2x_block_enc_type(p, end, depth + 1, units)) return 0;
            if (total > 4096 - *units) return 0;
            total += *units;
        }
        if (*p >= end || *(*p)++ != '}' || !fields) return 0;
        *units = total;
        return 1;
    }
    return 0;
}
static int a2x_encoded_signature_valid(const char *s, int block) {
    if (!s) return 0;
    size_t n = strnlen(s, 4097);
    if (!n || n > 4096) return 0;
    const char *p = s, *end = s + n; size_t units;
    if (!a2x_block_enc_type(&p, end, 0, &units)) return 0;
    while (p < end && *p >= '0' && *p <= '9') ++p;
    /* Every block signature starts its arguments with the block context itself. */
    if (block && (end - p < 2 || p[0] != '@' || p[1] != '?')) return 0;
    if (!block && (p == end || (*p != '@' && *p != '#'))) return 0;
    unsigned args = 0;
    while (p < end) {
        if (!block && args == 1 && *p != ':') return 0;
        if (++args > 64 || !a2x_block_enc_type(&p, end, 0, &units)) return 0;
        while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    return block || args >= 2;
}
static int a2x_block_signature_valid(const char *s) { return a2x_encoded_signature_valid(s, 1); }
static const char *a2x_block_descriptor_signature(unsigned flags, const void *desc) {
    if (!(flags & (1u << 30)) || !desc) return NULL;
    const unsigned char *p = desc;
    p += 2 * sizeof(unsigned long);
    if (flags & (1u << 25)) p += 2 * sizeof(unsigned long);
    const char *sig; memcpy(&sig, p, sizeof sig);
    return a2x_block_signature_valid(sig) ? sig : NULL;
}
#endif
