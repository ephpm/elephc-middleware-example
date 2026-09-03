/*
 * ephpm_elephc_shim.c — adapter between ePHPm's native-middleware C ABI and
 * an elephc-compiled PHP cdylib.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * ePHPm's middleware ABI (crates/ephpm-middleware/src/abi.rs) requires:
 *
 *   int32_t     ephpm_middleware_init(uint32_t, const char*, const ephpm_host_v1*);
 *   int32_t     ephpm_middleware_invoke(const ephpm_request_t*, ephpm_response_t*);
 *   void        ephpm_middleware_shutdown(void);
 *   const char* ephpm_middleware_describe(void);
 *
 * elephc's `--emit cdylib` v1 can only export functions whose parameters are
 * int / float / bool / string. Struct pointers are rejected at compile time:
 *
 *   error: exported function 'ephpm_middleware_init' parameter #3 has
 *   unsupported type for --emit cdylib v1; supported: int, float, bool, string
 *
 * So the PHP side cannot speak ePHPm's ABI directly. This shim does:
 *   - it exports the four symbols ePHPm's loader dlsym()s;
 *   - it dereferences the host table and the opaque request/response structs;
 *   - it flattens the request into plain strings and calls the elephc export;
 *   - it collects header writes the PHP made via `extern` calls back into here.
 *
 * The shim is the artifact ePHPm loads. The elephc cdylib is a DT_NEEDED
 * dependency of it, so dlsym() on the shim's handle also reaches it and the
 * PHP side's undefined mw_* symbols bind against this file.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* ---- ePHPm ABI v1 (mirrors crates/ephpm-middleware/src/abi.rs) ----------
 *
 * MINOR LEVEL: this mirror is deliberately the **minor 0** shape.
 *
 * The host's table has grown since — minor 1 appended the response-phase
 * accessors, minor 2 the scheme / is_secure / normalized-host accessors, and
 * minor 3 the five process-global KV slots. Growth within major 1 is purely
 * additive and the table is passed by pointer, so a shorter mirror stays
 * correct: every field declared below is at the same offset it has ever been
 * at, and the shim simply cannot see the ones it does not declare. Do NOT add
 * a trailing field here without adding every field before it, in order.
 *
 * The version check is on the MAJOR byte only, which is the whole
 * compatibility contract; the minor is an additive capability level. If this
 * shim ever wants a newer slot, it must first check
 * `(host->abi_version & 0x00FFFFFF) >= <the minor that added it>`.
 *
 * MINOR 3 REDEFINED TWO SLOTS DECLARED BELOW (ephpm#390 / ephpm#376). This
 * shim calls neither, so nothing here changes — but anyone extending it must
 * know:
 *
 *   - `request_vhost_id` returns the router's CANONICAL SITE KEY (the vhost
 *     directory name), not the raw `Host` header, and **returns NULL** for a
 *     request that matched no virtual host. NULL is a normal return, not an
 *     error: it means "no tenant", which is exactly what a gate needs in order
 *     to fail closed rather than key policy on a client-supplied string. The
 *     shim flattens request fields into C strings for the PHP side, so
 *     forwarding this one requires a NULL check first — `strlen(NULL)` is a
 *     segfault, and it is reachable from any request with an unknown `Host`.
 *
 *   - the `kv_*` slots resolve THE SERVING VHOST'S keyspace on a multi-tenant
 *     node, not the process-global store. That makes per-tenant state the
 *     default; node-wide state now needs the appended `kv_*_global` slots,
 *     which this minor-0 mirror does not declare.
 */

#define EPHPM_ABI_V1 0x01000000u

#define ACTION_CONTINUE 0
#define ACTION_RESPOND  1
#define ACTION_REWRITE  2

#define LOG_ERROR 1
#define LOG_WARN  2
#define LOG_INFO  3
#define LOG_DEBUG 4

typedef struct ephpm_request_t ephpm_request_t;

typedef struct {
    const char *name;
    const char *value;
} ephpm_header_kv;

typedef struct {
    int32_t         action;
    uint16_t        status;
    const uint8_t  *body;
    size_t          body_len;
    const char     *rewrite_path;
    const ephpm_header_kv *header_overrides;
    size_t          header_overrides_len;
    const ephpm_header_kv *response_headers;
    size_t          response_headers_len;
} ephpm_response_t;

