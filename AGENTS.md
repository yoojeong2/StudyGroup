# AGENTS.md

## Cursor Cloud specific instructions

### What this repo is
This repo contains a static frontend `index.html` — a Korean "통합 관제 대시보드"
(Integrated Monitoring Dashboard) built with plain HTML/CSS/vanilla JS — and a C++ backend
`main.cpp` that serves it and its two APIs. No package manager or frontend build step; no
frontend tests or lint tooling.

### C++ backend (`main.cpp`)
All logic (severity, timing, throttling, pairing) lives in the backend. It serves
`index.html` and three relative same-origin endpoints:
- `POST /api/events` — receive `{module, type, level, message}`.
- `GET /api/dashboard` — active alarms only (`Open`/`In Progress`), newest-first; each row is
  `{eventId, module, type, level, severity, status, count, firstTime, lastTime, message}`.
  `severity` (`Critical/Major/Minor/Normal`) drives row color via `row-<severity>` CSS.
- `PUT /api/events/{eventId}/status` — body `{status: "In Progress" | "Closed"}`; `Closed`
  removes the alarm from the active map immediately.

Alarm model (`std::unordered_map`, Active Key = `module + "|" + type`, mutex-guarded):
- Throttling: same-key events within `THROTTLE_MS` (1000ms) only bump `count` and skip the
  heavy escalation path.
- Case A — fault-clear pairing: an `INFO` event of a fault type closes (removes) active
  error-level alarms (`ERROR`/`WARN`) in the same `module`, then registers itself as a new
  `Open`/`Normal` alarm.
- Case B — `ERROR`/`WARN`: new → initial severity (`네트워크 단절`/`프로세스 다운` ERROR = `Major`,
  `리소스 임계치 초과` WARN = `Minor`, else `Normal`); existing → `count++`, `lastTime`/`message`
  update, then time/count escalation.
- Escalation thresholds are top-of-file `const` constants, overridable via env vars for
  testing: `ESC_NET_DISCONNECT_SEC`(180), `ESC_PROCESS_DOWN_SEC`(60), `ESC_PROCESS_DOWN_COUNT`(3),
  `ESC_RESOURCE_SEC`(300), `THROTTLE_MS`(1000). e.g. `ESC_NET_DISCONNECT_SEC=2 ./server`.

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
