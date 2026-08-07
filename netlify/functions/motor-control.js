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

function forwardToDevice(action) {
    return new Promise((resolve) => {
        const targetUrl = process.env.ESP32_CONTROL_URL;
        if (!targetUrl) {
            resolve({ ok: false, reason: 'No ESP32_CONTROL_URL configured' });
            return;
        }

        const url = new URL(targetUrl);
        const payload = JSON.stringify({ action });
        const client = url.protocol === 'https:' ? https : http;

        const request = client.request(
            {
                hostname: url.hostname,
                port: url.port || (url.protocol === 'https:' ? 443 : 80),
                path: url.pathname + (url.search || ''),
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Content-Length': Buffer.byteLength(payload),
                },
            },
            (response) => {
                let body = '';
                response.on('data', (chunk) => {
                    body += chunk;
                });
                response.on('end', () => {
                    resolve({ ok: response.statusCode >= 200 && response.statusCode < 300, statusCode: response.statusCode, body });
                });
            }
        );

        request.on('error', () => {
            resolve({ ok: false, reason: 'ESP32 request failed' });
        });

        request.write(payload);
        request.end();
    });
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
        return {
            statusCode: 200,
            headers,
            body: JSON.stringify(readState()),
        };
    }

    if (event.httpMethod === 'POST') {
        let payload = {};

        try {
            payload = event.body ? JSON.parse(event.body) : {};
        } catch (error) {
            payload = {};
        }

        const action = payload.action || event.queryStringParameters?.action || 'status';
        const state = writeState(action);
        const forwarded = await forwardToDevice(action);

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
