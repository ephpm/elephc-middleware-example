<?php
/**
 * Second Composer test: skip Composer's generated ClassLoader (which does not
 * compile) and require the library's own sources directly, to find out
 * whether the *library* is elephc-compatible independently of the autoloader.
 */

require __DIR__ . '/../vendor/nikic/fast-route/src/bootstrap.php';

extern function mw_set_header(string $name, string $value): void;
extern function mw_log(string $msg): void;

#[Export]
function mw_invoke(string $method, string $path): int {
    mw_log("elephc-mw(fastroute-direct): " . $method . " " . $path);

    $dispatcher = FastRoute\simpleDispatcher(function (FastRoute\RouteCollector $r) {
        $r->addRoute('GET', '/api/users', 'users');
        $r->addRoute('GET', '/', 'home');
    });

    $info = $dispatcher->dispatch($method, $path);

    mw_set_header("X-Elephc", "1");
    mw_set_header("X-Route", $info[0] === FastRoute\Dispatcher::FOUND ? $info[1] : "no-match");

    return 0;
}
