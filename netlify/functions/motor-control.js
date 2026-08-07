const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');

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

    fs.writeFileSync(getStateFilePath(), JSON.stringify(state));
    return state;
}

function getDeviceBaseUrl() {
    return process.env.ESP32_CONTROL_URL;
}

function buildDeviceUrl(targetPath, query = {}) {
    const baseUrl = getDeviceBaseUrl();
    if (!baseUrl) {
        return null;
    }

    const url = new URL(baseUrl);
    url.pathname = `${url.pathname.replace(/\/$/, '')}${targetPath}`;

    Object.entries(query).forEach(([key, value]) => {
        if (value !== undefined && value !== null) {
            url.searchParams.set(key, String(value));
        }
    });

    return url;
}

function forwardToDevice(action, mode, target) {
    return new Promise((resolve) => {
        const targetUrl = getDeviceBaseUrl();
        if (!targetUrl) {
            resolve({ ok: false, reason: 'No ESP32_CONTROL_URL configured' });
            return;
        }

        const url = new URL(targetUrl);
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
                timeout: 6000,
            },
            (response) => {
                let body = '';
                response.on('data', (chunk) => {
                    body += chunk;
                });
                response.on('end', () => {
                    const latencyMs = Date.now() - startTime;
                    resolve({ ok: response.statusCode >= 200 && response.statusCode < 300, statusCode: response.statusCode, latencyMs, body });
                });
            }
        );

        request.on('timeout', () => {
            request.destroy(new Error('ESP32 command timed out'));
        });

        request.on('error', () => {
            resolve({ ok: false, reason: 'ESP32 request failed' });
        });

        request.end();
    });
}

async function checkDeviceHealth() {
    const targetUrl = getDeviceBaseUrl();
    if (!targetUrl) {
        return { connected: false, reason: 'No ESP32_CONTROL_URL configured' };
    }

    try {
        const url = new URL(targetUrl);
        const client = url.protocol === 'https:' ? https : http;
        const startTime = Date.now();

        const response = await new Promise((resolve, reject) => {
            const request = client.request(
                {
                    hostname: url.hostname,
                    port: url.port || (url.protocol === 'https:' ? 443 : 80),
                    path: `${url.pathname.replace(/\/$/, '')}/health`,
                    method: 'GET',
                    timeout: 5000,
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
                request.destroy(new Error('ESP32 health check timed out'));
            });
            request.on('error', reject);
            request.end();
        });

        const connected = response.statusCode >= 200 && response.statusCode < 300;
        return {
            connected,
            statusCode: response.statusCode,
            latencyMs: response.latencyMs,
            details: response.body,
        };
    } catch (error) {
        return { connected: false, reason: error.message };
    }
}

async function checkDeviceStatus() {
    const targetUrl = getDeviceBaseUrl();
    if (!targetUrl) {
        return { connected: false, reason: 'No ESP32_CONTROL_URL configured' };
    }

    try {
        const url = new URL(targetUrl);
        const client = url.protocol === 'https:' ? https : http;
        const startTime = Date.now();

        const response = await new Promise((resolve, reject) => {
            const request = client.request(
                {
                    hostname: url.hostname,
                    port: url.port || (url.protocol === 'https:' ? 443 : 80),
                    path: `${url.pathname.replace(/\/$/, '')}/status`,
                    method: 'GET',
                    timeout: 5000,
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
                request.destroy(new Error('ESP32 status check timed out'));
            });
            request.on('error', reject);
            request.end();
        });

        const connected = response.statusCode >= 200 && response.statusCode < 300;
        return { connected, statusCode: response.statusCode, latencyMs: response.latencyMs, details: response.body };
    } catch (error) {
        return { connected: false, reason: error.message };
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

    if (event.httpMethod === 'GET') {
        const startTime = Date.now();
        const state = readState();
        const health = await checkDeviceHealth();
        const status = await checkDeviceStatus();
        const details = status.connected && status.details ? status.details : {};
        const totalDuration = Date.now() - startTime;

        return {
            statusCode: 200,
            headers,
            body: JSON.stringify({
                ...state,
                ...details,
                deviceReachable: health.connected,
                connectionStatus: health.connected ? 'online' : 'offline',
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
        const state = writeState(action || mode || 'status');
        const forwarded = await forwardToDevice(action || mode, mode ? mode : undefined, target);

        return {
            statusCode: 200,
            headers,
            body: JSON.stringify({ ok: true, ...state, forwarded, source: 'netlify-function' }),
        };
    }

    return {
        statusCode: 405,
        headers,
        body: JSON.stringify({ ok: false, error: 'Method not allowed' }),
    };
};
