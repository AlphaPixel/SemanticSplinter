# Dependencies

Third-party code comes only through vcpkg ports, at the versions fixed by `builtin-baseline` in `vcpkg.json`
(D-52). Bump the baseline deliberately and update this table in the same commit. Default dependencies must be
permissive (MIT, BSD, Apache-2.0, Zlib, public domain); copyleft libraries are opt-in features (D-19).

**Baseline:** vcpkg `b08975f4fdf01ad9a5c08db8f1953a57d11e8b26` (2026-09-25).

| Port | Version | License | vcpkg feature | Used for | Why this one |
|---|---|---|---|---|---|
| catch2 | 3.16.0 | BSL-1.0 | `tests` | Unit and property tests | D-9: CTest discovery, readable property-style tests |
