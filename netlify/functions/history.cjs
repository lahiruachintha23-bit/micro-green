// netlify/functions/history.js
// Returns historical data for charts (height and water events) from Firebase

const FIREBASE_DB_URL = process.env.FIREBASE_DATABASE_URL;

async function fetchFirebaseData(path, limit = 50) {
    if (!FIREBASE_DB_URL) return [];
    const response = await fetch(
        `${FIREBASE_DB_URL}${path}.json?orderBy="$key"&limitToLast=${limit}`
    );
    if (!response.ok) return [];
    const data = await response.json();
    if (!data) return [];
    // Firebase returns an object keyed by push IDs; convert to array
    return Object.values(data);
}

exports.handler = async (event) => {
    const headers = {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Headers': 'Content-Type',
        'Access-Control-Allow-Methods': 'GET, OPTIONS',
        'Content-Type': 'application/json',
    };

    if (event.httpMethod === 'OPTIONS') {
        return { statusCode: 200, headers, body: '' };
    }

    const type = event.queryStringParameters?.type || 'height';
    const limit = parseInt(event.queryStringParameters?.limit || '50', 10);

    try {
        if (type === 'height' || type === 'sensor') {
            const entries = await fetchFirebaseData('/sensorReadings', limit);
            // Format for dashboard – reverse to show chronological order
            const formatted = entries.reverse().map(row => ({
                time: row.timestamp ? Math.floor(row.timestamp / 1000) : Math.floor(Date.now() / 1000),
                height: row.height || 0,
                stage: row.stage || 'Unknown',
                temp: row.temperature,
                hum: row.humidity,
                soil: row.soil,
                flow: row.flow
            }));
            return { statusCode: 200, headers, body: JSON.stringify(formatted) };
        }

        if (type === 'water') {
            const entries = await fetchFirebaseData('/waterEvents', limit);
            const formatted = entries.reverse().map(row => ({
                time: row.timestamp ? Math.floor(row.timestamp / 1000) : Math.floor(Date.now() / 1000),
                type: row.event_type || 'pump_on',
                details: row.details || ''
            }));
            return { statusCode: 200, headers, body: JSON.stringify(formatted) };
        }

        return { statusCode: 400, headers, body: JSON.stringify({ error: 'Invalid type' }) };
    } catch (err) {
        console.error('History fetch error:', err.message);
        return { statusCode: 500, headers, body: JSON.stringify([]) };
    }
};