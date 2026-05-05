-- Eco-metrics inputs: per-port daily in_use seconds (drives the USB Wh
-- estimate via Pavg × hours) and the AC outlet's measured Wh today
-- (delta from midnight on the PZEM cumulative counter).

alter table public.port_state
    add column if not exists daily_in_use_seconds integer default 0;

alter table public.station_state
    add column if not exists ac_energy_wh_today integer default 0;