typedef struct {
    uint32_t abi_version;

    const char *(*request_method)(const ephpm_request_t *);
    const char *(*request_path)(const ephpm_request_t *);
    const char *(*request_query)(const ephpm_request_t *);
    const char *(*request_remote_ip)(const ephpm_request_t *);
    const char *(*request_header)(const ephpm_request_t *, const char *);
    size_t      (*request_body)(const ephpm_request_t *, const uint8_t **);
    /* Canonical site key since minor 3, and MAY BE NULL — see the header
     * comment. Unused by this shim. */
    const char *(*request_vhost_id)(const ephpm_request_t *);

    /* Per-serving-vhost keyspace since minor 3, not the process-global store.
     * Unused by this shim. */
    int32_t (*kv_get)(const uint8_t *, size_t, uint8_t **, size_t *);
    int32_t (*kv_set)(const uint8_t *, size_t, const uint8_t *, size_t, int64_t);
    int32_t (*kv_set_nx)(const uint8_t *, size_t, const uint8_t *, size_t, int64_t);
    int32_t (*kv_incr)(const uint8_t *, size_t, int64_t, int64_t *);
    void    (*kv_free)(uint8_t *, size_t);

    void (*log)(int32_t, const uint8_t *, size_t);

    int32_t (*kv_incr_ttl)(const uint8_t *, size_t, int64_t, int64_t, int64_t *);
} ephpm_host_v1;

/* ---- elephc-side symbols ------------------------------------------------ */
/* Provided by libmw_php.so (elephc --emit cdylib). elephc marshals a PHP
 * `string` parameter as two C arguments: (const char *ptr, size_t len). */

extern int32_t elephc_init(void);
extern void    elephc_shutdown(void);
extern int64_t mw_invoke(const char *method, size_t method_len,
                         const char *path,   size_t path_len);

/* ---- shim state --------------------------------------------------------- */

static const ephpm_host_v1 *g_host = NULL;

/* elephc's cdylib runtime documents "single-threaded hosts only: the runtime
 * has no locking around its heap". ePHPm invokes middleware concurrently
 * (the Middleware trait is Send + Sync), so every call into the elephc
 * runtime is serialized here. This is a correctness requirement, and it is
 * also the throughput ceiling of this whole approach. */
static pthread_mutex_t g_elephc_lock = PTHREAD_MUTEX_INITIALIZER;

/* Header scratch, filled by mw_set_header() during one invoke. Guarded by
 * g_elephc_lock (only touched between lock/unlock in invoke). */
#define MAX_HDRS 16
#define HDR_BUF  256
static char  g_hdr_name[MAX_HDRS][HDR_BUF];
static char  g_hdr_val[MAX_HDRS][HDR_BUF];
static ephpm_header_kv g_hdrs[MAX_HDRS];
static size_t g_hdr_count = 0;

/* ---- callbacks the PHP calls via `extern` ------------------------------- */

void mw_set_header(const char *name, const char *value) {
    if (g_hdr_count >= MAX_HDRS || !name || !value) return;
    snprintf(g_hdr_name[g_hdr_count], HDR_BUF, "%s", name);
    snprintf(g_hdr_val[g_hdr_count],  HDR_BUF, "%s", value);
    g_hdrs[g_hdr_count].name  = g_hdr_name[g_hdr_count];
    g_hdrs[g_hdr_count].value = g_hdr_val[g_hdr_count];
    g_hdr_count++;
}

void mw_log(const char *msg) {
    if (!g_host || !msg) return;
    g_host->log(LOG_INFO, (const uint8_t *)msg, strlen(msg));
}

/* ---- the four symbols ePHPm's loader looks up --------------------------- */

int32_t ephpm_middleware_init(uint32_t abi_version,
                              const char *config_json,
                              const ephpm_host_v1 *host) {
    (void)config_json;
    /* Refuse if the host's major is newer than we were built for. The MINOR is
     * deliberately not checked: it is an additive capability level, this shim
     * uses only minor-0 slots, and refusing a newer minor would mean refusing
     * to load on every host from here on. */
    if ((abi_version >> 24) != (EPHPM_ABI_V1 >> 24)) return -1;
    if (!host) return -1;
    g_host = host;
    return elephc_init();   /* boot the elephc runtime/heap */
}

int32_t ephpm_middleware_invoke(const ephpm_request_t *req,
                                ephpm_response_t *out) {
    if (!g_host || !req || !out) return -1;

    const char *method = g_host->request_method(req);
    const char *path   = g_host->request_path(req);
    if (!method) method = "";
    if (!path)   path   = "";

    pthread_mutex_lock(&g_elephc_lock);
    g_hdr_count = 0;

    int64_t action = mw_invoke(method, strlen(method), path, strlen(path));

    out->action = (int32_t)action;
    if (g_hdr_count > 0) {
        out->response_headers     = g_hdrs;
        out->response_headers_len = g_hdr_count;
    }
    /* NOTE: g_hdrs stays valid until the next invoke; the host copies
     * everything before this function's caller unwinds, and the lock is not
     * released until after we have published the pointers. */
    pthread_mutex_unlock(&g_elephc_lock);

    return 0;
}

void ephpm_middleware_shutdown(void) {
    elephc_shutdown();
}

const char *ephpm_middleware_describe(void) {
    return "elephc-php-middleware (experimental adapter)";
}
