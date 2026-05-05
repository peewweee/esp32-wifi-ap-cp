-- Add hardware battery telemetry columns used by the ESP32 station_state upsert.

alter table public.station_state
    add column if not exists battery_voltage_v numeric,
    add column if not exists battery_raw_mv integer,
    add column if not exists battery_state text;
