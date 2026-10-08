create table public.todos (
    id bigint generated always as identity primary key,
    title text not null,
    done boolean not null default false
);

grant all on public.todos to anon, authenticated, service_role;
alter table public.todos replica identity full;
alter publication supabase_realtime add table public.todos;

create function public.add_numbers(a int, b int) returns int
language sql immutable as 'select a + b';
grant execute on function public.add_numbers to anon, authenticated, service_role;
