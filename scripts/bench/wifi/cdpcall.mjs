// node cdpcall.mjs <port> <method> <json params>
const [port, method, params] = process.argv.slice(2);
const tabs = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
let t = tabs.find((x) => x.type === 'page') || await (await fetch(`http://127.0.0.1:${port}/json/new`, { method: 'PUT' })).json();
const ws = new WebSocket(t.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
ws.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (m.id === 1) { console.log(JSON.stringify(m.result ?? m.error)); process.exit(0); } });
ws.send(JSON.stringify({ id: 1, method, params: JSON.parse(params || '{}') }));
