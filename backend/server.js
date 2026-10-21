require('dotenv').config();
const express = require('express');
const session = require('express-session');
const bcrypt = require('bcryptjs');
const helmet = require('helmet');
const rateLimit = require('express-rate-limit');
const users = require('./users');
const authTx = require('./authTx');

const app = express();
app.use(helmet());
app.use(express.json({ limit: '1kb' }));
app.use(session({ secret: process.env.SESSION_SECRET || 'development-only-secret', resave: false, saveUninitialized: false, cookie: { httpOnly: true, sameSite: 'strict', secure: false, maxAge: 30 * 60 * 1000 } }));
authTx.device.on('error', (error) => {
  if (process.env.NODE_ENV !== 'test') console.error(`[device] ${error.message}`);
});
authTx.device.start();
const loginLimiter = rateLimit({ windowMs: 15 * 60 * 1000, limit: 20, standardHeaders: true, legacyHeaders: false });
const verifyLimiter = rateLimit({ windowMs: 15 * 60 * 1000, limit: 30, standardHeaders: true, legacyHeaders: false });
const requireAuth = (req, res, next) => req.session.user ? next() : res.status(401).json({ error: 'unauthorized' });
const requireAdmin = (req, res, next) => req.session.user?.role === 'admin' ? next() : res.status(req.session.user ? 403 : 401).json({ error: req.session.user ? 'forbidden' : 'unauthorized' });
const requirePendingAdmin = (req, res, next) => req.session.pending ? next() : res.status(401).json({ error: 'unauthorized' });
const establish = (req, res, user) => req.session.regenerate(() => { req.session.user = { id: user.id, username: user.username, role: user.role }; res.json({ status: 'authenticated', user: req.session.user }); });

app.post('/api/login', loginLimiter, async (req, res) => {
  const { username, password } = req.body || {};
  if (typeof username !== 'string' || typeof password !== 'string' || username.length > 40 || password.length > 100) return res.status(400).json({ error: 'bad_request' });
  const user = users.find((item) => item.username === username.toLowerCase());
  const ok = await bcrypt.compare(password, user?.passwordHash || bcrypt.hashSync('dummy', 10));
  if (!user || !ok) return res.status(401).json({ error: 'invalid_credentials' });
  if (user.role === 'user') return establish(req, res, user);
  try { const txId = authTx.begin(user.username); req.session.pending = { userId: user.id, txId }; res.json({ status: 'otp_required', txId }); }
  catch (error) { const mapped = { DEVICE_OFFLINE: [503, 'device_offline'], BUSY: [409, 'board_busy'] }[error.message] || [500, 'server_error']; res.status(mapped[0]).json({ error: mapped[1] }); }
});

app.get('/api/admin/otp/status', requirePendingAdmin, (req, res) => res.json(authTx.status(req.session.pending.txId)));
app.post('/api/admin/otp/verify', verifyLimiter, requirePendingAdmin, async (req, res) => {
  if (!req.session.pending) return res.status(401).json({ error: 'unauthorized' });
  try { const result = await authTx.verify(req.session.pending.txId, req.body?.otp); if (result.ok) { const user = users.find((item) => item.id === req.session.pending.userId); return establish(req, res, user); } return res.status(result.reason === 'bad_otp' ? 401 : 410).json(result); }
  catch (error) { return res.status(error.message === 'BAD_FORMAT' ? 400 : 410).json({ error: error.message.toLowerCase() }); }
});
app.post('/api/admin/otp/cancel', requirePendingAdmin, (req, res) => { authTx.cancel(req.session.pending.txId); delete req.session.pending; res.json({ status: 'cancelled' }); });
app.get('/api/session', (req, res) => req.session.user ? res.json({ user: req.session.user }) : res.status(401).json({ error: 'unauthorized' }));
app.post('/api/logout', requireAuth, (req, res) => { if (req.session.user.role === 'admin') { try { authTx.device.send('DEV_LOCK'); } catch {} } req.session.destroy(() => res.json({ status: 'logged_out' })); });

if (require.main === module) app.listen(process.env.PORT || 3000, () => console.log(`Backend listening on ${process.env.PORT || 3000}`));
module.exports = app;
