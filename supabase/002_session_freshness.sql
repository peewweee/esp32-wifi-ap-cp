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
declare
    v_now timestamptz := timezone('utc', now());
    v_stale_after interval := interval '120 seconds';
begin
    if p_installation_id is null then
        return;
    end if;

    insert into public.pwa_installations (installation_id, last_seen_at)
    values (p_installation_id, v_now)
    on conflict (installation_id)
    do update
    set last_seen_at = excluded.last_seen_at;

    return query
    with ranked_sessions as (
        select
            s.session_token,
            s.device_hash,
            case
                when s.status = 'active' and s.remaining_seconds <= 0 then 0
                when s.status = 'active' and coalesce(s.last_heartbeat, s.created_at) < (v_now - v_stale_after) then greatest(s.remaining_seconds, 0)
                else s.remaining_seconds
            end as effective_remaining_seconds,
            case
                when s.status = 'active' and s.remaining_seconds <= 0 then 'expired'
                when s.status = 'active' and coalesce(s.last_heartbeat, s.created_at) < (v_now - v_stale_after) then 'disconnected'
                else s.status
            end as effective_status,
            case
                when s.status = 'active' and coalesce(s.last_heartbeat, s.created_at) < (v_now - v_stale_after) then false
                else s.ap_connected
            end as effective_ap_connected,
            s.session_end,
            s.last_heartbeat,
            coalesce(s.last_heartbeat, s.created_at) as freshness_order
        from public.installation_links il
        join public.sessions s
          on s.device_hash = il.device_hash
        where il.installation_id = p_installation_id
    )
    select
        rs.session_token,
        rs.device_hash,
        rs.effective_remaining_seconds,
        rs.effective_status,
        rs.effective_ap_connected,
        rs.session_end,
        rs.last_heartbeat
    from ranked_sessions rs
    order by
        case
            when rs.effective_status = 'active'
                 and rs.effective_ap_connected
                 and rs.effective_remaining_seconds > 0 then 0
            else 1
        end,
        rs.freshness_order desc
    limit 1;
end;
$$;
