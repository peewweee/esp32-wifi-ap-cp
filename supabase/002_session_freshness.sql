create or replace function public.resolve_installation_session(
    p_installation_id text
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
language sql
security definer
set search_path = public
as $$
    with linked_device as (
        select coalesce(s.device_hash, s.mac_hash) as device_hash
        from public.sessions s
        where s.installation_id = p_installation_id
          and coalesce(s.device_hash, s.mac_hash) is not null
        order by coalesce(s.last_heartbeat, s.session_start, s.session_end) desc nulls last,
                 s.id desc
        limit 1
    )
    select
        coalesce(s.session_token, s.token) as session_token,
        coalesce(s.device_hash, s.mac_hash) as device_hash,
        case
            when coalesce(lower(s.status), '') = 'expired' or coalesce(s.remaining_seconds, 0) <= 0
                then 0
            else coalesce(s.remaining_seconds, 0)::integer
        end as remaining_seconds,
        case
            when coalesce(lower(s.status), '') = 'expired' or coalesce(s.remaining_seconds, 0) <= 0
                then 'expired'
            else lower(coalesce(s.status, 'disconnected'))
        end as status,
        case
            when coalesce(lower(s.status), '') = 'expired' or coalesce(s.remaining_seconds, 0) <= 0
                then false
            else coalesce(s.ap_connected, false)
        end as ap_connected,
        s.session_end,
        s.last_heartbeat
    from public.sessions s
    join linked_device ld on ld.device_hash = coalesce(s.device_hash, s.mac_hash)
    order by coalesce(s.last_heartbeat, s.session_start, s.session_end) desc nulls last,
             s.id desc
    limit 1;
$$;
