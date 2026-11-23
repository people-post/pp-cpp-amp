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

## Docs

| Doc | What |
|-----|------|
| [docs/STACK.md](docs/STACK.md) | **Start here.** The big picture and layer map: layers, objects, failure propagation, QoS, security, channel 0, circuits, addressing; the product entry (`MeshRuntime`) and the lifetimes product code can rely on |
| [docs/contracts/ADP.md](docs/contracts/ADP.md) | L1 wire (normative): packets, version negotiation, addresses |
| [docs/contracts/AMP-SESSION.md](docs/contracts/AMP-SESSION.md) | L2 session (normative): MSH handshake, keys, sealing packets, rekey |
| [docs/contracts/AMP-CHANNEL.md](docs/contracts/AMP-CHANNEL.md) | L3 channels (normative): frames, lifecycle, channel 0, fragmentation, policies, the nested Session carrier, ownership |
| [docs/DESIGN.md](docs/DESIGN.md) | L1 design: threat model, API, I/O (and the retired wire v1) |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Why: the stack's decisions (A001–A029, the transport's; ids from pp-browser, unchanged) |
| [docs/testing/](docs/testing/TEST_MATRIX.md) | Test matrices: [Tier B](docs/testing/TEST_MATRIX.md) across layers; Tier A per layer, [L1](docs/testing/L1_TEST_MATRIX.md), [L2](docs/testing/L2_TEST_MATRIX.md), [L3](docs/testing/L3_TEST_MATRIX.md), [link](docs/testing/LINK_TEST_MATRIX.md) |
| [docs/FAULT_CASES.md](docs/FAULT_CASES.md) | Fault / bad-network correctness matrix (N1–N16) |
| [docs/PERF_CASES.md](docs/PERF_CASES.md) | Performance case matrix and scaffold (`pp_amp_perf`) |
| [docs/STATUS.md](docs/STATUS.md) | What is left to do |
| [docs/OWNERSHIP.md](docs/OWNERSHIP.md) | Ownership boundary: what Amp shares, what is the product's L4 |
| [docs/ADR_LINK_PLANE.md](docs/ADR_LINK_PLANE.md) | Link plane ADR |
| [docs/ADR_STREAM_TRANSPORT.md](docs/ADR_STREAM_TRANSPORT.md) | Stream transport for networks without UDP (TCP / TLS / WebSocket, Bluetooth; proposed, not built) |
| [docs/KEEPALIVE.md](docs/KEEPALIVE.md) | NAT / association keepalive |
| [docs/TUNING.md](docs/TUNING.md) | Tunable transport policy (`PeerLinkConfig::adp` / `::mux`) |

The stack's design history, contracts and test matrices moved here from pp-browser (its `projects/adp/` and `docs/contracts/`, [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). What stays there is the app's: its adoption of Amp (`projects/adp/`, decisions A006, A008–A011, A017, A020–A023, A028), the protocols it and pp-cpp-mesh run on channels ([AMP-CHANNEL](https://github.com/people-post/pp-browser/blob/main/docs/contracts/AMP-CHANNEL.md), [L4_PROTOCOL_KINDS](https://github.com/people-post/pp-browser/blob/main/docs/contracts/L4_PROTOCOL_KINDS.md)), and the link error table [AMP-LINK-ERRORS](https://github.com/people-post/pp-browser/blob/main/docs/contracts/AMP-LINK-ERRORS.md), which pp-cpp-mesh mirrors.

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

155 tests across five ctest targets (`pp_amp_l1_test` … `pp_amp_integration_test`). Unit tests are co-located per layer; integration tests live under `tests/integration/`.

Performance harness (A–F matrix + C5/D3/E4/OsUdpAmp): `pp_amp_perf` under `tests/perf/` — see [docs/PERF_CASES.md](docs/PERF_CASES.md).

Bad-network / adversarial correctness IDs: [docs/FAULT_CASES.md](docs/FAULT_CASES.md).

## Formatting

Every first-party C/C++ source is formatted with [`.clang-format`](.clang-format) by exactly clang-format 23.1.5 (`pip install clang-format==23.1.5`; other versions format differently). Run `./scripts/check_format.sh --fix` before committing; CI fails on an unformatted file. Vendored code is left as it came.
