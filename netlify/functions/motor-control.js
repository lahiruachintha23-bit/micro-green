const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');
const { createClient } = require('@supabase/supabase-js');

function getSupabaseClient() {
    const supabaseUrl = process.env.SUPABASE_URL;
    const supabaseKey = process.env.SUPABASE_ANON_KEY || process.env.SUPABASE_SERVICE_ROLE_KEY;

    if (!supabaseUrl || !supabaseKey) {
        return null;
    }

    return createClient(supabaseUrl, supabaseKey);
}

function getStateFilePath() {
    return path.join('/tmp', 'microgreen-motor-state.json');
}

function readState() {
    try {
        const statePath = getStateFilePath();
        if (!fs.existsSync(statePath)) {
            return { action: 'none', state: 'UNKNOWN', updatedAt: null };
        }

        return JSON.parse(fs.readFileSync(statePath, 'utf8'));
    } catch (error) {
        return { action: 'none', state: 'UNKNOWN', updatedAt: null };
    }
}

function writeState(action) {
    const state = {
        action,
        state: action === 'on' ? 'ON' : action === 'off' ? 'OFF' : 'UNKNOWN',
        updatedAt: new Date().toISOString(),
    };

    try {
        fs.writeFileSync(getStateFilePath(), JSON.stringify(state));
    } catch (e) {
        // Ignore read-only filesystem errors if any
    }
    return state;
}

function getDeviceBaseUrl() {
    return process.env.ESP32_CONTROL_URL;
}

function isPrivateHost(hostname) {
    if (!hostname) return true;
    if (hostname === 'localhost' || hostname === '127.0.0.1' || hostname.endsWith('.local')) return true;
    if (/^192\.168\.\d{1,3}\.\d{1,3}$/.test(hostname)) return true;
    if (/^10\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(hostname)) return true;
    if (/^172\.(1[6-9]|2\d|3[0-1])\.\d{1,3}\.\d{1,3}$/.test(hostname)) return true;
    return false;
}

function forwardToDevice(action, mode, target) {
    return new Promise((resolve) => {
        const targetUrl = getDeviceBaseUrl();
        if (!targetUrl) {
            resolve({ ok: false, reason: 'No ESP32_CONTROL_URL configured in Netlify environment variables.' });
            return;
        }

        let url;
        try {
            url = new URL(targetUrl);
        } catch (e) {
            resolve({ ok: false, reason: `Invalid ESP32_CONTROL_URL format: ${targetUrl}` });
            return;
        }

        if (isPrivateHost(url.hostname)) {
            resolve({ ok: false, reason: `ESP32 target URL (${url.hostname}) is a private local IP. Netlify cloud servers cannot reach local LAN IPs directly.` });
            return;
        }

        let requestPath = url.pathname || '/';
        let requestQuery = {};

        if (target === 'fan' || action === 'fan') {
            requestPath = `${requestPath.replace(/\/$/, '')}/fan`;
            if (mode) requestQuery.mode = mode;
        } else if (target === 'mister' || action === 'mister') {
            requestPath = `${requestPath.replace(/\/$/, '')}/mister`;
            if (action && action !== 'mister') requestQuery.action = action;
            if (mode) requestQuery.mode = mode;
        } else if (mode) {
            requestPath = `${requestPath.replace(/\/$/, '')}/control`;
            requestQuery.mode = mode;
        } else if (['on', 'off', 'reset'].includes(action)) {
            requestPath = `${requestPath.replace(/\/$/, '')}/motor`;
            requestQuery.action = action;
        } else if (['Auto', 'ManualOn', 'ManualOff'].includes(action)) {
            requestPath = `${requestPath.replace(/\/$/, '')}/control`;
            requestQuery.mode = action;
        }

        const client = url.protocol === 'https:' ? https : http;
        const startTime = Date.now();
        const request = client.request(
            {
                hostname: url.hostname,
                port: url.port || (url.protocol === 'https:' ? 443 : 80),
                path: `${requestPath}?${new URLSearchParams(requestQuery).toString()}`,
                method: 'GET',
                timeout: 2500,
            },
            (response) => {
                let body = '';
                response.on('data', (chunk) => { body += chunk; });
                response.on('end', () => {
                    const latencyMs = Date.now() - startTime;
                    resolve({ ok: response.statusCode >= 200 && response.statusCode < 300, statusCode: response.statusCode, latencyMs, body });
                });
            }
        );

        request.on('timeout', () => {
            request.destroy();
            resolve({ ok: false, reason: 'ESP32 request timed out (2.5s)' });
        });

        request.on('error', (err) => {
            resolve({ ok: false, reason: `ESP32 connection error: ${err.message}` });
        });

        request.end();
    });
}

