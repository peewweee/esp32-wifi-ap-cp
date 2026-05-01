-- ============================================================================
-- Smart Solar Hub — Supabase schema of record
-- ============================================================================
-- Last verified against the live project: 2026-04-24
-- Project ref: pzkatqdyxexkevyccjzm
--
-- Replaces the previous files 001_session_linking.sql and 002_session_freshness.sql,
-- which described a schema that was never fully applied to the live database.
--
-- This file is DOCUMENTATION first, runnable-on-a-fresh-project second.
-- The TABLE section can be applied as-is. The FUNCTION section contains
-- SIGNATURES ONLY — bodies must be pasted from Supabase Studio (the REST
-- API cannot export function definitions). See the helper RPC at the bottom.
--
-- ⚠️  DO NOT paste this entire file into the SQL Editor of the existing
--     live project. The function stubs below would OVERWRITE the three
--     working RPCs (claim_session_link, resolve_installation_session,
--     cleanup_old_sessions) with bodies that just raise an exception.
--     To add new tables / indexes / grants, run ONLY the relevant section.
-- ============================================================================

create extension if not exists pgcrypto;

-- ----------------------------------------------------------------------------
-- TABLE: public.sessions
-- ----------------------------------------------------------------------------
-- Single source of truth for all session rows. Carries BOTH the legacy
-- column names (token, mac_hash) AND the newer ones (session_token,
-- device_hash). Every row in the live DB has these pairs populated with
-- identical values; callers may read either spelling. Something in the
-- write path (ESP32 firmware or a trigger) is keeping them in sync —
-- audit this before dropping the legacy columns.
-- ----------------------------------------------------------------------------
create table if not exists public.sessions (
    id                 bigint generated always as identity primary key,

    -- Legacy identifiers (still required / NOT NULL in live DB)
    token              text not null,
    mac_hash           text not null,

    -- New-style identifiers (nullable in live DB; populated alongside legacy)
    session_token      text,
    device_hash        text,

    -- Linked PWA installation (set by claim_session_link; null until the
    -- user opens the one-time redirect and the PWA binds itself)
    installation_id    text,

    -- Timing
    session_start      timestamptz not null default now(),
    session_end        timestamptz not null,
    remaining_seconds  integer     not null default 3600,
    last_heartbeat     timestamptz not null default now(),

    -- Status
    status             text        not null default 'active',
        -- observed values so far: 'active'
        -- expected by ESP32 firmware and PWA code: 'active' | 'expired' | 'disconnected'
    ap_connected       boolean     not null default true
);

-- Indexes that exist in the live DB are not introspectable via REST;
-- recommended ones based on query patterns:
create index if not exists sessions_token_idx           on public.sessions (token);
create index if not exists sessions_session_token_idx   on public.sessions (session_token);
create index if not exists sessions_device_hash_idx     on public.sessions (device_hash);
create index if not exists sessions_mac_hash_idx        on public.sessions (mac_hash);
create index if not exists sessions_installation_id_idx on public.sessions (installation_id);
create index if not exists sessions_last_heartbeat_idx  on public.sessions (last_heartbeat desc);

-- ----------------------------------------------------------------------------
-- TABLE: public.port_state
-- ----------------------------------------------------------------------------
-- One row per (station_id, port_key). The ESP32 upserts on every port
-- status change (via /api/ports -> supabase_post_upsert). The PWA reads
-- these rows to render the "Available Ports" section on the dashboard.
-- ----------------------------------------------------------------------------
create table if not exists public.port_state (
    id          bigint generated always as identity primary key,
    station_id  text not null default 'solar-hub-01',
    port_key    text not null
        check (port_key in ('usb_a_1','usb_a_2','usb_c_1','usb_c_2','outlet')),
    status      text not null default 'available'
        check (status in ('available','in_use','fault','offline')),
    current_ma  numeric,
    bus_voltage_v numeric,
    updated_at  timestamptz not null default now(),
    unique (station_id, port_key)
);
create index if not exists port_state_station_idx on public.port_state (station_id);

-- ----------------------------------------------------------------------------
-- TABLE: public.station_state
-- ----------------------------------------------------------------------------
-- One row per station. Holds station-wide metrics not attributable to a
-- specific port (battery level, future: solar watts, card_present, etc.).
-- ----------------------------------------------------------------------------
create table if not exists public.station_state (
    station_id      text primary key default 'solar-hub-01',
    battery_percent numeric check (battery_percent between 0 and 100),
    updated_at      timestamptz not null default now()
);

-- ----------------------------------------------------------------------------
-- Shared trigger: keep updated_at fresh on any UPDATE.
-- Reused by port_state and station_state. Safe to re-run.
-- ----------------------------------------------------------------------------
create or replace function public.set_updated_at()
returns trigger language plpgsql as $$
begin new.updated_at = now(); return new; end;
$$;

drop trigger if exists port_state_touch on public.port_state;
create trigger port_state_touch before update on public.port_state
    for each row execute function public.set_updated_at();

