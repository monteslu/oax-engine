#!/usr/bin/env node
// pause-stack.mjs: attach to a node process started with --inspect=PORT,
// pause it, and print the call stack. Finds where a cart is spinning when
// wc_render never returns (build the cart with --profiling-funcs for names).
//
//   node misc/wasmcart/pause-stack.mjs 9333
const port = process.argv[2] || '9229';
const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const ws = new WebSocket(list[0].webSocketDebuggerUrl);
let id = 0;
const send = (method, params = {}) => ws.send(JSON.stringify({ id: ++id, method, params }));
ws.onopen = () => { send('Debugger.enable'); setTimeout(() => send('Debugger.pause'), 300); };
ws.onmessage = (m) => {
  const msg = JSON.parse(m.data);
  if (msg.method === 'Debugger.paused') {
    for (const f of msg.params.callFrames.slice(0, 40)) console.log(f.functionName || '(anon)', f.url ? f.url.split('/').pop() : '');
    process.exit(0);
  }
};