async function checkDeviceHealth() {
    const targetUrl = getDeviceBaseUrl();
    if (!targetUrl) {
        return { connected: false, reason: 'No ESP32_CONTROL_URL configured in Netlify environment variables.' };
    }

    try {
        const url = new URL(targetUrl);
        if (isPrivateHost(url.hostname)) {
            return {
                connected: false,
                reason: `ESP32 target URL (${targetUrl}) is a private local LAN IP. Netlify cloud servers cannot reach private IPs directly. Use direct local mode or configure a public tunnel.`,
                isPrivateIp: true
            };
        }

        const client = url.protocol === 'https:' ? https : http;
        const startTime = Date.now();

        const response = await new Promise((resolve) => {
            const request = client.request(
                {
                    hostname: url.hostname,
                    port: url.port || (url.protocol === 'https:' ? 443 : 80),
                    path: `${url.pathname.replace(/\/$/, '')}/health`,
                    method: 'GET',
                    timeout: 2500,
                },
                (res) => {
                    let body = '';
                    res.on('data', (chunk) => { body += chunk; });
                    res.on('end', () => {
                        const latencyMs = Date.now() - startTime;
                        try {
                            resolve({ statusCode: res.statusCode, latencyMs, body: body ? JSON.parse(body) : {} });
                        } catch (error) {
                            resolve({ statusCode: res.statusCode, latencyMs, body });
                        }
                    });
                }
            );

            request.on('timeout', () => {
                request.destroy();
                resolve({ statusCode: 504, reason: 'ESP32 health check timed out (2.5s)' });
            });
            request.on('error', (err) => resolve({ statusCode: 502, reason: err.message }));
            request.end();
        });

        const connected = response.statusCode >= 200 && response.statusCode < 300;
        return {
            connected,
            statusCode: response.statusCode,
            latencyMs: response.latencyMs,
            details: response.body,
            reason: response.reason
        };
    } catch (error) {
        return { connected: false, reason: error.message };
    }
}

async function checkDeviceStatus() {
    const targetUrl = getDeviceBaseUrl();
    if (!targetUrl) {
        return { connected: false, reason: 'No ESP32_CONTROL_URL configured in Netlify environment variables.' };
    }

    try {
        const url = new URL(targetUrl);
        if (isPrivateHost(url.hostname)) {
            return { connected: false, reason: 'Private IP target' };
        }

        const client = url.protocol === 'https:' ? https : http;
        const startTime = Date.now();

        const response = await new Promise((resolve) => {
            const request = client.request(
                {
                    hostname: url.hostname,
                    port: url.port || (url.protocol === 'https:' ? 443 : 80),
                    path: `${url.pathname.replace(/\/$/, '')}/status`,
                    method: 'GET',
                    timeout: 2500,
                },
                (res) => {
                    let body = '';
                    res.on('data', (chunk) => { body += chunk; });
                    res.on('end', () => {
                        const latencyMs = Date.now() - startTime;
                        try {
                            resolve({ statusCode: res.statusCode, latencyMs, body: body ? JSON.parse(body) : {} });
                        } catch (error) {
                            resolve({ statusCode: res.statusCode, latencyMs, body });
                        }
                    });
                }
            );

            request.on('timeout', () => {
                request.destroy();
                resolve({ statusCode: 504, reason: 'ESP32 status check timed out (2.5s)' });
            });
            request.on('error', (err) => resolve({ statusCode: 502, reason: err.message }));
            request.end();
        });

        const connected = response.statusCode >= 200 && response.statusCode < 300;
        return { connected, statusCode: response.statusCode, latencyMs: response.latencyMs, details: response.body };
    } catch (error) {
        return { connected: false, reason: error.message };
    }
}

