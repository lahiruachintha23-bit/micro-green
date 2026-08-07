CREATE TABLE "commands" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid(),
	"recorded_at" timestamp with time zone DEFAULT now() NOT NULL,
	"action" text NOT NULL,
	"status" text DEFAULT 'pending' NOT NULL
);
--> statement-breakpoint
CREATE TABLE "sensor_readings" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid(),
	"recorded_at" timestamp with time zone DEFAULT now() NOT NULL,
	"temperature" double precision,
	"humidity" double precision,
	"flow" double precision,
	"soil" integer,
	"height" integer,
	"stage" text
);
--> statement-breakpoint
CREATE TABLE "water_events" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid(),
	"recorded_at" timestamp with time zone DEFAULT now() NOT NULL,
	"event_type" text NOT NULL,
	"details" text,
	"metadata" jsonb
);
