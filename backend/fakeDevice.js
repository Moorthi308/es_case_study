const EventEmitter = require('node:events');

class FakeDevice extends EventEmitter {
  constructor() {
    super();
    this.lastSeen = Date.now();
    this.online = true;
    this.current = null;
    this.sent = [];
  }

  start() {
    this.timer = setInterval(() => { if (this.online) this.lastSeen = Date.now(); }, 1000);
    this.timer.unref();
  }

  send(line) {
    if (!this.online) throw new Error('DEVICE_OFFLINE');
    this.sent.push(line);
    const [command, id, otp] = line.split(':');
    setImmediate(() => {
      if (command === 'PING') return this.emit('line', 'PONG');
      if (command === 'AUTH_START') {
        this.current = { id, otp: '123456', stage: 'waiting_card' };
        return this.emit('line', `ACK:AUTH_START:${id}`);
      }
      if (command === 'AUTH_VERIFY' && this.current?.id === id) {
        if (this.current.stage !== 'waiting_otp') return this.emit('line', 'NAK:NO_ACTIVE');
        if (otp === this.current.otp) {
          this.current = { ...this.current, stage: 'granted' };
          return this.emit('line', `RES:OK:${id}`);
        }
        this.current.attempts = (this.current.attempts || 3) - 1;
        if (this.current.attempts <= 0) {
          this.current = null;
          return this.emit('line', `RES:FAIL:${id}:TOO_MANY`);
        }
        return this.emit('line', `EVT:BAD_OTP:${id}:${this.current.attempts}`);
      }
      if (command === 'AUTH_CANCEL' && this.current?.id === id) {
        this.current = null;
        return this.emit('line', `RES:FAIL:${id}:CANCELLED`);
      }
      if (command === 'DEV_LOCK') {
        this.current = null;
        return this.emit('line', 'EVT:LOCKED');
      }
    });
  }

  simulateCard(authorized = true) {
    if (!this.current) return;
    this.current.stage = authorized ? 'waiting_button' : 'waiting_card';
    this.emit('line', authorized ? `EVT:CARD_OK:${this.current.id}` : `EVT:CARD_BAD:${this.current.id}`);
  }

  simulateButton() {
    if (!this.current || this.current.stage !== 'waiting_button') return;
    this.current.stage = 'waiting_otp';
    this.emit('line', `EVT:OTP_SENT:${this.current.id}`);
  }
}

module.exports = new FakeDevice();
