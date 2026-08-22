<?php
/**
 * Third Composer test: firebase/php-jwt, the canonical middleware library.
 * Sources required directly (Composer's ClassLoader does not compile).
 */

require __DIR__ . '/../vendor/firebase/php-jwt/src/JWT.php';
require __DIR__ . '/../vendor/firebase/php-jwt/src/Key.php';

extern function mw_set_header(string $name, string $value): void;
extern function mw_log(string $msg): void;

#[Export]
function mw_invoke(string $method, string $path): int {
    mw_log("elephc-mw(jwt): " . $method . " " . $path);
    mw_set_header("X-Elephc", "1");
    return 0;
}
