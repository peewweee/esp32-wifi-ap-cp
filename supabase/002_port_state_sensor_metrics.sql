-- Add nullable live sensor metrics to port_state.
-- Existing firmware/manual /ports writers that only send status continue to work.

alter table public.port_state
    add column if not exists current_ma numeric,
    add column if not exists bus_voltage_v numeric;
