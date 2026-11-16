/* tlv.c — thread-local variables of a translated image. dyld never sees the arm64 image, so its __thread_vars
 * descriptors {thunk, key, offset} are ours to service. Each image gets one pthread key and one per-thread block, created on
 * a thread's first access: the __thread_data template copied in, the __thread_bss rest zero (calloc).
 * The descriptor's thunk becomes __arm2x86_tlv_get, a native function: translated code loads it and `blr`s, which the
 *    dispatcher hands to the native-pointer adapter with x0 = the descriptor. The key field holds our image index.
 * The template is read from the mapped image, after fixups, so a thread_local holding a pointer gets the rebased value. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct img_tlv { unsigned long tdata, tdata_size, tsize; pthread_key_t key; };
static struct img_tlv g_tlv[16];
static int g_ntlv;
void *__arm2x86_tlv_get(unsigned long *desc) {
    struct img_tlv *t = &g_tlv[desc[1] & 15];
    char *b = pthread_getspecific(t->key);
    if (!b) {
        b = calloc(1, 16 + (t->tsize ? t->tsize : 1));
        if (!b) { fprintf(stderr, "[tlv] out of memory for a %lu-byte block\n", t->tsize); a2x_stop("out of memory", "thread-local block"); abort(); }
        b[1] = (char)(desc[1] & 15);   /* header: [0] destructor passes seen, [1] image index (its key) */
        if (t->tdata_size) memcpy(b + 16, (void *)t->tdata, t->tdata_size);
        pthread_setspecific(t->key, b);
    }
    return b + 16 + desc[2];
}
/* The block must not be freed by the first destructor pass. A thread_local's destructor (_tlv_atexit,
 *    run from dyld's own key) may run after ours in that pass and would then destroy freed memory — key order decides,
 *    and nothing promises it. The block survives one extra pass (pthread re-runs a key whose value is set again,
 *    up to PTHREAD_DESTRUCTOR_ITERATIONS = 4): the 16-byte header counts passes, the second one frees. */
static pthread_key_t g_key_of[16];
static void tlv_block_free(void *v) {
    char *b = v;
    if (!b[0]) { b[0] = 1; pthread_setspecific(g_key_of[(unsigned char)b[1]], b); return; }
    free(b);
}
/* vars: the __thread_vars section (24-byte descriptors); tstart/tsize: the TLV template range (__thread_data + __thread_bss) */
void __arm2x86_tlv_register(unsigned long vars, unsigned long nvars, unsigned long tdata, unsigned long tdata_size,
                            unsigned long tsize) {
    if (g_ntlv >= 16) { fprintf(stderr, "[tlv] more than 16 images with thread-locals\n"); a2x_stop("too many images with thread-local variables", ""); abort(); }
    int idx = g_ntlv++;
    g_tlv[idx].tdata = tdata; g_tlv[idx].tdata_size = tdata_size; g_tlv[idx].tsize = tsize;
    if (pthread_key_create(&g_tlv[idx].key, tlv_block_free)) { fprintf(stderr, "[tlv] pthread_key_create failed\n"); a2x_stop("no thread-local key could be made", ""); abort(); }
    g_key_of[idx] = g_tlv[idx].key;
    for (unsigned long i = 0; i < nvars; i++) {
        unsigned long *d = (unsigned long *)(vars + 24 * i);
        d[0] = (unsigned long)&__arm2x86_tlv_get; d[1] = (unsigned long)idx;   /* d[2]: the variable's offset, kept */
    }
}
