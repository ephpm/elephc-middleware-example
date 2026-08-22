/*
 * test_host.c — minimal stand-in for ePHPm's middleware loader.
 *
 * Mirrors crates/ephpm-middleware: dlopen the module, dlsym the four ABI
 * symbols, hand it a host table at init, then invoke it with a request and
 * print the verdict + any response headers it produced.
 *
 * Build: gcc -o test_host test_host.c -ldl
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define EPHPM_ABI_V1 0x01000000u

typedef struct ephpm_request_t ephpm_request_t;

typedef struct { const char *name; const char *value; } ephpm_header_kv;

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
    const char *(*request_vhost_id)(const ephpm_request_t *);
    int32_t (*kv_get)(const uint8_t *, size_t, uint8_t **, size_t *);
    int32_t (*kv_set)(const uint8_t *, size_t, const uint8_t *, size_t, int64_t);
    int32_t (*kv_set_nx)(const uint8_t *, size_t, const uint8_t *, size_t, int64_t);
    int32_t (*kv_incr)(const uint8_t *, size_t, int64_t, int64_t *);
    void    (*kv_free)(uint8_t *, size_t);
    void    (*log)(int32_t, const uint8_t *, size_t);
    int32_t (*kv_incr_ttl)(const uint8_t *, size_t, int64_t, int64_t, int64_t *);
} ephpm_host_v1;

/* --- fake request ------------------------------------------------------- */
struct ephpm_request_t { const char *method; const char *path; };

static const char *r_method(const ephpm_request_t *r) { return r->method; }
static const char *r_path(const ephpm_request_t *r)   { return r->path; }
static const char *r_query(const ephpm_request_t *r)  { (void)r; return ""; }
static const char *r_ip(const ephpm_request_t *r)     { (void)r; return "127.0.0.1"; }
static const char *r_hdr(const ephpm_request_t *r, const char *n) { (void)r; (void)n; return NULL; }
static size_t      r_body(const ephpm_request_t *r, const uint8_t **o) { (void)r; *o = NULL; return 0; }
static const char *r_vhost(const ephpm_request_t *r)  { (void)r; return "localhost"; }

static int32_t kv_get(const uint8_t *k, size_t kl, uint8_t **o, size_t *ol) {
    (void)k;(void)kl;(void)o;(void)ol; return 1; }
static int32_t kv_set(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, int64_t t) {
    (void)k;(void)kl;(void)v;(void)vl;(void)t; return 0; }
static int32_t kv_set_nx(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, int64_t t) {
    (void)k;(void)kl;(void)v;(void)vl;(void)t; return 0; }
static int32_t kv_incr(const uint8_t *k, size_t kl, int64_t by, int64_t *o) {
    (void)k;(void)kl;(void)by; *o = 1; return 0; }
static void    kv_free(uint8_t *p, size_t l) { (void)p;(void)l; }
static int32_t kv_incr_ttl(const uint8_t *k, size_t kl, int64_t by, int64_t t, int64_t *o) {
    (void)k;(void)kl;(void)by;(void)t; *o = 1; return 0; }

static void host_log(int32_t lvl, const uint8_t *m, size_t l) {
    printf("  [host log lvl=%d] %.*s\n", lvl, (int)l, (const char *)m);
}

int main(int argc, char **argv) {
    const char *lib = (argc > 1) ? argv[1] : "./libelephc_mw.so";
    const char *path = (argc > 2) ? argv[2] : "/api/users";

    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
    printf("OK   dlopen %s\n", lib);

    int32_t     (*init)(uint32_t, const char *, const ephpm_host_v1 *) = dlsym(h, "ephpm_middleware_init");
    int32_t     (*invoke)(const ephpm_request_t *, ephpm_response_t *) = dlsym(h, "ephpm_middleware_invoke");
    void        (*shutdown_fn)(void)  = dlsym(h, "ephpm_middleware_shutdown");
    const char *(*describe)(void)     = dlsym(h, "ephpm_middleware_describe");

    printf("OK   dlsym init=%p invoke=%p shutdown=%p describe=%p\n",
           (void*)init, (void*)invoke, (void*)shutdown_fn, (void*)describe);
    if (!init || !invoke || !shutdown_fn) { printf("FAIL missing ABI symbol\n"); return 1; }
    if (describe) printf("OK   describe: %s\n", describe());

    static ephpm_host_v1 host;
    host.abi_version = EPHPM_ABI_V1;
    host.request_method = r_method; host.request_path = r_path;
    host.request_query = r_query;   host.request_remote_ip = r_ip;
    host.request_header = r_hdr;    host.request_body = r_body;
    host.request_vhost_id = r_vhost;
    host.kv_get = kv_get; host.kv_set = kv_set; host.kv_set_nx = kv_set_nx;
    host.kv_incr = kv_incr; host.kv_free = kv_free;
    host.log = host_log; host.kv_incr_ttl = kv_incr_ttl;

    int32_t rc = init(EPHPM_ABI_V1, "{}", &host);
    printf("%s init -> %d\n", rc == 0 ? "OK  " : "FAIL", rc);
    if (rc != 0) return 1;

    struct ephpm_request_t req = { "GET", path };
    ephpm_response_t resp;
    memset(&resp, 0, sizeof(resp));

    rc = invoke(&req, &resp);
    printf("%s invoke(GET %s) -> rc=%d action=%d\n",
           rc == 0 ? "OK  " : "FAIL", path, rc, resp.action);

    printf("     response_headers (%zu):\n", resp.response_headers_len);
    for (size_t i = 0; i < resp.response_headers_len; i++)
        printf("       %s: %s\n", resp.response_headers[i].name,
                                  resp.response_headers[i].value);

    shutdown_fn();
    printf("OK   shutdown\n");
    return 0;
}
