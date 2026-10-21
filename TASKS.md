# STM32 Hardware OTP Login — Execution Checklist

## Phase 0 — Preconditions

- [x] Confirm the project workspace and runtime tooling.
- [ ] Confirm the STM32 board model and USB-to-UART module.
- [ ] Verify COM-port loopback and 3.3 V wiring.
- [ ] Obtain or add the STM32 firmware source.

## Phase 1 — Backend prototype with fake device

- [x] Create Node.js project structure and environment template.
- [x] Add users, password hashing, sessions, security headers, body limits, and rate limiting.
- [x] Add a fake device with the same interface as the real serial device.
- [x] Add the single-admin transaction state machine and transaction IDs.
- [x] Add login, session, OTP status, verify, cancel, and logout routes.
- [x] Add automated backend tests for customer/admin and failure paths.

## Phase 2 — React frontend

- [x] Add Vite React app and API proxy.
- [x] Add login, OTP status, success, and protected-route flows.
- [x] Add accessible status/error/loading states.

## Phase 3 — Firmware

- [ ] Integrate USART1 protocol framing and interrupt receive path.
- [ ] Integrate the board state machine, timeouts, strict OTP validation, and lock behavior.
- [ ] Validate USART1 and USART2/HC-05 together on the confirmed board.

## Phase 4 — Real-device integration and hardening

- [x] Replace the fake device with a selectable `serialport` implementation.
- [ ] Run the full end-to-end test matrix.
- [ ] Add deployment/security limitations report and demo script.

## Current gate

The software prototype can be completed without hardware. Firmware and real serial integration remain gated until the board model, firmware source, and COM device are supplied.
