# AGENTS.md

## Cursor Cloud specific instructions

### What this repo is
This repo contains a single static file, `Index.html` — a Korean "통합 관제 대시보드"
(Integrated Monitoring Dashboard) frontend built with plain HTML/CSS/vanilla JS (no
frameworks, no package manager, no build step, no tests, no lint tooling).

### C++ backend (`main.cpp`)
The frontend calls relative endpoints on the same origin:
- `POST /api/events` — send an event `{module, level, type}` (on button click).
- `GET /api/dashboard` — polled every 1 second; returns an array of aggregated rows
  (`firstTime, lastTime, module, level, type, severity, count, message`). Severity is
  computed by the backend (frontend only binds values) and drives row color via
  `row-<severity>` CSS classes (`Catastrophic/Critical/Major/Minor/Normal`).

`main.cpp` is an offline (폐쇄망) C++ HTTP server implementing both endpoints AND serving
`Index.html` on the same origin (default port 8080). It uses only two vendored header-only
libraries in `third_party/` (`httplib.h`, `json.hpp`) — no package manager / internet
needed.

- Build (Linux/macOS): `make` (or `g++ -std=c++17 -O2 -pthread main.cpp -o server`).
- Build (Windows / Visual Studio): use CMake ("Open Folder" in VS, or `cmake -S . -B build`
  then `cmake --build build`). MSVC needs the `/utf-8` flag (already set in `CMakeLists.txt`)
  for the Korean string literals; `httplib.h` auto-links `ws2_32`. The build dir and the
  `server` binary are git-ignored.
- Run: `./server` (optional port arg: `./server 9000`), then open `http://localhost:8080/`.
  The server serves the page at `/` and `/Index.html`, so origin matches the API.
- Escalation window is 10 minutes (600s) per spec. It can be overridden ONLY for testing
  via env var `ALARM_WINDOW_SECONDS` (e.g. `ALARM_WINDOW_SECONDS=2 ./server`) to exercise
  the duration-based escalation quickly; default remains 600.
- State is in-memory only (`std::unordered_map`), so restarting the server clears alarms.

The file is `Index.html` with a capital `I`. To serve the frontend without the backend,
use any static server but open `http://localhost:8000/Index.html` explicitly (a plain
static server won't auto-serve a capital-`I` file at `/`).
