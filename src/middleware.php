<?php
/**
 * Trivial ePHPm middleware, written in PHP, compiled by elephc to a cdylib.
 *
 * The logic is deliberately dumb: log the request and inject two response
 * headers. The point is the pipeline, not the policy.
 *
 * These two functions are provided by the C shim (ephpm_elephc_shim.c) and
 * left as UNDEFINED symbols in this cdylib; they bind at dlopen time.
 */

extern function mw_set_header(string $name, string $value): void;
extern function mw_log(string $msg): void;

/**
 * Called once per request by the shim.
 * Return value is an ePHPm action code: 0 = CONTINUE, 1 = RESPOND, 2 = REWRITE.
 */
#[Export]
function mw_invoke(string $method, string $path): int {
    mw_log("elephc-mw: " . $method . " " . $path);

    mw_set_header("X-Elephc", "1");
    mw_set_header("X-Route", classify($path));

    return 0; // ACTION_CONTINUE — headers ride along on PHP's response
}

/** Not exported: ordinary PHP, inlined/compiled like any other function. */
function classify(string $path): string {
    if (strlen($path) >= 5 && substr($path, 0, 5) === "/api/") {
        return "api";
    }
    return "web";
}
