# ePHPm native middleware, written in PHP, compiled with elephc

**Status: this works. It was built and run end to end, not designed on paper.**

A trivial middleware is written in ordinary PHP, compiled to native code by
[elephc](https://elephc.dev/), loaded by a stock ePHPm release binary via
`dlopen`, and observed injecting headers into a live HTTP response — with PHP
never being interpreted for the middleware itself.

Everything below marked **Verified** was executed on 2026-08-20 against
**elephc 0.26.4** and the published **ephpm v0.7.3+php8.5.7-linux-x86_64**
release binary, on Ubuntu 26.04 (WSL2, x86_64). Everything marked
**Not verified** or **Planned** was not.

---

## The result first

```
$ curl -sS -D- -o /dev/null http://127.0.0.1:8299/api/users.php
HTTP/1.1 200 OK
x-elephc: 1
x-route: api

$ curl -sS -D- -o /dev/null http://127.0.0.1:8299/index.php
HTTP/1.1 200 OK
x-elephc: 1
x-route: web
```

Server log — the middleware writing into ePHPm's own `tracing` subscriber:

```
INFO ephpm_server::middleware: middleware initialised
     module=/root/mwx/libelephc_mw.so
     describe=elephc-php-middleware (experimental adapter)
INFO ephpm_server: middleware chain loaded count=1
INFO ephpm_middleware: elephc-mw: GET /api/users.php
INFO ephpm_middleware: elephc-mw: GET /index.php
```

`x-route` is `api` vs `web` because this PHP function ran as native code,
before PHP dispatch:

```php
function classify(string $path): string {
    if (strlen($path) >= 5 && substr($path, 0, 5) === "/api/") {
        return "api";
    }
    return "web";
}
```

**Verified.**

---

## Why it works

### ePHPm's middleware lane is a C ABI, not a PHP hook

ePHPm runs middleware **before PHP dispatch and before any request-body bytes
are read** — a rejected request never boots PHP and never pays for the body
transfer. The lane is a small versioned C ABI
(`crates/ephpm-middleware/src/abi.rs`); a module is any shared library
(`.so`/`.dylib`/`.dll`) exporting four symbols:

```c
int32_t     ephpm_middleware_init(uint32_t abi_version,
                                  const char *config_json,
                                  const ephpm_host_v1 *host);
int32_t     ephpm_middleware_invoke(const ephpm_request_t *req,
                                    ephpm_response_t *out);
void        ephpm_middleware_shutdown(void);
const char *ephpm_middleware_describe(void);   /* optional */
```

The host hands the module a **callback table** at `init` (rather than the
module `dlsym`-ing host symbols, which would need `-rdynamic` on Linux and has
no clean Windows analogue). That table carries the request accessors, the
`tracing` logger, and the cluster-replicated KV store — which is why a
cluster-wide rate limiter is one `kv_incr_ttl` call.

Because the contract is *just C*, the language that produced the `.so` is
irrelevant to ePHPm. That is the opening elephc walks through.

#### ABI version: this shim targets major 1, minor 0

The **major byte** gates compatibility and `ephpm_middleware_init` refuses a
host whose major it was not built for. The lower three bytes are an additive
**minor** level, and the table is passed by pointer, so a module that declares a
*shorter* mirror of the host table stays correct on a newer host — it simply
cannot see the slots it did not declare. That is what this shim does, and why
it has needed no change as the ABI has grown to minor 3.

If you extend the shim, two things from **minor 3**
([ephpm#448](https://github.com/ephpm/ephpm/pull/448)) matter, because they
redefined slots this shim already declares rather than only appending new ones:

- **`request_vhost_id` can return NULL, and is now the canonical site key.** It
  is the vhost *directory name* the router resolved — not the `Host` header,
  and not the header even normalised — and it is NULL when the request matched
  no virtual host (which on a single-site node is every request). Since this
  shim's job is to flatten request fields into C strings for the PHP side,
  forwarding it means NULL-checking it first: `strlen(NULL)` is a segfault
  reachable by sending an unknown `Host`. Use NULL as "no tenant" — that is the
  point of it ([ephpm#390](https://github.com/ephpm/ephpm/issues/390)).
- **`kv_get`/`kv_set`/`kv_incr`/`kv_incr_ttl` are per-tenant.** On a
  multi-tenant node they resolve the serving vhost's own keyspace — the same
  store that vhost's PHP reaches through `ephpm_kv_*` — instead of the
  process-global one ([ephpm#376](https://github.com/ephpm/ephpm/issues/376)).
  Node-wide state needs the appended `kv_*_global` slots, which are minor 3 and
  which this shim's minor-0 mirror does not declare; adding them means adding
  *every* preceding minor-1/2/3 field first, in `abi.rs` order.

Neither changes anything on a single-site node, and neither affects this
example as it stands — it calls only `request_method`, `request_path` and
`log`.

### elephc compiles PHP to a native cdylib

elephc is a real, shipping, MIT-licensed PHP-to-native AOT compiler
([github.com/illegalstudio/elephc](https://github.com/illegalstudio/elephc)).
It parses PHP, type-checks it, lowers it through its own SSA IR, and emits
assembly — no Zend Engine, no VM, no opcode fallback. `--emit cdylib` is a
real flag in the shipping compiler, not a roadmap item:

```
$ elephc --help
Output modes:
  --emit KIND    Output kind: executable (default) | cdylib
```

Functions marked `#[Export]` become C-callable exports:

```
$ nm -D --defined-only libmw_php.so | grep ' T '
T elephc_free
T elephc_init
T elephc_last_error
T elephc_shutdown
T mw_invoke          <-- our #[Export] function
```

**Verified.**

### The catch: elephc cannot express ePHPm's ABI directly

This is the part that matters, and it is why this repo contains a C file.

elephc's cdylib v1 marshaller only accepts `int`, `float`, `bool`, and
`string` in exported signatures. ePHPm's ABI is built on **struct pointers**
(the host table, the opaque request, the response out-param). Trying to
export ePHPm's entry points straight from PHP fails at compile time:

```
$ elephc --emit cdylib abi_probe.php
error[abi_probe.php:11:1]: exported function 'ephpm_middleware_init'
  parameter #3 has unsupported type for --emit cdylib v1;
  supported: int, float, bool, string
```

**Verified.** So an adapter is **mandatory**, not stylistic. There is no
version of "just point ePHPm at the elephc output" that works today.

### The adapter

`shim/ephpm_elephc_shim.c` (~180 lines) is what ePHPm actually loads. It:

1. **exports the four ABI symbols** ePHPm's loader `dlsym`s;
2. **dereferences** the host table and opaque request struct — the pointer
   work PHP cannot do;
3. **flattens** the request to plain strings and calls elephc's `mw_invoke`;
4. **provides the callbacks** the PHP side calls (`mw_set_header`, `mw_log`),
   translating them onto the host table;
5. **serializes** every entry into the elephc runtime behind a mutex.

The two halves link like this:

```
ePHPm  --dlopen-->  libelephc_mw.so (C shim, exports ephpm_middleware_*)
                          |
                          +--NEEDED--> libmw_php.so (elephc output, exports mw_invoke)
```

The PHP side's `extern` declarations are left as **undefined symbols** in the
elephc cdylib and bind to the shim at load time:

```
$ nm -D libmw_php.so | grep -E ' T | U '
T mw_invoke
U mw_log            <-- resolved from the shim at dlopen
U mw_set_header
```

**Verified** — the resolution works under `dlopen(..., RTLD_NOW | RTLD_LOCAL)`,
which is how ePHPm loads modules.

Note `-Wl,-rpath,'$ORIGIN'` in `build.sh`: without it the shim cannot find
`libmw_php.so` at load time and ePHPm fails startup with
`cannot open shared object file`. **Verified** (hit and fixed during this run.)

---

## Build and run

```bash
./build.sh                              # needs elephc + gcc
./test_host ./libelephc_mw.so /api/users   # standalone ABI harness
ephpm serve -c ephpm.toml               # real server
```

Mount:

```toml
[[middleware]]
library = "/path/to/libelephc_mw.so"
order   = 10
config  = {}
```

`test_host.c` is a ~150-line stand-in for ePHPm's loader (same dlopen/dlsym
sequence, same host table). Useful because it isolates ABI bugs from server
bugs. Its output on a working build:

```
OK   dlopen ./libelephc_mw.so
OK   dlsym init/invoke/shutdown/describe
OK   describe: elephc-php-middleware (experimental adapter)
OK   init -> 0
  [host log lvl=3] elephc-mw: GET /api/users
OK   invoke(GET /api/users) -> rc=0 action=0
     response_headers (2):
       X-Elephc: 1
       X-Route: api
OK   shutdown
```

**Verified.**

---

## Do your existing Composer libraries work?

**No — not today, and this was tested rather than assumed.**

This is the claim most worth being careful about. Three real packages were
installed with Composer and compiled. All three failed, each for a different
ordinary, modern PHP idiom.

| Package | Result | Root cause |
|---|---|---|
| Composer's own `vendor/autoload.php` | **fails** | `while (false !== $lastPos = strrpos(...))` — assignment inside a condition → `Invalid assignment target`; also typed static properties |
| `nikic/fast-route` | **fails** | `new $options['routeCollector']` → `Dynamic class-name expressions after 'new' are not supported` |
| `firebase/php-jwt` | **fails** | `(object) $payload` cast; `#[\SensitiveParameter] $key` parameter attribute |

**Verified** (all three, exact compiler output captured).

Note the first row is the important one: **Composer's generated ClassLoader
itself does not compile**, so `require 'vendor/autoload.php'` — the entry point
of essentially every modern PHP project — is a hard stop. You can bypass it by
`require`-ing library sources directly, which is what rows 2 and 3 did, and
they still failed on their own code.

elephc's docs do advertise Composer metadata integration and static autoload
insertion, and its stdlib coverage is genuinely broad (500+ builtins, PDO,
PCRE, classes, traits, enums, generators, fibers). The gap here is **parser and
feature coverage on real-world library code**, not ambition. elephc's own
README is candid: *"Not everything PHP supports is implemented, and you will
find bugs"*, *"not a drop-in replacement for an entire dynamic framework
today"*.

**What actually works today** is straightforward first-party PHP: functions,
classes, string/array/math builtins, control flow, regex. That is a real and
useful subset for middleware — auth checks, path classification, header
manipulation, feature flags — but it is **your code**, not your dependency
tree. Budget for writing the middleware against elephc's subset rather than
lifting an existing vendor-backed class into it.

The working example in `src/middleware.php` therefore uses no Composer
packages. `src/middleware_composer.php`, `src/middleware_fastroute_direct.php`,
and `src/middleware_jwt.php` are kept as **reproducible failing cases** — they
are the evidence for the table above, and they are the regression tests if
elephc's coverage improves.

---

## Limits you must know before using this

**Verified constraints:**

- **Concurrency is serialized.** elephc's cdylib runtime documents
  *"single-threaded hosts only: the runtime has no locking around its heap."*
  ePHPm invokes middleware concurrently (its `Middleware` trait is
  `Send + Sync`). The shim therefore wraps every call into elephc in a
  `pthread_mutex`. This is required for correctness and is the throughput
  ceiling of the whole approach — one request at a time through the PHP
  middleware. A Rust or C module has no such limit.
  *Not verified:* the behaviour under real concurrent load. Correctness was
  reasoned about and the lock is in place, but no load test was run, and no
  attempt was made to measure the contention cost.

- **Linux and macOS only.** elephc targets `linux-x86_64`, `linux-aarch64`,
  and `macos-aarch64` (`elephc --help`). **There is no Windows target**, so
  this path cannot produce a `.dll` — even though ePHPm's middleware lane
  itself supports Windows via `LoadLibrary`.

- **Request bodies are invisible.** ABI v1's body accessor always returns
  length 0 by design — the chain runs before the body is read. Body-inspecting
  middleware is not possible in v1, in any language.

- **fpm mode covers PHP-dispatched requests only.** Static files and router
  404/403 responses bypass the chain. Worker mode routes everything through
  PHP, so the chain sees all requests.

- **No exception propagation.** elephc's docs state a PHP fatal inside a
  cdylib terminates the process. The shim does not (and cannot portably)
  contain that. A crash in your PHP middleware takes the server down.
  *Not verified* — no fatal was deliberately triggered.

- **One entry source per cdylib.** `require`/`include` work, but you get a
  single compilation root.

- **Strings cannot be returned from elephc exports** (v1 marshals `string`
  only as a parameter, as `const char *, size_t`). That is why the PHP pushes
  header values out through `mw_set_header` instead of returning them.

**Not verified / out of scope for this run:**

- The **KV host callbacks** (`kv_get`/`kv_set`/`kv_incr_ttl`) are wired
  through the shim's host-table struct and the struct layout mirrors
  `abi.rs`, but only `log` was actually exercised end to end. A
  cluster-replicated rate limiter in elephc-PHP is **plausible but untested**.
  Note also that since ABI minor 3 those callbacks are **per serving vhost**,
  so such a limiter would be per-tenant rather than node-wide unless it used
  the `kv_*_global` slots — which this minor-0 mirror does not declare.
- **`request_vhost_id` is not exercised either**, and since minor 3 it can
  return NULL. Anything that starts forwarding it to the PHP side needs a NULL
  check; see the ABI-version note above.
- Long-running stability, memory behaviour under sustained traffic, and
  `elephc_shutdown` semantics on server reload.
- Any `ACTION_RESPOND` (short-circuit) path — only `ACTION_CONTINUE` was
  exercised. Rejecting a request before PHP boots is the headline use case for
  middleware and it is **untested here**.

---

## Is this ready to ship?

Honestly: **not as a supported feature, yes as a real capability.**

It demonstrably works, but the adapter is hand-written per project, the
concurrency story is a global lock, Composer is effectively unavailable, and
Windows is unreachable. ePHPm's own tracking note has long read
*"Native middleware via elephc cdylib — Planned (roadmap), spec only, no
code."* That note is now out of date on the feasibility question: the path is
real and the blocking unknown is answered. What does not exist is **a
first-party adapter**.

If ePHPm wanted to make this a supported lane, the concrete work is:

1. **Ship the shim as a reusable artifact** — a generated or prebuilt
   `ephpm-elephc-adapter` object, so users write only PHP. The shim is
   entirely mechanical; nothing in it is project-specific except the name of
   the elephc export it calls.
2. **Publish a PHP stub package** (`Ephpm\Middleware\*`) declaring the
   `extern` surface (`mw_set_header`, `mw_log`, KV accessors) so authors get
   IDE completion and elephc type-checking against the real host table.
3. **Decide the concurrency policy** — global lock (simple, slow), or N
   pre-initialised elephc instances behind a pool, if elephc's runtime can be
   instantiated more than once per process. That question is open and should
   be asked upstream.
4. **Gate on elephc's parser coverage** before promising anything about
   Composer. The three failures above are good upstream bug reports.

---

## Files

| Path | What it is |
|---|---|
| `src/middleware.php` | The working middleware — trivial PHP, no dependencies |
| `shim/ephpm_elephc_shim.c` | The adapter; exports ePHPm's ABI, calls elephc |
| `shim/test_host.c` | Standalone loader harness mimicking ePHPm |
| `build.sh` | Builds both halves + the harness |
| `ephpm.toml` | Server config mounting the module |
| `www/` | Two trivial PHP pages to route against |
| `src/abi_probe.php` | Proves elephc rejects ePHPm's ABI signatures |
| `src/middleware_composer.php` | Composer autoload failure case |
| `src/middleware_fastroute_direct.php` | `nikic/fast-route` failure case |
| `src/middleware_jwt.php` | `firebase/php-jwt` failure case |

## References

- elephc — <https://elephc.dev/>, <https://github.com/illegalstudio/elephc> (MIT)
- elephc cdylib docs — <https://elephc.dev/docs/beyond-php/cdylib/>
- elephc extern/FFI docs — <https://elephc.dev/docs/beyond-php/extern/>
- ePHPm native middleware guide — `site/content/guides/native-middleware.md`
- ePHPm middleware ABI — `crates/ephpm-middleware/src/abi.rs`
