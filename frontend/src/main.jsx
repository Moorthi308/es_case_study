import React, { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { api } from './api';
import './style.css';

const messages = { starting: 'Starting hardware verification…', waiting_card: 'Tap your RFID card on the reader', waiting_button: 'Card accepted. Press the button on the board', waiting_otp: 'OTP sent to your phone via Bluetooth', verifying: 'Checking with the device…', failed: 'Verification failed' };

function Login({ go }) {
  const [form, setForm] = useState({ username: '', password: '' }); const [error, setError] = useState(''); const [busy, setBusy] = useState(false);
  async function submit(event) { event.preventDefault(); setBusy(true); setError(''); const result = await api('/api/login', { method: 'POST', body: form }); setBusy(false); if (result.ok) return go(result.data.status === 'otp_required' ? '/otp' : '/success'); const map = { invalid_credentials: 'Invalid username or password', device_offline: 'Admin login needs the security device connected', board_busy: 'Another admin is authenticating, try again shortly' }; setError(map[result.data.error] || 'Unable to log in'); }
  return <main className="card"><h1>Secure sign in</h1><p className="muted">Use your account credentials to continue.</p><form onSubmit={submit}><label>Username<input required value={form.username} onChange={(e) => setForm({ ...form, username: e.target.value })} /></label><label>Password<input required type="password" value={form.password} onChange={(e) => setForm({ ...form, password: e.target.value })} /></label>{error && <p className="error">{error}</p>}<button disabled={busy}>{busy ? 'Signing in…' : 'Sign in'}</button></form><p className="hint">Demo accounts: customer1 / customer-password, admin1 / admin-password</p></main>;
}

function Otp({ go }) {
  const [status, setStatus] = useState({ step: 'starting' }); const [otp, setOtp] = useState(''); const [error, setError] = useState('');
  useEffect(() => { let live = true; const poll = async () => { const result = await api('/api/admin/otp/status'); if (live && result.ok) setStatus(result.data); else if (!result.ok) go('/login'); }; poll(); const timer = setInterval(poll, 1000); return () => { live = false; clearInterval(timer); }; }, [go]);
  async function verify(event) { event.preventDefault(); setError(''); const result = await api('/api/admin/otp/verify', { method: 'POST', body: { otp } }); if (result.ok) go('/success'); else setError(result.data.error === 'bad_otp' ? `Wrong OTP. Attempts left: ${result.data.attemptsLeft}` : result.data.error || 'Verification failed'); }
  async function cancel() { await api('/api/admin/otp/cancel', { method: 'POST' }); go('/login'); }
  return <main className="card"><h1>Hardware verification</h1><div className="steps"><span className={status.step !== 'starting' ? 'done' : 'active'}>1. Card</span><span className={['waiting_button', 'waiting_otp', 'verifying'].includes(status.step) ? 'active' : ''}>2. Button</span><span className={['waiting_otp', 'verifying'].includes(status.step) ? 'active' : ''}>3. OTP</span></div><p>{messages[status.step] || status.reason || 'Verification ended.'}</p>{status.step === 'waiting_otp' && <form onSubmit={verify}><label>Six-digit OTP<input autoFocus inputMode="numeric" pattern="[0-9]{6}" maxLength="6" value={otp} onChange={(e) => setOtp(e.target.value.replace(/\D/g, ''))} /></label><button disabled={otp.length !== 6}>Verify</button></form>}{error && <p className="error">{error}</p>}<button className="secondary" onClick={cancel}>Cancel</button></main>;
}

function Success({ go }) { const [user, setUser] = useState(null); useEffect(() => { api('/api/session').then((r) => r.ok ? setUser(r.data.user) : go('/login')); }, [go]); if (!user) return <main className="card"><p>Loading session…</p></main>; return <main className="card"><h1>Access granted</h1><p>Logged in as <strong>{user.username}</strong> ({user.role})</p>{user.role === 'admin' && <p className="badge">✓ Verified with hardware OTP</p>}<button onClick={async () => { await api('/api/logout', { method: 'POST' }); go('/login'); }}>Log out</button></main>; }

function App() { const [path, setPath] = useState(window.location.pathname); const go = (next) => { window.history.pushState({}, '', next); setPath(next); }; useEffect(() => { const handler = () => setPath(window.location.pathname); window.addEventListener('popstate', handler); return () => window.removeEventListener('popstate', handler); }, []); return <div className="shell"><header><span>STM32 Secure Login</span></header>{path === '/otp' ? <Otp go={go} /> : path === '/success' ? <Success go={go} /> : <Login go={go} />}</div>; }
createRoot(document.getElementById('root')).render(<App />);
