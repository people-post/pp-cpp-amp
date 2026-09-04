# pp-cpp-amp

Association mesh protocol stack (ADP L1 wire, MSH L2 session, L3 channel mux, link layer).

Depends only on [pp-cpp-common](https://github.com/people-post/pp-cpp-common) and [pp-cpp-crypto](https://github.com/people-post/pp-cpp-crypto).

## Layout

```
include/amp/          Public headers (L1/, L2/, L3/, link/)
src/L1/               L1 implementation + unit tests
src/L2/               L2 implementation + unit tests
src/L3/               L3 implementation + unit tests
src/link/             Link layer + unit tests
tests/support/        Tier B test harness (PeerId stub)
tests/integration/    Cross-layer integration tests
```

Consumers include headers as `#include "amp/L1/Clock.h"` (include root is `include/`).

NAT / association keepalive: [docs/KEEPALIVE.md](docs/KEEPALIVE.md).

Performance case matrix + scaffold: [docs/PERF_CASES.md](docs/PERF_CASES.md) (`pp_amp_perf`).

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DPP_AMP_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Sibling checkouts (`../pp-cpp-common`, `../pp-cpp-crypto`) are used automatically when present. Override with `-DPP_CPP_COMMON_SOURCE_DIR=` / `-DPP_CPP_CRYPTO_SOURCE_DIR=`.

## CMake targets

| Target | Layer |
|--------|-------|
| `pp_amp_l1` | L1 ADP wire |
| `pp_amp_l2` | L2 MSH session |
| `pp_amp_l3` | L3 channel mux |
| `pp_amp_link` | Link / mesh runtime |

## Tests

126 tests across five ctest targets (`pp_amp_l1_test` … `pp_amp_integration_test`). Unit tests are co-located per layer; integration tests live under `tests/integration/`.

Performance scaffold (A–F matrix): `pp_amp_perf` under `tests/perf/` — see [docs/PERF_CASES.md](docs/PERF_CASES.md).