async function syncToSupabaseIfConnected(telemetry) {
    const supabase = getSupabaseClient();
    if (!supabase || !telemetry) return false;

    try {
        const record = {
            temperature: telemetry.temperature ? parseFloat(telemetry.temperature) : null,
            humidity: telemetry.humidity ? parseFloat(telemetry.humidity) : null,
            flow: telemetry.flowValue ? parseFloat(telemetry.flowValue) : null,
            soil: telemetry.soilValue ? parseInt(telemetry.soilValue, 10) : null,
            height: telemetry.distance ? parseInt(telemetry.distance, 10) : null,
            stage: telemetry.growthStage || 'Unknown'
        };

        await supabase.from('sensor_readings').insert([record]);
        return true;
    } catch (e) {
        console.error('Supabase auto-sync error:', e.message);
        return false;
    }
}

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
            distance: last.height,
            soilValue: last.soil,
            flowValue: last.flow,
            temperature: last.temperature,
            humidity: last.humidity,
            growthStage: last.stage,
            isStaleDbValue: true
        };
    } catch (e) {
        return null;
    }
}

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
        if (event.httpMethod === 'GET') {
            const startTime = Date.now();
            const state = readState();
            
            // Query ESP32 health & status in parallel
            const [health, status] = await Promise.all([
                checkDeviceHealth(),
                checkDeviceStatus()
            ]);

            const isOnline = health.connected && status.connected;
            let details = isOnline && status.details ? status.details : {};
            let storedInSupabase = false;

            if (isOnline) {
                // Connection is ACTIVE -> Store sensor data in Supabase
                storedInSupabase = await syncToSupabaseIfConnected(details);
            } else {
                // Connection FAILS -> DO NOT STORE DATA.
                // Fetch last known readings from Supabase to present to user
                const lastDbReading = await getLastSupabaseTelemetry();
                if (lastDbReading) {
                    details = { ...lastDbReading, ...details };
                }
            }

            const totalDuration = Date.now() - startTime;

            return {
                statusCode: 200,
                headers,
                body: JSON.stringify({
                    ...state,
                    ...details,
                    deviceReachable: isOnline,
                    connectionStatus: isOnline ? 'online' : 'offline',
                    cloudStorageActive: isOnline && storedInSupabase,
                    gatewayLatencyMs: health.latencyMs || totalDuration,
                    targetUrl: getDeviceBaseUrl() || 'Not Configured',
                    health,
                }),
            };
        }

        if (event.httpMethod === 'POST') {
            let payload = {};

            try {
                payload = event.body ? JSON.parse(event.body) : {};
            } catch (error) {
                payload = {};
            }

            const action = payload.action || event.queryStringParameters?.action;
            const mode = payload.mode || event.queryStringParameters?.mode;
            const target = payload.target || event.queryStringParameters?.target;

            // Attempt to forward command to ESP32
            const forwarded = await forwardToDevice(action || mode, mode ? mode : undefined, target);

            let supabaseLogged = false;
            if (forwarded.ok) {
                // If command SUCCEEDED on ESP32 -> Log to Supabase commands table
                const state = writeState(action || mode || 'status');
                const supabase = getSupabaseClient();
                if (supabase) {
                    try {
                        await supabase.from('commands').insert([{
                            action: action || mode || 'control',
                            status: 'executed'
                        }]);
                        supabaseLogged = true;
                    } catch (err) {
                        console.error('Supabase command log failed:', err.message);
                    }
                }
                return {
                    statusCode: 200,
                    headers,
                    body: JSON.stringify({ ok: true, ...state, forwarded, supabaseLogged, source: 'netlify-function' }),
                };
            } else {
                // If ESP32 is UNREACHABLE -> DO NOT write pending command to Supabase
                return {
                    statusCode: 200,
                    headers,
                    body: JSON.stringify({
                        ok: false,
                        reason: forwarded.reason || 'ESP32 device unreachable. Command was NOT stored or executed.',
                        supabaseLogged: false,
                        forwarded
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
        return {
            statusCode: 200,
            headers,
            body: JSON.stringify({
                connectionStatus: 'offline',
                deviceReachable: false,
                cloudStorageActive: false,
                targetUrl: getDeviceBaseUrl() || 'Not Configured',
                health: { connected: false, reason: globalErr.message }
            })
        };
    }
};
