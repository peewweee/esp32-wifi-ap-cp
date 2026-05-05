-- Add AC telemetry columns used by the ESP32 PZEM-004T v3 upsert.

alter table public.station_state
    add column if not exists ac_voltage_v numeric,
    add column if not exists ac_current_a numeric,
    add column if not exists ac_power_w   numeric,
    add column if not exists ac_energy_wh integer;
