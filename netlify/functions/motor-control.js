const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');
const { createClient } = require('@supabase/supabase-js');

// ==================== Supabase Client ====================
function getSupabaseClient() {
    const supabaseUrl = process.env.SUPABASE_URL;
    const supabaseKey = process.env.SUPABASE_ANON_KEY || process.env.SUPABASE_SERVICE_ROLE_KEY;
    if (!supabaseUrl || !supabaseKey) return null;
    return createClient(supabaseUrl, supabaseKey);
}

// ==================== State File (ephemeral, /tmp) ====================
function getStateFilePath() {
    return path.join('/tmp', 'microgreen-motor-state.json');
}

function readState() {
    try {
        const p = getStateFilePath();
        if (!fs.existsSync(p)) return { action: 'none', state: 'UNKNOWN', updatedAt: null };
        return JSON.parse(fs.readFileSync(p, 'utf8'));
    } catch {
        return { action: 'none', state: 'UNKNOWN', updatedAt: null };
    }
}

function writeState(action) {
    const state = {
        action,
        state: action === 'on' || action === 'motor_on' ? 'ON'
             : action === 'off' || action === 'motor_off' ? 'OFF'
             : 'UNKNOWN',
        updatedAt: new Date().toISOString(),
    };
    try { fs.writeFileSync(getStateFilePath(), JSON.stringify(state)); } catch {}
    return state;
}

// ==================== Supabase: Read Last Sensor Reading ====================
async function getLastSupabaseTelemetry() {
    const supabase = getSupabaseClient();
    if (!supabase) return null;
    try {
        const { data, error } = await supabase
            .from('sensor_readings')
            .select('*')
            .order('recorded_at', { ascending: false })
            .limit(1);
        if (error || !data || data.length === 0) return null;
        const last = data[0];
        return {
            distance:     last.height,
            soilValue:    last.soil,
            flowValue:    last.flow,
            temperature:  last.temperature,
            humidity:     last.humidity,
            growthStage:  last.stage,
            recordedAt:   last.recorded_at,
            isStaleDbValue: true
        };
    } catch { return null; }
}

// ==================== Supabase: Write Pending Command ====================
// Instead of forwarding directly to ESP32 (which is on a private local IP),
// we write a 'pending' command row. The ESP32 polls Supabase every 5s,
// finds the row, executes it on the hardware, and marks it 'executed'.
async function writeCommandToSupabase(action) {
    const supabase = getSupabaseClient();
    if (!supabase) {
        return { ok: false, reason: 'Supabase not configured (check SUPABASE_URL and SUPABASE_ANON_KEY in Netlify env vars)' };
    }
    try {
        const { data, error } = await supabase
            .from('commands')
            .insert([{ action, status: 'pending' }])
            .select();
        if (error) throw error;
        return { ok: true, commandId: data?.[0]?.id, status: 'pending', note: 'ESP32 will pick this up within 5 seconds' };
    } catch (err) {
        return { ok: false, reason: `Supabase write error: ${err.message}` };
    }
}

// ==================== Main Handler ====================
exports.handler = async (event) => {
    const headers = {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Headers': 'Content-Type',
        'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
        'Content-Type': 'application/json',
    };

    if (event.httpMethod === 'OPTIONS') {
        return { statusCode: 200, headers, body: '' };
    }

    try {
        // ====== GET — return latest status from Supabase ======
        if (event.httpMethod === 'GET') {
            const state = readState();
            const lastTelemetry = await getLastSupabaseTelemetry();
            const supabase = getSupabaseClient();
            const supabaseConfigured = !!supabase;

            // Check how recent the last reading is (within 60s = device likely online)
            let deviceReachable = false;
            let connectionStatus = 'offline';
            let cloudStorageActive = false;

            if (lastTelemetry && lastTelemetry.recordedAt) {
                const ageMs = Date.now() - new Date(lastTelemetry.recordedAt).getTime();
                // If a reading arrived within the last 60 seconds, ESP32 is pushing live
                if (ageMs < 60000) {
                    deviceReachable = true;
                    connectionStatus = 'online';
                    cloudStorageActive = true;
                } else if (ageMs < 300000) {
                    // Reading is 1–5 minutes old — device was recently online
                    deviceReachable = false;
                    connectionStatus = 'stale';
                    cloudStorageActive = false;
                }
            }

            return {
                statusCode: 200,
                headers,
                body: JSON.stringify({
                    ...state,
                    ...(lastTelemetry || {}),
                    deviceReachable,
                    connectionStatus,
                    cloudStorageActive,
                    supabaseConfigured,
                    source: 'supabase-db',
                    targetUrl: 'Direct ESP32 Push (no tunnel needed)',
                    health: {
                        connected: deviceReachable,
                        reason: deviceReachable
                            ? 'ESP32 is pushing live data to Supabase'
                            : lastTelemetry
                                ? `Last reading: ${lastTelemetry.recordedAt}`
                                : 'No readings in Supabase yet — ensure ESP32 is powered on and connected to WiFi',
                    }
                }),
            };
        }

        // ====== POST — write command to Supabase for ESP32 to pick up ======
        if (event.httpMethod === 'POST') {
            let payload = {};
            try { payload = event.body ? JSON.parse(event.body) : {}; } catch {}

            const rawAction = payload.action || event.queryStringParameters?.action;
            const mode      = payload.mode   || event.queryStringParameters?.mode;
            const target    = payload.target || event.queryStringParameters?.target;

            // Map dashboard actions to Supabase command action strings
            let commandAction = rawAction || mode || 'status';

            if (target === 'fan') {
                commandAction = mode === 'ManualOn' ? 'fan_on'
                              : mode === 'ManualOff' ? 'fan_off'
                              : 'fan_auto';
            } else if (target === 'mister') {
                commandAction = rawAction === 'spray' || mode === 'ManualOn' ? 'mister_spray'
                              : mode === 'ManualOff' ? 'mister_off'
                              : 'mister_auto';
            } else if (mode && !rawAction) {
                // Pump mode change
                commandAction = mode === 'ManualOn'  ? 'pump_on'
                              : mode === 'ManualOff' ? 'pump_off'
                              : 'pump_auto';
            } else if (rawAction === 'on') {
                commandAction = 'motor_on';
            } else if (rawAction === 'off') {
                commandAction = 'motor_off';
            } else if (rawAction === 'reset') {
                commandAction = 'motor_reset';
            }

            const result = await writeCommandToSupabase(commandAction);

            if (result.ok) {
                const state = writeState(commandAction);
                return {
                    statusCode: 200,
                    headers,
                    body: JSON.stringify({
                        ok: true,
                        ...state,
                        commandId: result.commandId,
                        commandAction,
                        status: 'pending',
                        note: 'Command queued in Supabase. ESP32 will execute within 5 seconds.',
                        source: 'supabase-command-queue'
                    }),
                };
            } else {
                return {
                    statusCode: 200,
                    headers,
                    body: JSON.stringify({
                        ok: false,
                        reason: result.reason,
                        commandAction,
                    }),
                };
            }
        }

        return {
            statusCode: 405,
            headers,
            body: JSON.stringify({ ok: false, error: 'Method not allowed' }),
        };

    } catch (globalErr) {
        console.error('[motor-control] Unhandled error:', globalErr.message);
        return {
            statusCode: 200,
            headers,
            body: JSON.stringify({
                connectionStatus: 'offline',
                deviceReachable: false,
                cloudStorageActive: false,
                health: { connected: false, reason: globalErr.message }
            })
        };
    }
};
