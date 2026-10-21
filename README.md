# STM32 Hardware OTP Login Prototype

This repository contains the software prototype for a React + Node.js login flow backed by a single-admin STM32 hardware OTP transaction.

## Run

```powershell
npm install
npm install --prefix frontend
Copy-Item .env.example .env
npm run dev
```

Open `http://localhost:5173`.

Demo credentials:

- Customer: `customer1` / `customer-password`
- Admin: `admin1` / `admin-password`

The current backend uses `backend/fakeDevice.js` so the application and automated tests can run without a board. The fake device uses `123456` as its OTP and the test suite drives card/button events directly.

## Use the provided STM32 firmware

1. Compile and flash the firmware with `WEB_MODE=1`.
2. Connect the USB-UART adapter to USART1: adapter TX → PA10, adapter RX → PA9, and common GND. Use 3.3 V logic.
3. Copy `.env.example` to `.env`, set `DEVICE_MODE=serial`, and set `SERIAL_PATH` to the actual COM port, such as `COM5`.
4. Keep `SERIAL_BAUD_RATE=9600`; this matches the firmware.
5. Start with `npm run dev`.

The real adapter sends `PING` and requires a recent `PONG` before an admin login starts. It then sends `AUTH_START`, receives the card/button/OTP events, and sends `AUTH_VERIFY` over USART1. The OTP itself remains on the HC-05 Bluetooth link.

## Validate

```powershell
npm test
npm run build
```

## Hardware gate

The USART1 firmware work is not applied because this workspace contains no STM32 source and the board/module model is unconfirmed. See [TASKS.md](./TASKS.md) for the remaining hardware and real-device tasks.
