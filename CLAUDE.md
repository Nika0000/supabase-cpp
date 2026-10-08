# supabase-cpp

C++20 client for Supabase (PostgREST/RPC, Auth, Edge Functions, Realtime; Storage and GraphQL are stubs). libcurl handles TLS; nlohmann::json handles data. API mirrors supabase-js.

## Rules

- Never build or run to "test" a change unless asked. Verify by reading code.
- Portable: MinGW GCC, Apple clang, NDK, devkitA64, vitasdk. C++20 language only; no `<format>`, `<ranges>`, `std::jthread`, `<expected>` (use `supabase::Result<T>`).
- No exceptions in the public API; every fallible call returns `Result<T>` with `Error`.
- No OpenSSL calls in library code. TLS goes through curl; CA bundle is injected via `CurlOptions::caBundle`.
- No platform `#if` in shared code. Put it in a dedicated file.
- API parity: names follow supabase-js (`from().select().eq()`, `auth().signInWithPassword()`, `rpc()`, `functions().invoke()`). C++ casing: methods camelCase, types PascalCase, `snake_case` only for wire/JSON field names.
- Public headers are self-contained, live in `include/supabase/`, and carry brief doxygen. Implementation in `src/`. No `using namespace` in headers.
- Style: `.clang-format` (WebKit, Allman). Format only touched files.
- Commits: conventional (`feat:`, `fix:`, `style:`...). No pushes without asking.
- Never touch the host app (`app/`) from this repo.

## Token budget

- When `graphify-out/graph.json` exists, use `graphify query`/`explain` first, then `rtk grep -n` and read ~40-line ranges. Don't re-read a file you just edited.
- Don't read `build*/`, `vcpkg_installed/`, `CHANGELOG.md`.
- Reply in 1-2 lines. No diff recaps.
- Do simple 1-3 file work directly; spawn agents (sonnet) only for multi-area work.

## Graphify

- Run `graphify update .` after source changes to refresh the local code graph.
- Use `graphify path "<A>" "<B>"` to inspect relationships. `graphify-out/` is generated and ignored by Git.

## Layout

`include/supabase/{client,config,error,result,http}.hpp` core; `postgrest/ auth/ functions/ realtime/` implemented; `storage/ graphql/` return `NotImplemented`. `src/` mirrors it. `tests/` uses a mock `http::Transport` (no network). `README.md` covers usage and project status.

## Agents / skills

Agents: `api-designer` (read-only parity), `http-core-engineer`, `postgrest-engineer`, `auth-engineer`, `test-engineer`, `reviewer`.
Skills: `/add-endpoint`, `/parity-check`, `/commit`.
