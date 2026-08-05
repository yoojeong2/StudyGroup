# AGENTS.md

## Cursor Cloud specific instructions

### What this repo is
This repo contains a static frontend `index.html` — a Korean "통합 관제 대시보드"
(Integrated Monitoring Dashboard) built with plain HTML/CSS/vanilla JS — and a C++ backend
`main.cpp` that serves it and its two APIs. No package manager or frontend build step; no
frontend tests or lint tooling.

### C++ backend (`main.cpp`)
All logic (severity, timing, pairing) lives in the backend. It serves `index.html` and three
relative same-origin endpoints:
- `POST /api/events` — receive `{module, type, level, message}`.
- `GET /api/dashboard` — active alarms only (`Open`/`In Progress`), newest-first; each row
  includes the 8 required fields `lastTime, module, type, severity, status, count, message,
  eventId` (plus `level`, `firstTime`). `severity` (`Catastrophic/Critical/Major/Minor/Normal`,
  capitalized) drives row color via `row-<severity>` CSS.
- `PUT /api/events/{eventId}/status` — body `{status: "In Progress" | "Closed"}`; `Closed`
  removes the alarm from the active map immediately.

Alarm model (`std::unordered_map`, Active Key = `module + "|" + type`, mutex-guarded; a
background timer thread also touches the map, so all access is under `g_mtx`):
- `eventId` is `MODULE-TYPECODE-NNN` (e.g. `AMCM-NET-001`; type codes NET/PRC/RES/DAT/HW).
- Severity has 5 levels: `Normal < Minor < Major < Critical < Catastrophic` (`bumpSeverity`
  raises one step, capped at `Catastrophic`).
- Fault-clear pairing (5 sets): an `INFO` recovery type closes the active error-level alarm of
  its **paired fault type** in the same `module` (`pairedFaultType()` map, removed from the
  map), then registers itself as a new `Open`/`Normal` alarm. Pairs: 네트워크 단절↔복구,
  프로세스 다운↔정상화, 리소스 임계치 초과↔정상, 데이터 처리 지연↔처리 정상화, 하드웨어 오류↔정상.
- New fault: initial severity by type (`네트워크 단절`/`프로세스 다운`/`하드웨어 오류` = `Major`;
  `리소스 임계치 초과`/`데이터 처리 지연` = `Minor`; else `Normal`). Existing fault re-received:
  `count++`, `lastTime`/`message` update; when `count` reaches `COUNT_ESCALATION_THRESHOLD` it
  bumps severity one step (once). No throttling — every duplicate increments count deterministically.
- Time-based escalation: a background thread scans every `TIMER_SCAN_SEC` and, for any alarm
  active longer than `ESCALATION_TIMEOUT_SEC` since `firstTime`, bumps severity one step (once).
- Top-of-file `const` tunables, env-overridable for testing: `COUNT_ESCALATION_THRESHOLD`(3),
  `ESCALATION_TIMEOUT_SEC`(600), `TIMER_SCAN_SEC`(5). e.g. `ESCALATION_TIMEOUT_SEC=2 TIMER_SCAN_SEC=1 ./server`.

`main.cpp` is an offline (폐쇄망) C++ HTTP server using only two vendored header-only
libraries in `third_party/` (`httplib.h`, `json.hpp`) — no package manager / internet needed.

- Build (Linux/macOS): `make` (or `g++ -std=c++17 -O2 -pthread main.cpp -o server`).
- Build (Windows / Visual Studio): use CMake ("Open Folder" in VS, or `cmake -S . -B build`
  then `cmake --build build`). MSVC needs the `/utf-8` flag (already set in `CMakeLists.txt`)
  for the Korean string literals; `httplib.h` auto-links `ws2_32`. The build dir and the
  `server` binary are git-ignored.
- Run: `./server` (optional port arg: `./server 9000`), then open `http://localhost:8080/`.
  The server serves the page at `/` and `/index.html`, so origin matches the API. Stop it
  with Ctrl+C (SIGINT) / SIGTERM — it shuts down gracefully.
- State is in-memory only (`std::unordered_map`), so restarting the server clears alarms.

To serve the frontend without the backend, use any static server and open
`http://localhost:8000/index.html`.
