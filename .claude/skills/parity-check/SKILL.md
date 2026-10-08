---
name: parity-check
description: Compare public C++ signatures against supabase-js naming/behavior for a module and list gaps.
---
Grep public headers for the module, compare against the known supabase-js surface (names, options, return shape). Output a gap table: method, supabase-js name, status (ok/missing/renamed). Do not edit.
