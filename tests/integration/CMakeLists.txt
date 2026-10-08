# Needs a running Supabase stack; see .github/workflows/integration.yml. Not registered with ctest.
add_executable(supabase_integration main.cpp)
target_link_libraries(supabase_integration PRIVATE supabase::supabase)
