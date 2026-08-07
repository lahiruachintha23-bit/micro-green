const { createClient } = require('@supabase/supabase-js');

function getSupabaseClient() {
    const supabaseUrl = process.env.SUPABASE_URL;
    const supabaseKey = process.env.SUPABASE_ANON_KEY || process.env.SUPABASE_SERVICE_ROLE_KEY;

    if (!supabaseUrl || !supabaseKey) {
        return null;
    }

    return createClient(supabaseUrl, supabaseKey);
}

async function insertRecord(table, record) {
    const supabase = getSupabaseClient();
    if (!supabase) {
        throw new Error('Supabase URL or Key missing in Netlify environment variables');
    }

    const { data, error } = await supabase
        .from(table)
        .insert([record])
        .select();

    if (error) {
        throw new Error(`Supabase insert error: ${error.message}`);
    }

    return data ? data[0] : null;
}

exports.handler = async (event) => {
    const headers = {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Headers': 'Content-Type, Authorization',
        'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
        'Content-Type': 'application/json',
    };

    if (event.httpMethod === 'OPTIONS') {
        return { statusCode: 200, headers, body: '' };
    }

    if (event.httpMethod !== 'POST') {
        return {
            statusCode: 405,
            headers,
            body: JSON.stringify({ ok: false, error: 'Method not allowed' }),
        };
    }

    try {
        const payload = event.body ? JSON.parse(event.body) : {};
        const { table, record } = payload;

        if (!table || !record) {
            return {
                statusCode: 400,
                headers,
                body: JSON.stringify({ ok: false, error: 'table and record are required' }),
            };
        }

        const allowedTables = ['sensor_readings', 'water_events', 'commands'];
        if (!allowedTables.includes(table)) {
            return {
                statusCode: 400,
                headers,
                body: JSON.stringify({ ok: false, error: `Invalid table. Allowed: ${allowedTables.join(', ')}` }),
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

exports.insertRecord = insertRecord;
