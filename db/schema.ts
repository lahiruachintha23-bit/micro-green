import { pgTable, uuid, timestamp, doublePrecision, integer, text, jsonb } from "drizzle-orm/pg-core";

export const sensorReadings = pgTable("sensor_readings", {
  id: uuid("id").primaryKey().defaultRandom(),
  recordedAt: timestamp("recorded_at", { withTimezone: true }).notNull().defaultNow(),
  temperature: doublePrecision("temperature"),
  humidity: doublePrecision("humidity"),
  flow: doublePrecision("flow"),
  soil: integer("soil"),
  height: integer("height"),
  stage: text("stage"),
});

export const waterEvents = pgTable("water_events", {
  id: uuid("id").primaryKey().defaultRandom(),
  recordedAt: timestamp("recorded_at", { withTimezone: true }).notNull().defaultNow(),
  eventType: text("event_type").notNull(),
  details: text("details"),
  metadata: jsonb("metadata"),
});

export const commands = pgTable("commands", {
  id: uuid("id").primaryKey().defaultRandom(),
  recordedAt: timestamp("recorded_at", { withTimezone: true }).notNull().defaultNow(),
  action: text("action").notNull(),
  status: text("status").notNull().default("pending"),
});
