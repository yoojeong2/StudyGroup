# AGENTS.md

## Cursor Cloud specific instructions

### What this repo is
This repo contains a single static file, `Index.html` — a Korean "통합 관제 대시보드"
(Integrated Monitoring Dashboard) frontend built with plain HTML/CSS/vanilla JS (no
frameworks, no package manager, no build step, no tests, no lint tooling).

### Running the frontend (dev)
There is nothing to install or build. Serve the file with any static server, e.g.
`python3 -m http.server 8000` from the repo root.

- The file is `Index.html` with a capital `I`. On Linux (case-sensitive) a plain static
  server will NOT auto-serve it at `/`; open `http://localhost:8000/Index.html` explicitly.

### Backend dependency (important gotcha)
The frontend calls relative endpoints on the same origin:
- `POST /api/events` — send an event `{module, level, type}` (on button click).
- `GET /api/dashboard` — polled every 1 second; returns an array of aggregated rows
  (`firstTime, lastTime, module, level, type, severity, count, message`). Severity is
  computed by the backend (frontend only binds values) and drives row color via
  `row-<severity>` CSS classes (`Catastrophic/Critical/Major/Minor/Normal`).

The backend that serves these endpoints (referenced as a C++ server in code comments) is
**NOT part of this repo**. With only a static file server, the page renders fine but the
table stays empty and the console logs fetch errors. For true end-to-end testing you must
run a backend that implements those two endpoints on the same origin (or reverse-proxy it).
