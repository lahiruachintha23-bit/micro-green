const https = require('https');

function requestJson(url, method, body) {
  return new Promise((resolve, reject) => {
    const parsed = new URL(url);
    const payload = body ? JSON.stringify(body) : null;

    const options = {
      hostname: parsed.hostname,
      port: parsed.port || (parsed.protocol === 'https:' ? 443 : 80),
      path: parsed.pathname + parsed.search,
      method,
      headers: {
        'Content-Type': 'application/json',
        'apikey': process.env.SUPABASE_ANON_KEY || '',
        'Authorization': `Bearer ${process.env.SUPABASE_ANON_KEY || ''}`,
      },
    };

    if (payload) {
      options.headers['Content-Length'] = Buffer.byteLength(payload);
    }

    const client = parsed.protocol === 'https:' ? require('https') : require('http');
    const req = client.request(options, (res) => {
      let raw = '';
      res.on('data', (chunk) => { raw += chunk; });
      res.on('end', () => {
        try {
          resolve({ status: res.statusCode, body: raw ? JSON.parse(raw) : {} });
        } catch (error) {
          resolve({ status: res.statusCode, body: raw });
        }
      });
    });

    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
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

  if (!process.env.SUPABASE_URL || !process.env.SUPABASE_ANON_KEY) {
    return {
      statusCode: 500,
      headers,
      body: JSON.stringify({ ok: false, error: 'Missing SUPABASE_URL or SUPABASE_ANON_KEY' }),
    };
  }

  if (event.httpMethod === 'POST') {
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

      const url = `${process.env.SUPABASE_URL}/rest/v1/${table}`;
      const response = await requestJson(url, 'POST', [record]);

      return {
        statusCode: response.status >= 200 && response.status < 300 ? 200 : response.status,
        headers,
        body: JSON.stringify({ ok: response.status >= 200 && response.status < 300, response: response.body }),
      };
    } catch (error) {
      return {
        statusCode: 500,
        headers,
        body: JSON.stringify({ ok: false, error: error.message }),
      };
    }
  }

  return {
    statusCode: 405,
    headers,
    body: JSON.stringify({ ok: false, error: 'Method not allowed' }),
  };
};
