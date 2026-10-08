add_executable(supabase_tests main.cpp)
target_link_libraries(supabase_tests PRIVATE supabase::supabase)
add_test(NAME supabase_tests COMMAND supabase_tests)
