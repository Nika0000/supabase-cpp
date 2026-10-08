---
name: add-endpoint
description: Scaffold a new API method or module (header, source, mock-transport test) following supabase-js naming. Args are the module and method.
---
1. Spawn/consult `api-designer` mentally: pick the supabase-js name and C++ signature (Result<T>, option structs, no exceptions).
2. Add declaration to `include/supabase/<module>/`, impl to `src/<module>/`.
3. Add a mock-transport test in `tests/` asserting URL, headers, body.
4. Update the feature status in `README.md` when it changes. Format only touched files.
