create extension if not exists pgcrypto;

create table if not exists sensor_readings (
  id uuid primary key default gen_random_uuid(),
  recorded_at timestamptz not null default now(),
  temperature double precision,
  humidity double precision,
  flow double precision,
  soil integer,
  height integer,
  stage text
);

create table if not exists water_events (
  id uuid primary key default gen_random_uuid(),
  recorded_at timestamptz not null default now(),
  event_type text not null,
  details text,
  metadata jsonb
);

create table if not exists commands (
  id uuid primary key default gen_random_uuid(),
  recorded_at timestamptz not null default now(),
  action text not null,
  status text not null default 'pending'
);

create index if not exists idx_sensor_readings_recorded_at
  on sensor_readings (recorded_at desc);

create index if not exists idx_water_events_recorded_at
  on water_events (recorded_at desc);

create index if not exists idx_commands_recorded_at
  on commands (recorded_at desc);

alter table public.sensor_readings enable row level security;
alter table public.water_events enable row level security;
alter table public.commands enable row level security;

create policy "Allow public read sensor readings"
  on public.sensor_readings
  for select
  using (true);

create policy "Allow public read water events"
  on public.water_events
  for select
  using (true);

create policy "Allow public read commands"
  on public.commands
  for select
  using (true);

create policy "Allow public insert sensor readings"
  on public.sensor_readings
  for insert
  with check (true);

create policy "Allow public insert water events"
  on public.water_events
  for insert
  with check (true);

create policy "Allow public insert commands"
  on public.commands
  for insert
  with check (true);
