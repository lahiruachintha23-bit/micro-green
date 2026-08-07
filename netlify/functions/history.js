const { createClient } = require('@supabase/supabase-js');

function getSupabaseClient() {
    const supabaseUrl = process.env.SUPABASE_URL;
    const supabaseKey = process.env.SUPABASE_ANON_KEY || process.env.SUPABASE_SERVICE_ROLE_KEY;

    if (!supabaseUrl || !supabaseKey) {
        return null;
    }

    return createClient(supabaseUrl, supabaseKey);
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

    const supabase = getSupabaseClient();
    if (!supabase) {
        return {
            statusCode: 200,
            headers,
            body: JSON.stringify([]),
        };
    }

    try {
        if (type === 'height' || type === 'sensor') {
            const { data, error } = await supabase
                .from('sensor_readings')
                .select('recorded_at, height, stage, temperature, humidity, soil, flow')
                .order('recorded_at', { ascending: false })
                .limit(limit);

            if (error) throw error;

            // Map to dashboard expected format
            const formatted = (data || []).reverse().map((row) => ({
                time: Math.floor(new Date(row.recorded_at).getTime() / 1000),
                height: row.height || 0,
                stage: row.stage || 'Unknown',
                temp: row.temperature,
                hum: row.humidity,
                soil: row.soil,
                flow: row.flow
            }));

            return {
                statusCode: 200,
                headers,
                body: JSON.stringify(formatted),
            };
        }

        if (type === 'water') {
            const { data, error } = await supabase
                .from('water_events')
                .select('recorded_at, event_type, details')
                .order('recorded_at', { ascending: false })
                .limit(limit);

            if (error) throw error;

            const formatted = (data || []).reverse().map((row) => ({
                time: Math.floor(new Date(row.recorded_at).getTime() / 1000),
                type: row.event_type,
                details: row.details
            }));

            return {
                statusCode: 200,
                headers,
                body: JSON.stringify(formatted),
            };
        }

        return {
            statusCode: 400,
            headers,
            body: JSON.stringify({ error: 'Invalid type parameter. Use type=height or type=water' }),
        };
    } catch (err) {
        console.error('History fetch error:', err.message);
        return {
            statusCode: 200,
            headers,
            body: JSON.stringify([]),
        };
    }
};
