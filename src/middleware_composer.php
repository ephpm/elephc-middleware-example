<?php
/**
 * Same trivial middleware, but the routing decision comes from a real
 * Composer package (nikic/fast-route) instead of hand-rolled substr().
 *
 * This is the "do my existing PHP libraries survive compilation?" test.
 */

require __DIR__ . '/../vendor/autoload.php';

extern function mw_set_header(string $name, string $value): void;
extern function mw_log(string $msg): void;

#[Export]
function mw_invoke(string $method, string $path): int {
    mw_log("elephc-mw(fastroute): " . $method . " " . $path);

    $dispatcher = FastRoute\simpleDispatcher(function (FastRoute\RouteCollector $r) {
        $r->addRoute('GET', '/api/users', 'users');
        $r->addRoute('GET', '/api/users/{id:\d+}', 'user_show');
        $r->addRoute('GET', '/', 'home');
    });

    $info = $dispatcher->dispatch($method, $path);

    mw_set_header("X-Elephc", "1");
    if ($info[0] === FastRoute\Dispatcher::FOUND) {
        mw_set_header("X-Route", $info[1]);
    } else {
        mw_set_header("X-Route", "no-match");
    }

    return 0; // ACTION_CONTINUE
}
