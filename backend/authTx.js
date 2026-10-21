const crypto = require('node:crypto');
const device = process.env.DEVICE_MODE === 'serial' ? require('./device') : require('./fakeDevice');

let tx = null;
const terminal = new Set(['done', 'failed']);
const active = () => tx && !terminal.has(tx.step) && Date.now() < tx.hardExpiry;

function fail(reason) {
  if (!tx || terminal.has(tx.step)) return;
  tx.step = 'failed';
  tx.reason = reason;
  clearTimeout(tx.timer);
  tx.waiter?.({ ok: false, reason, attemptsLeft: tx.attemptsLeft });
  tx.waiter = null;
}

function begin(username) {
  if (active()) throw new Error('BUSY');
  if (!device.online) throw new Error('DEVICE_OFFLINE');
  tx = { id: crypto.randomBytes(2).toString('hex'), username, step: 'starting', attemptsLeft: 3, hardExpiry: Date.now() + 120000, waiter: null };
  device.send(`AUTH_START:${tx.id}`);
  tx.timer = setTimeout(() => fail('device_timeout'), 2000);
  return tx.id;
}

device.on('line', (line) => {
  if (!tx) return;
  const parts = line.split(':');
  const id = parts[0] === 'ACK' ? parts[2] : parts[2] || parts[1];
  if (id && id !== tx.id) return;
  if (line.startsWith('ACK:AUTH_START')) { clearTimeout(tx.timer); tx.step = 'waiting_card'; }
  else if (line.startsWith('EVT:CARD_OK')) tx.step = 'waiting_button';
  else if (line.startsWith('EVT:OTP_SENT')) tx.step = 'waiting_otp';
  else if (line.startsWith('EVT:BAD_OTP')) { tx.step = 'waiting_otp'; tx.attemptsLeft = Number(parts[3]); tx.waiter?.({ ok: false, reason: 'bad_otp', attemptsLeft: tx.attemptsLeft }); tx.waiter = null; }
  else if (line.startsWith('RES:OK')) { tx.step = 'done'; tx.waiter?.({ ok: true }); tx.waiter = null; }
  else if (line.startsWith('RES:FAIL')) fail((parts[3] || 'failed').toLowerCase());
});

function verify(id, otp) {
  if (!active() || tx.id !== id || tx.step !== 'waiting_otp') throw new Error('NO_ACTIVE');
  if (!/^\d{6}$/.test(otp)) throw new Error('BAD_FORMAT');
  tx.step = 'verifying';
  return new Promise((resolve) => { tx.waiter = resolve; tx.timer = setTimeout(() => fail('device_timeout'), 3000); device.send(`AUTH_VERIFY:${id}:${otp}`); });
}

function cancel(id) {
  if (tx?.id === id && active()) { device.send(`AUTH_CANCEL:${id}`); fail('cancelled'); }
}

function status(id) {
  if (!tx || tx.id !== id) return { step: 'none' };
  return { step: tx.step, reason: tx.reason, attemptsLeft: tx.attemptsLeft, secondsLeft: null };
}

function reset() { if (tx) clearTimeout(tx.timer); tx = null; }

module.exports = { begin, verify, cancel, status, reset, device };
