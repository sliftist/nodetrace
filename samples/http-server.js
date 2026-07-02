'use strict';
// Basic HTTP request/response sample.  Spins up an http server, hits it from
// the same process a few times, exits.  Exercises async parent-ref chains and
// gives us traceMeta calls on request handlers.
const http = require('http');

function processGreet(name) {
  const greeting = { hello: name.trim(), at: Date.now() };
  process.traceMeta?.('greetLen', greeting.hello.length);
  return greeting;
}

function processPayload(body) {
  const parsed = JSON.parse(body || '{}');
  process.traceMeta?.('op', parsed.op ?? 'noop');
  process.traceMeta?.('items', Array.isArray(parsed.items) ? parsed.items.length : 0);
  if (parsed.op === 'sum' && Array.isArray(parsed.items)) {
    return { total: parsed.items.reduce((a, b) => a + b, 0) };
  }
  return { ok: true, echo: parsed };
}

const server = http.createServer((req, res) => {
  process.traceMeta?.('method', req.method);
  process.traceMeta?.('url', req.url);

  let body = '';
  req.on('data', c => { body += c; });
  req.on('end', () => {
    try {
      let out;
      if (req.url === '/greet') out = processGreet(String(req.headers['x-name'] ?? 'anon'));
      else                       out = processPayload(body);
      const buf = Buffer.from(JSON.stringify(out));
      res.setHeader('content-type', 'application/json');
      res.setHeader('content-length', buf.length);
      res.end(buf);
    } catch (e) {
      process.traceMeta?.('error', e.message);
      res.statusCode = 500;
      res.end(e.message);
    }
  });
});

async function hit(port, path, method, body, headers) {
  return new Promise((resolve, reject) => {
    const req = http.request({ port, path, method, headers }, res => {
      let b = '';
      res.on('data', c => b += c);
      res.on('end', () => resolve({ status: res.statusCode, body: b }));
    });
    req.on('error', reject);
    if (body) req.write(body);
    req.end();
  });
}

async function main() {
  await new Promise(r => server.listen(0, r));
  const port = server.address().port;
  process.traceMeta?.('serverPort', port);
  console.log(`listening on ${port}`);

  for (let i = 0; i < 20; i++) {
    process.traceMeta?.('round', i);
    await hit(port, '/greet', 'GET', null, { 'x-name': `user${i}` });
    await hit(port, '/compute', 'POST', JSON.stringify({
      op: 'sum', items: Array.from({ length: 8 }, (_, k) => i * 10 + k),
    }));
  }

  server.close();
  process.traceMeta?.('done', true);
}

main().catch(e => { console.error(e); process.exit(1); });
