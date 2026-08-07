const { Client } = require('pg');

const TABLES = new Set(['sensor_readings', 'water_events', 'commands']);

function normalizeKey(key) {
    if (key === 'recordedAt') return 'recorded_at';
    if (key === 'eventType') return 'event_type';
    return key;
}

function getDatabaseConnectionString() {
    return process.env.NETLIFY_DATABASE_URL || process.env.DATABASE_URL || process.env.POSTGRES_URL;
}

async function insertRecord(table, record) {
    const connectionString = getDatabaseConnectionString();
    if (!connectionString) {
        throw new Error('Missing Netlify database connection string. Add NETLIFY_DATABASE_URL or DATABASE_URL to your environment.');
    }

    const client = new Client({
        connectionString,
        ssl: connectionString.includes('sslmode=require') || connectionString.includes('render.com') ? { rejectUnauthorized: false } : undefined,
    });

    await client.connect();

    try {
        const normalizedRecord = {};
        for (const [key, value] of Object.entries(record || {})) {
            normalizedRecord[normalizeKey(key)] = value;
        }

        const columns = Object.keys(normalizedRecord);
        if (!columns.length) {
            throw new Error('Record is empty');
        }

        const placeholders = columns.map((_, index) => `$${index + 1}`).join(', ');
        const columnList = columns.map((column) => `"${column}"`).join(', ');
        const values = columns.map((column) => normalizedRecord[column]);

        const query = `INSERT INTO "${table}" (${columnList}) VALUES (${placeholders}) RETURNING *;`;
        const result = await client.query(query, values);
        return result.rows[0];
    } finally {
        await client.end();
    }
}

async function fetchPendingCommand(status = 'pending') {
    const connectionString = getDatabaseConnectionString();
    if (!connectionString) {
        throw new Error('Missing Netlify database connection string. Add NETLIFY_DATABASE_URL or DATABASE_URL to your environment.');
    }

    const client = new Client({
        connectionString,
        ssl: connectionString.includes('sslmode=require') || connectionString.includes('render.com') ? { rejectUnauthorized: false } : undefined,
    });

    await client.connect();

    try {
        const query = `SELECT * FROM "commands" WHERE "status" = $1 ORDER BY "recorded_at" ASC LIMIT 1;`;
        const result = await client.query(query, [status]);
        return result.rows[0] || null;
    } finally {
        await client.end();
    }
}

async function updateCommandStatus(id, status) {
    const connectionString = getDatabaseConnectionString();
    if (!connectionString) {
        throw new Error('Missing Netlify database connection string. Add NETLIFY_DATABASE_URL or DATABASE_URL to your environment.');
    }

    const client = new Client({
        connectionString,
        ssl: connectionString.includes('sslmode=require') || connectionString.includes('render.com') ? { rejectUnauthorized: false } : undefined,
    });

    await client.connect();

    try {
        const query = `UPDATE "commands" SET "status" = $1 WHERE "id"::text = $2 RETURNING *;`;
        const result = await client.query(query, [status, id]);
        return result.rows[0] || null;
    } finally {
        await client.end();
    }
}

exports.handler = async (event) => {
    const headers = {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Headers': 'Content-Type, Authorization',
        'Access-Control-Allow-Methods': 'GET, POST, PATCH, OPTIONS',
        'Content-Type': 'application/json',
    };

    if (event.httpMethod === 'OPTIONS') {
        return { statusCode: 200, headers, body: '' };
    }

    try {
        if (event.httpMethod === 'GET') {
            const status = event.queryStringParameters?.status || 'pending';
            const command = await fetchPendingCommand(status);
            return {
                statusCode: 200,
                headers,
                body: JSON.stringify(command ? [command] : []),
            };
        }

        if (event.httpMethod === 'PATCH') {
            const id = event.queryStringParameters?.id;
            const payload = event.body ? JSON.parse(event.body) : {};
            const status = payload.status || 'executed';

            if (!id) {
                return { statusCode: 400, headers, body: JSON.stringify({ ok: false, error: 'id is required' }) };
            }

            const updated = await updateCommandStatus(id, status);
            return {
                statusCode: 200,
                headers,
                body: JSON.stringify({ ok: true, row: updated }),
            };
        }

        if (event.httpMethod !== 'POST') {
            return {
                statusCode: 405,
                headers,
                body: JSON.stringify({ ok: false, error: 'Method not allowed' }),
            };
        }

        const payload = event.body ? JSON.parse(event.body) : {};
        const table = payload.table || payload.type;
        const record = payload.record || payload;

        if (!table || !record) {
            return {
                statusCode: 400,
                headers,
                body: JSON.stringify({ ok: false, error: 'table and record are required' }),
            };
        }

        if (!TABLES.has(table)) {
            return {
                statusCode: 400,
                headers,
                body: JSON.stringify({ ok: false, error: 'Unsupported table. Use sensor_readings, water_events, or commands.' }),
            };
        }

        const insertedRow = await insertRecord(table, record);

        return {
            statusCode: 200,
            headers,
            body: JSON.stringify({ ok: true, table, row: insertedRow }),
        };
    } catch (error) {
        return {
            statusCode: 500,
            headers,
            body: JSON.stringify({ ok: false, error: error.message }),
        };
    }
};
