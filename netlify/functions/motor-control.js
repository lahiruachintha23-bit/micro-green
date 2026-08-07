// netlify/functions/motor-control.js
// Handles GET (status) and POST (write command to Firebase /commands)

const FIREBASE_DB_URL = process.env.FIREBASE_DATABASE_URL;

if (!FIREBASE_DB_URL) {
    console.error('Missing FIREBASE_DATABASE_URL environment variable');
}

// ---------- Firebase Helpers ----------
async function writeCommandToFirebase(action) {
    if (!FIREBASE_DB_URL) {
        throw new Error('Firebase URL not configured');
    }
    const response = await fetch(`${FIREBASE_DB_URL}/commands.json`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
            action: action,
            status: 'pending',
            timestamp: Date.now()
        })
    });
    if (!response.ok) {
        throw new Error(`Firebase write failed: ${response.status}`);
    }
    const data = await response.json();
    return { commandId: data.name }; // Firebase push key
}

async function getLatestSensorReading() {
    if (!FIREBASE_DB_URL) return null;
    const response = await fetch(
        `${FIREBASE_DB_URL}/sensorReadings.json?orderBy="$key"&limitToLast=1`
    );
    if (!response.ok) return null;
    const data = await response.json();
    if (!data) return null;
    const keys = Object.keys(data);
    const lastKey = keys[0];
    return { id: lastKey, ...data[lastKey] };
}

// ---------- Main Handler ----------
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
        // ---------- GET: return status and latest telemetry ----------
        if (event.httpMethod === 'GET') {
            const lastReading = await getLatestSensorReading();
            let deviceReachable = false;
            let connectionStatus = 'offline';
            let cloudStorageActive = false;

            if (lastReading && lastReading.timestamp) {
                const ageMs = Date.now() - lastReading.timestamp;
                if (ageMs < 60000) {
                    deviceReachable = true;
                    connectionStatus = 'online';
                    cloudStorageActive = true;
                } else if (ageMs < 300000) {
                    connectionStatus = 'stale';
                }
            }

            // Build telemetry object
            const telemetry = {
                distance: lastReading?.height,
                soilValue: lastReading?.soil,
                flowValue: lastReading?.flow,
                temperature: lastReading?.temperature,
                humidity: lastReading?.humidity,
                growthStage: lastReading?.stage,
                recordedAt: lastReading?.timestamp ? new Date(lastReading.timestamp).toISOString() : null,
                daysSinceGermination: lastReading?.daysSinceGermination || 0,
                pumpMode: lastReading?.pumpMode || 'Auto',
                pumpState: lastReading?.pumpState || 'OFF',
                motorStatus: lastReading?.motorStatus || 'OFF',
                fansActive: lastReading?.fansActive || false,
                fanMode: lastReading?.fanMode || 'Auto',
                misterActive: lastReading?.misterActive || false,
                misterMode: lastReading?.misterMode || 'Auto',
                floatSwitch: lastReading?.floatSwitch || false,
                wifiConnected: lastReading?.wifiConnected || false,
                wifiMode: lastReading?.wifiMode || 'STA',
                deviceIp: lastReading?.deviceIp || '--',
            };

            return {
                statusCode: 200,
                headers,
                body: JSON.stringify({
                    deviceReachable,
                    connectionStatus,
                    cloudStorageActive,
                    source: 'firebase-db',
                    ...telemetry,
                    health: {
                        connected: deviceReachable,
                        reason: deviceReachable
                            ? 'ESP32 is pushing live data to Firebase'
                            : lastReading
                                ? `Last reading: ${new Date(lastReading.timestamp).toISOString()}`
                                : 'No readings in Firebase yet — ensure ESP32 is powered on and connected to WiFi',
                    }
                }),
            };
        }

        // ---------- POST: write a command to Firebase ----------
        if (event.httpMethod === 'POST') {
            let payload = {};
            try { payload = event.body ? JSON.parse(event.body) : {}; } catch { }

            const rawAction = payload.action || event.queryStringParameters?.action;
            const mode = payload.mode || event.queryStringParameters?.mode;
            const target = payload.target || event.queryStringParameters?.target;

            // Map dashboard actions to Firebase command strings (same as ESP32 expects)
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
                commandAction = mode === 'ManualOn' ? 'pump_on'
                    : mode === 'ManualOff' ? 'pump_off'
                        : 'pump_auto';
            } else if (rawAction === 'on') {
                commandAction = 'motor_on';
            } else if (rawAction === 'off') {
                commandAction = 'motor_off';
            } else if (rawAction === 'reset') {
                commandAction = 'motor_reset';
            }

            const result = await writeCommandToFirebase(commandAction);

            return {
                statusCode: 200,
                headers,
                body: JSON.stringify({
                    ok: true,
                    commandId: result.commandId,
                    commandAction,
                    status: 'pending',
                    note: 'Command queued in Firebase. ESP32 will execute within 5 seconds.',
                    source: 'firebase-command-queue'
                }),
            };
        }

        return { statusCode: 405, headers, body: 'Method Not Allowed' };
    } catch (error) {
        console.error('[motor-control] Error:', error.message);
        return {
            statusCode: 500,
            headers,
            body: JSON.stringify({ ok: false, error: error.message }),
        };
    }
};