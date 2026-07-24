# AGENTS.md

## Cursor Cloud specific instructions

### What this repo is
This repo contains a static frontend `index.html` — a Korean "통합 관제 대시보드"
(Integrated Monitoring Dashboard) built with plain HTML/CSS/vanilla JS — and a C++ backend
`main.cpp` that serves it and its two APIs. No package manager or frontend build step; no
frontend tests or lint tooling.

### C++ backend (`main.cpp`)
The frontend calls relative endpoints on the same origin:
- `POST /api/events` — send an event `{module, level, type}` (on button click).
- `GET /api/dashboard` — polled every 1 second; returns an array of aggregated rows
  (`firstTime, lastTime, module, level, type, severity, count, message`). Severity is
  computed by the backend (frontend only binds values) and drives row color via
  `row-<severity>` CSS classes (`Catastrophic/Critical/Major/Minor/Normal`).

`main.cpp` is an offline (폐쇄망) C++ HTTP server implementing both endpoints AND serving
`index.html` on the same origin (default port 8080). It uses only two vendored header-only
libraries in `third_party/` (`httplib.h`, `json.hpp`) — no package manager / internet
needed.

- Build (Linux/macOS): `make` (or `g++ -std=c++17 -O2 -pthread main.cpp -o server`).
- Build (Windows / Visual Studio): use CMake ("Open Folder" in VS, or `cmake -S . -B build`
  then `cmake --build build`). MSVC needs the `/utf-8` flag (already set in `CMakeLists.txt`)
  for the Korean string literals; `httplib.h` auto-links `ws2_32`. The build dir and the
  `server` binary are git-ignored.
- Run: `./server` (optional port arg: `./server 9000`), then open `http://localhost:8080/`.
  The server serves the page at `/` and `/index.html`, so origin matches the API. Stop it
  with Ctrl+C (SIGINT) / SIGTERM — it shuts down gracefully.
- Escalation window is 10 minutes (600s) per spec. It can be overridden ONLY for testing
  via env var `ALARM_WINDOW_SECONDS` (e.g. `ALARM_WINDOW_SECONDS=2 ./server`) to exercise
  the duration-based escalation quickly; default remains 600.
- State is in-memory only (`std::unordered_map`), so restarting the server clears alarms.
- `occurrence_times` is a `std::deque` (not vector): stale timestamps are pruned via
  `pop_front()` for O(1) removal under event-storm load.

To serve the frontend without the backend, use any static server and open
`http://localhost:8000/index.html`.
