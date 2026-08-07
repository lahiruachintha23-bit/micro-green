# Supabase setup for the microgreen dashboard

1. Create a new Supabase project.
2. Open the SQL editor.
3. Run the contents of `schema.sql`.
4. Copy the project URL and anon key from Project Settings -> API.
5. Add these values to Netlify environment variables:

- `SUPABASE_URL`
- `SUPABASE_ANON_KEY`
- `ESP32_CONTROL_URL` (optional for direct ESP32 forwarding)

## Expected tables

- `sensor_readings`
- `water_events`
- `commands`

## Example insert for sensor data

```sql
insert into sensor_readings (
  temperature,
  humidity,
  flow,
  soil,
  height,
  stage
) values (
  24.5,
  58.2,
  110.4,
  1800,
  12,
  'Growth'
);
```

## Example insert for motor command

```sql
insert into commands (action, status)
values ('on', 'pending');
```

## Recommended next step

Expose these tables through your Netlify function or a small backend API and let the ESP32 poll for pending commands.
