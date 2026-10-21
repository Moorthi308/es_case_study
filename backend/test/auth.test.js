process.env.NODE_ENV = 'test';
process.env.DEVICE_MODE = 'fake';
const test = require('node:test');
const assert = require('node:assert/strict');
const request = require('supertest');
const app = require('../server');
const authTx = require('../authTx');

function resetDevice() {
  authTx.reset();
  authTx.device.online = true;
  authTx.device.current = null;
  authTx.device.sent.length = 0;
}

test.beforeEach(resetDevice);

test('customer login succeeds without contacting the device', async () => {
  const result = await request(app).post('/api/login').send({ username: 'customer1', password: 'customer-password' });
  assert.equal(result.status, 200);
  assert.equal(result.body.user.role, 'user');
  assert.deepEqual(authTx.device.sent, []);
});

test('wrong admin password is rejected without contacting the device', async () => {
  const result = await request(app).post('/api/login').send({ username: 'admin1', password: 'wrong' });
  assert.equal(result.status, 401);
  assert.deepEqual(authTx.device.sent, []);
});

test('admin happy path requires card, button, and correct OTP', async () => {
  const agent = request.agent(app);
  const login = await agent.post('/api/login').send({ username: 'admin1', password: 'admin-password' });
  assert.equal(login.status, 200);
  assert.equal(login.body.status, 'otp_required');
  const { txId } = login.body;
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateCard();
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateButton();
  await new Promise((resolve) => setImmediate(resolve));
  const status = await agent.get('/api/admin/otp/status');
  assert.equal(status.body.step, 'waiting_otp');
  const verified = await agent.post('/api/admin/otp/verify').send({ otp: '123456' });
  assert.equal(verified.status, 200);
  assert.equal(verified.body.user.role, 'admin');
  assert.ok(authTx.device.sent.includes(`AUTH_START:${txId}`));
});

test('a second admin login always starts a new hardware verification', async () => {
  const agent = request.agent(app);
  await agent.post('/api/login').send({ username: 'admin1', password: 'admin-password' });
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateCard();
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateButton();
  await new Promise((resolve) => setImmediate(resolve));
  const firstVerified = await agent.post('/api/admin/otp/verify').send({ otp: '123456' });
  assert.equal(firstVerified.status, 200);

  const secondLogin = await agent.post('/api/login').send({ username: 'admin1', password: 'admin-password' });
  assert.equal(secondLogin.status, 200);
  assert.equal(secondLogin.body.status, 'otp_required');
  const session = await agent.get('/api/session');
  assert.equal(session.status, 401);
  assert.equal(authTx.device.sent.filter((command) => command.startsWith('AUTH_START:')).length, 2);
});

test('invalid OTP format is rejected before the device', async () => {
  const agent = request.agent(app);
  await agent.post('/api/login').send({ username: 'admin1', password: 'admin-password' });
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateCard();
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateButton();
  await new Promise((resolve) => setImmediate(resolve));
  const before = authTx.device.sent.length;
  const result = await agent.post('/api/admin/otp/verify').send({ otp: '12x' });
  assert.equal(result.status, 400);
  assert.equal(authTx.device.sent.length, before);
});

test('second admin attempt is rejected while the board is busy', async () => {
  const first = await request(app).post('/api/login').send({ username: 'admin1', password: 'admin-password' });
  assert.equal(first.status, 200);
  const second = await request(app).post('/api/login').send({ username: 'admin1', password: 'admin-password' });
  assert.equal(second.status, 409);
});

test('admin logout sends a device lock command', async () => {
  const agent = request.agent(app);
  await agent.post('/api/login').send({ username: 'admin1', password: 'admin-password' });
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateCard();
  await new Promise((resolve) => setImmediate(resolve));
  authTx.device.simulateButton();
  await new Promise((resolve) => setImmediate(resolve));
  await agent.post('/api/admin/otp/verify').send({ otp: '123456' });
  const result = await agent.post('/api/logout');
  assert.equal(result.status, 200);
  assert.ok(authTx.device.sent.includes('DEV_LOCK'));
});
