create extension if not exists pgcrypto;

create table if not exists public.sessions (
    session_token text primary key,
    device_hash text not null,
    remaining_seconds integer not null default 0,
    status text not null default 'active'
        check (status in ('active', 'expired', 'disconnected')),
    ap_connected boolean not null default false,
    session_end timestamptz,
    last_heartbeat timestamptz,
    created_at timestamptz not null default timezone('utc', now()),
    updated_at timestamptz not null default timezone('utc', now())
);

create index if not exists sessions_device_hash_idx
    on public.sessions (device_hash);

create index if not exists sessions_last_heartbeat_idx
    on public.sessions (last_heartbeat desc);

create table if not exists public.pwa_installations (
    installation_id uuid primary key,
    created_at timestamptz not null default timezone('utc', now()),
    last_seen_at timestamptz not null default timezone('utc', now())
);

create table if not exists public.installation_links (
    installation_id uuid not null references public.pwa_installations(installation_id) on delete cascade,
    device_hash text not null,
    linked_at timestamptz not null default timezone('utc', now()),
    last_linked_at timestamptz not null default timezone('utc', now()),
    last_session_token text,
    primary key (installation_id, device_hash)
);

create index if not exists installation_links_device_hash_idx
    on public.installation_links (device_hash);

create or replace function public.set_updated_at()
returns trigger
language plpgsql
as $$
begin
    new.updated_at = timezone('utc', now());
    return new;
end;
$$;

drop trigger if exists sessions_set_updated_at on public.sessions;
create trigger sessions_set_updated_at
before update on public.sessions
for each row
execute function public.set_updated_at();

create or replace function public.claim_session_link(
    p_session_token text,
    p_installation_id uuid
)
returns table (
    session_token text,
    device_hash text,
    remaining_seconds integer,
    status text,
    ap_connected boolean,
    session_end timestamptz,
    last_heartbeat timestamptz
)
language plpgsql
security definer
set search_path = public
as $$
declare
    v_session public.sessions%rowtype;
begin
    if p_session_token is null or length(trim(p_session_token)) = 0 then
        raise exception 'session_token is required';
    end if;

    if p_installation_id is null then
        raise exception 'installation_id is required';
    end if;

    select *
    into v_session
    from public.sessions
    where public.sessions.session_token = p_session_token;

    if not found then
        raise exception 'session not found';
    end if;

    insert into public.pwa_installations (installation_id, last_seen_at)
    values (p_installation_id, timezone('utc', now()))
    on conflict (installation_id)
    do update
    set last_seen_at = excluded.last_seen_at;

    insert into public.installation_links (
        installation_id,
        device_hash,
        linked_at,
        last_linked_at,
        last_session_token
    )
    values (
        p_installation_id,
        v_session.device_hash,
        timezone('utc', now()),
        timezone('utc', now()),
        v_session.session_token
    )
    on conflict (installation_id, device_hash)
    do update
    set last_linked_at = excluded.last_linked_at,
        last_session_token = excluded.last_session_token;

    return query
    select
        v_session.session_token,
        v_session.device_hash,
        v_session.remaining_seconds,
        v_session.status,
        v_session.ap_connected,
        v_session.session_end,
        v_session.last_heartbeat;
end;
$$;

create or replace function public.resolve_installation_session(
    p_installation_id uuid
)
returns table (
    session_token text,
    device_hash text,
    remaining_seconds integer,
    status text,
    ap_connected boolean,
    session_end timestamptz,
    last_heartbeat timestamptz
)
language plpgsql
security definer
set search_path = public
as $$
begin
    if p_installation_id is null then
        return;
    end if;

    insert into public.pwa_installations (installation_id, last_seen_at)
    values (p_installation_id, timezone('utc', now()))
    on conflict (installation_id)
    do update
    set last_seen_at = excluded.last_seen_at;

    return query
    select
        s.session_token,
        s.device_hash,
        s.remaining_seconds,
        s.status,
        s.ap_connected,
        s.session_end,
        s.last_heartbeat
    from public.installation_links il
    join public.sessions s
      on s.device_hash = il.device_hash
    where il.installation_id = p_installation_id
    order by
        case
            when s.status = 'active' and s.ap_connected and s.remaining_seconds > 0 then 0
            else 1
        end,
        coalesce(s.last_heartbeat, s.created_at) desc
    limit 1;
end;
$$;

alter table public.sessions enable row level security;
alter table public.pwa_installations enable row level security;
alter table public.installation_links enable row level security;

revoke all on public.sessions from anon, authenticated;
revoke all on public.pwa_installations from anon, authenticated;
revoke all on public.installation_links from anon, authenticated;

grant execute on function public.claim_session_link(text, uuid) to anon, authenticated;
grant execute on function public.resolve_installation_session(uuid) to anon, authenticated;
