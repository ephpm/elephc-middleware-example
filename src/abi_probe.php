<?php
// Probe: can elephc express ePHPm's middleware C ABI directly?
//
// ePHPm requires (crates/ephpm-middleware/src/abi.rs):
//   int32_t ephpm_middleware_init(uint32_t, const char*, const ephpm_host_v1*);
//   int32_t ephpm_middleware_invoke(const ephpm_request_t*, ephpm_response_t*);
//   void    ephpm_middleware_shutdown(void);
//   const char* ephpm_middleware_describe(void);

#[Export]
function ephpm_middleware_init(int $abi_version, string $config_json, ptr $host): int {
    return 0;
}

#[Export]
function ephpm_middleware_invoke(ptr $request, ptr $response_out): int {
    return 0;
}

#[Export]
function ephpm_middleware_shutdown(): void {
}