drop trigger if exists station_state_touch on public.station_state;
create trigger station_state_touch before update on public.station_state
    for each row execute function public.set_updated_at();

-- ----------------------------------------------------------------------------
-- SECURITY MODEL (as of 2026-04-24)
-- ----------------------------------------------------------------------------
-- RLS is effectively OFF. The anon key can SELECT and UPDATE rows
-- directly (PATCH /rest/v1/sessions?token=eq.XXX returned 204). This
-- is verified; it is NOT the normalized-table design that 001_session_linking.sql
-- originally described.
--
-- Practical consequence: anyone with the public anon key (shipped to the
-- browser) can read every session row and modify any row by token. For a
-- single-device thesis demo this is acceptable. BEFORE the project sees
-- real users, enable RLS on public.sessions and restrict direct access so
-- that only the three RPCs below can touch the table.
-- ----------------------------------------------------------------------------
alter table public.sessions       disable row level security;
alter table public.port_state     disable row level security;
alter table public.station_state  disable row level security;

grant select, insert, update, delete on public.sessions      to anon, authenticated;
grant select, insert, update         on public.port_state    to anon, authenticated;
grant select, insert, update         on public.station_state to anon, authenticated;

-- ----------------------------------------------------------------------------
-- FUNCTIONS (RPC surface)
-- ----------------------------------------------------------------------------
-- Signatures verified against PostgREST OpenAPI on 2026-04-24.
-- Parameter names and types are authoritative; bodies are NOT included here
-- because they cannot be read via the REST API.
--
-- To extract the live bodies, run the helper at the bottom of this file in
-- Supabase Studio, call it once from any REST client, paste the returned
-- definitions over the stubs below, then drop the helper.
-- ----------------------------------------------------------------------------

-- claim_session_link(session_token text, installation_id text)
--   Called by the PWA when the one-time redirect URL is opened.
--   Observed behavior:
--     - looks up the session row by session_token (or legacy token column)
--     - writes the caller's installation_id into that row
--     - returns the affected row (empty set if no match)
--   Callable by: anon, authenticated
create or replace function public.claim_session_link(
    session_token   text,
    installation_id text
) returns setof public.sessions
language plpgsql security definer
set search_path = public
as $$
begin
    raise exception 'STUB: replace this body with the live function definition from Supabase Studio';
end;
$$;

-- resolve_installation_session(installation_id text)
--   Called by the PWA on the generic dashboard route (no token in URL).
--   Observed behavior:
--     - finds the most recent session linked to this installation_id
--     - returns that row, or empty if the browser has never been linked
--   Callable by: anon, authenticated
create or replace function public.resolve_installation_session(
    installation_id text
) returns setof public.sessions
language plpgsql security definer
set search_path = public
as $$
begin
    raise exception 'STUB: replace this body with the live function definition from Supabase Studio';
end;
$$;

-- cleanup_old_sessions()
--   Present in the live DB but not previously documented.
--   Purpose inferred from the name: deletes (or archives) stale session
--   rows so the table does not grow unbounded. Likely criteria:
--     session_end < now()  OR  status in ('expired', 'disconnected')
--   Schedule is unknown — could be triggered by pg_cron, by a
--   Supabase Edge Function, or called manually by an operator. Verify
--   in Supabase Studio (Database → Cron Jobs) whether it is scheduled.
--   Callable by: anon, authenticated, service_role
create or replace function public.cleanup_old_sessions()
returns void
language plpgsql security definer
set search_path = public
as $$
begin
    raise exception 'STUB: replace this body with the live function definition from Supabase Studio';
end;
$$;

grant execute on function public.claim_session_link(text, text)        to anon, authenticated;
grant execute on function public.resolve_installation_session(text)    to anon, authenticated;
grant execute on function public.cleanup_old_sessions()                to anon, authenticated, service_role;

-- ============================================================================
-- RECOVERY HELPER — run once in Supabase Studio to export real function bodies
-- ============================================================================
-- 1. Uncomment the block below.
-- 2. Paste into Supabase Studio → SQL Editor → Run.
-- 3. Call it from any REST client:
--      POST <project>/rest/v1/rpc/__export_function_defs
--      headers: apikey: <service-role-key>
--      body:    {}
-- 4. Paste each returned `def` into the corresponding stub above.
-- 5. Drop the helper:  drop function public.__export_function_defs();
-- ============================================================================
-- create or replace function public.__export_function_defs()
-- returns table(fn_name text, def text)
-- language sql security definer
-- as $$
--     select p.proname::text, pg_get_functiondef(p.oid)
--     from pg_proc p
--     join pg_namespace n on n.oid = p.pronamespace
--     where n.nspname = 'public'
--       and p.proname in (
--           'claim_session_link',
--           'resolve_installation_session',
--           'cleanup_old_sessions'
--       );
-- $$;
-- grant execute on function public.__export_function_defs() to service_role;
