# FIFA World Cup 2026 Live Ticker — Axis ACAP

A native C ACAP application for **Axis C1720 / C1710** network speakers that streams live FIFA World Cup 2026 match data to the built-in LED display. Goals trigger audio clips and a team-color strobe effect. Fully configurable via a web UI served directly from the device.

![Version](https://img.shields.io/badge/version-1.1.0-blue)
![Architecture](https://img.shields.io/badge/arch-aarch64-green)
![SDK](https://img.shields.io/badge/ACAP%20SDK-12.9.0-orange)

---

## Features

- **Live ticker** — cycles match scores, status, and goal events on the C1720/C1710 display
- **Dual API** — primary: [api-football.com](https://api-football.com), fallback: [football-data.org](https://football-data.org); switches automatically on failure
- **Adaptive poll rate** — 30 s during live play, 90 s pre-match, 5 min idle
- **Goal audio** — streams MP3 clips to the speaker via VAPIX `transmit.cgi` with configurable volume (baked into PCM, never changes device output level)
- **Goal strobe** — flashes the display in the scoring team's kit colors
- **Per-team overrides** — custom clip ID and flash count per tracked team
- **Goal event details** — shows scorer name, minute, OG / PEN labels
- **Knockout bracket tab** — live bracket view once the group stage ends
- **Golden Boot tab** — top scorers leaderboard
- **Display preview** — Live tab shows the exact text currently on the device
- **Manual override** — inject a custom message for 15 s – 2 min or until cleared
- **Webhook push** — POSTs a JSON payload to any URL (Home Assistant, Discord, etc.) on goal
- **Demo mode** — runs entirely from local mock JSON files, no API calls
- **Health endpoint** — `GET /health` for monitoring/liveness probes

---

## Requirements

| Item | Detail |
|------|--------|
| Device | Axis C1720 or C1710 network speaker |
| Firmware | AXIS OS with ACAP support (ARTPEC-8, aarch64) |
| Build host | Docker (any OS) |
| API keys | api-football.com free tier (100 req/day) and/or football-data.org free tier |

---

## Build

```bash
# Clone the repo
git clone https://github.com/gscarlet22-design/FIFA-World-Cup-ACAP.git
cd FIFA-World-Cup-ACAP

# Build the .eap package
docker build --tag fifa_wc:1.0.0 v1.0.0/

# Extract the artifact
mkdir -p build
id=$(docker create fifa_wc:1.0.0)
docker cp "$id:/opt/app/." ./build/
docker rm "$id"
ls build/*.eap
```

The output is `build/fifa_wc_1_1_0_aarch64.eap` (or similar). GitHub Actions builds this automatically on every push — grab the artifact from the **Actions** tab if you don't want to build locally.

---

## Installation

1. Open the device web interface: `https://<device-ip>`
2. Go to **Apps → Add app** and upload the `.eap` file
3. The app starts automatically. Open its settings page to configure.

---

## Configuration

The settings page is available at `https://<device-ip>/local/fifa_wc/` or via the **Apps** panel.

### Country Selection
Pick the teams you want to track. Tracked teams get top priority in the ticker queue and trigger audio/strobe on goals.

### API Keys
- **api-football Key** (`X-RapidAPI-Key`) — primary source
- **football-data.org Key** (`X-Auth-Token`) — automatic fallback

Leave both blank to run in **Demo Mode** only.

### Audio Alerts
Click **Upload Clips to Device** once after installation to store the two bundled MP3s (`goal_cheer.mp3`, `goal_horn.mp3`) in the device clip store. Then select which clip plays for goals and alerts from the dropdown — the list is populated live from whatever clips are on the device.

### Goal Strobe
Flashes the display in the scoring team's kit colors. Configurable flash count (2–10). Each flash alternates the team's primary and inverted kit colors.

### Per-Team Overrides
Override the clip ID and flash count for individual teams. Set to `0` to fall back to the global setting.

### Manual Override
Push a custom message to the display for a fixed duration without interrupting the ticker loop. The ticker resumes automatically when the override expires.

### Webhook
On every tracked-team goal, the app POSTs:
```json
{
  "event": "goal",
  "team": "USA",
  "opponent": "ENG",
  "score": "1-0",
  "minute": 67,
  "scorer": "Pulisic"
}
```
Compatible with Home Assistant, Discord bots, Zapier, IFTTT, and any HTTP endpoint.

---

## API Endpoints

All endpoints are available under `/local/fifa_wc/api/` via the VAPIX reverse proxy (admin auth required).

| Method | Path | Description |
|--------|------|-------------|
| GET | `/status` | Live match data, poll mode, app version |
| GET | `/health` | Liveness probe — `{status, version, uptime_sec}` |
| GET | `/config` | Full app configuration |
| POST | `/config` | Save configuration |
| GET | `/standings` | Group standings |
| GET | `/bracket` | Knockout bracket |
| GET | `/scorers` | Top scorers |
| GET | `/clips` | Clips from device clip store `{clips:[{id,name}]}` |
| GET | `/display_state` | Last text on display + override status |
| POST | `/display_override` | Set/clear manual display override |
| POST | `/refresh` | Force immediate API poll |
| POST | `/test_display` | Send a test message to the display |
| POST | `/test_audio` | Play the goal clip |
| POST | `/test_webhook` | Send a test webhook payload |
| POST | `/upload_clips` | Upload bundled audio clips to device clip store |

---

## Demo Mode

Enable **Demo Mode** in the Config tab to run against local mock JSON files with no live API calls. Useful for testing on-device display and audio behavior before the tournament starts. Mock files are at `v1.0.0/app/mock/`.

---

## Project Structure

```
v1.0.0/
├── Dockerfile              # Cross-compile build (acap-native-sdk:12.9.0, aarch64)
└── app/
    ├── main.c              # C backend — HTTP server, polling, display, audio
    ├── manifest.json       # ACAP package manifest (v1.1.0)
    ├── Makefile
    ├── html/
    │   ├── index.html      # Web UI (Live | Standings | Bracket | Golden Boot | Config | Diag)
    │   ├── app.js          # Frontend logic
    │   └── style.css       # Dark theme styles
    ├── audio/
    │   ├── goal_cheer.mp3  # Bundled goal audio clip
    │   └── goal_horn.mp3   # Bundled goal horn clip
    └── mock/               # Demo mode data files
```

---

## Changelog

| Version | Description |
|---------|-------------|
| **v1.1.0** | Health endpoint, display blank on shutdown, CI artifact fix, footer |
| v1.0.9 | Display preview in Live tab, manual display override |
| v1.0.8a | Audio clip dropdowns — named selects populated from device clip store |
| v1.0.8 | Per-team audio clip and strobe flash overrides |
| v1.0.7 | Knockout bracket tab, `norm_round()` for round label normalization |
| v1.0.6 | Goal scorer event parsing — name, minute, OG/PEN labels |
| v1.0.5 | Goal strobe — team kit colors, `display_flash()` |
| v1.0.4 | Adaptive poll rate, goal event webhook push |
| v1.0.3 | Audio pipeline — MP3 → PCM → µ-law → VAPIX transmit.cgi |

---

## License

See [LICENSE](v1.0.0/app/LICENSE).

---

*gscarlet22 design*
