const { EventEmitter } = require('node:events');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

class DeviceLink extends EventEmitter {
  constructor(path, baudRate = 9600) {
    super(); this.path = path; this.baudRate = baudRate; this.port = null;
    this.lastSeen = 0; this.opening = false; this.retryTimer = null; this.heartbeatTimer = null;
  }
  get online() { return Boolean(this.port?.isOpen && Date.now() - this.lastSeen < 12000); }
  start() {
    if (this.heartbeatTimer) return; this.open();
    this.heartbeatTimer = setInterval(() => { if (this.port?.isOpen) { try { this.port.write('PING\r\n'); } catch { this.emit('offline'); } } }, 4000);
    this.heartbeatTimer.unref();
  }
  scheduleRetry() {
    if (this.retryTimer) return;
    this.retryTimer = setTimeout(() => { this.retryTimer = null; this.open(); }, 3000);
    this.retryTimer.unref();
  }
  open() {
    if (this.opening || this.port?.isOpen) return;
    if (!this.path) { this.emit('error', new Error('SERIAL_PATH is not configured')); return; }
    this.opening = true;
    const port = new SerialPort({ path: this.path, baudRate: this.baudRate, autoOpen: false });
    // parser-readline expects a string/Buffer delimiter; trim() removes CR.
    const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
    this.port = port; parser.on('data', (line) => this.onLine(line));
    port.on('error', (error) => { this.emit('error', error); this.scheduleRetry(); });
    port.on('close', () => { this.opening = false; this.emit('offline'); this.scheduleRetry(); });
    port.open((error) => { this.opening = false; if (error) { this.emit('error', error); this.scheduleRetry(); } });
  }
  send(line) {
    if (!this.port?.isOpen) throw new Error('DEVICE_OFFLINE');
    if (typeof line !== 'string' || !line || line.length > 48 || /[\r\n]/.test(line)) throw new Error('BAD_COMMAND');
    this.port.write(`${line}\r\n`);
  }
  onLine(line) {
    const clean = String(line).trim(); if (!clean || clean.length > 48) return;
    this.lastSeen = Date.now(); this.emit('line', clean);
  }
}

module.exports = new DeviceLink(process.env.SERIAL_PATH, Number(process.env.SERIAL_BAUD_RATE || 9600));
