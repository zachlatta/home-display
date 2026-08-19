# Zach / Now — reTerminal E1002 dashboard

A live, six-color personal dashboard for the 7.3-inch reTerminal E1002. Its design is grounded in [a jobs-to-be-done brief](DESIGN.md). It turns data from the Personal Data Warehouse (PDW) into a glanceable e-paper view of:

- what actually happened today, spread across the whole day rather than the last burst
- a recent photo from the private photo archive, dithered for the six-color panel
- the single next calendar transition

The display refreshes every 30 minutes and deep-sleeps between updates. Failed updates retry after five minutes.

## Architecture

```text
PDW API ──Bearer/HTTPS──> Node gateway ─┬─ HTTPS ──────────────> E1002
        (same host)                     └─ scoped USB bridge ──> E1002
                                        └─ 5-min cache + 24-hour last-good
```

The gateway is deployed as a container next to PDW, so the display does not
depend on any workstation being awake.

The display payload is protocol 5. The gateway, the serial bridge, and the firmware all pin that number and the device refuses anything else, so they are deployed together.

The device never receives the general PDW credential or arbitrary SQL capability. `server/dashboard-query.sql` is the only query, the gateway reduces its result to a versioned display payload, and the device authenticates with a separate random bearer token. The gateway fetches candidate photos through short-lived PDW object URLs and keeps the newest one that survives reduction to the panel's six-color palette, exposing neither storage references nor signed URLs. Health and financial data are not queried or returned. The public route is HTTPS and the firmware validates it against the embedded ISRG Root X1 CA; TLS verification is never disabled.

## Repository

- `src/main.cpp` — ESP32-S3 networking, transports, provisioning, and deep sleep
- `src/dashboard_view.h` — payload parsing and e-paper layout, shared with the simulator
- `src/isrg-root-x1.h` — public CA certificate used for TLS verification
- `sim/` — host simulator that renders the panel without hardware ([details](sim/README.md))
- `server/dashboard.mjs` — authenticated, cached PDW gateway
- `server/serial_bridge.py` — local USB fallback when provisioned Wi-Fi is unavailable
- `server/dashboard-query.sql` — fixed aggregate dashboard query
- `test/dashboard.test.mjs` — payload and HTTP security tests
- `platformio.ini` — reTerminal E1002 build and upload configuration

Secrets and runtime state live in ignored, mode-`0600` files under `.secrets/`, `.state/`, and `include/secrets.h`.

## Deploy

The gateway is a container. `server/` is the only thing in the image — the
firmware and simulator are build-time tooling.

```sh
docker build -t home-display .
docker run --rm -p 8787:8787 \
  -e PDW_API_URL=https://pdw.example.com \
  -e PDW_SECRET_TOKEN=... \
  -e HOME_DISPLAY_TOKEN=... \
  -v home-display-state:/data \
  home-display
```

All configuration is environment variables; nothing is read from disk in
production.

| Variable | Purpose |
| --- | --- |
| `PDW_API_URL` | PDW base URL. Falls back to `~/.config/pdw-cli/config.json` for local development. |
| `PDW_SECRET_TOKEN` | PDW API token, minimum 32 characters. |
| `HOME_DISPLAY_TOKEN` | Bearer token the device presents. Falls back to `.secrets/dashboard-token`. |
| `HOME_DISPLAY_HOST` / `HOME_DISPLAY_PORT` | Bind address, default `0.0.0.0:8787` in the image. |
| `HOME_DISPLAY_CACHE_FILE` | Last-good payload path, default `/data/dashboard-cache.json`. |

Mount a volume at `/data` so the 24-hour last-good payload survives a redeploy;
without it a restart during a PDW outage costs the display its fallback. A cache
write failure is logged and otherwise ignored — it never fails a request.

`GET /health` is unauthenticated and discloses nothing, for container health
checks. `GET /api/dashboard` requires the bearer token.

## Develop and verify

Requires Node 22+, PlatformIO, and a configured `pdw` CLI.

```sh
node --test
pio run -e reterminal_e1002
```

Render the panel without hardware. The simulator compiles `src/dashboard_view.h`
— the same parser and layout the firmware runs — against a GxEPD2-equivalent
paged framebuffer, then writes both a flat PNG and a simulated view of the
physical e-paper:

```sh
make -C sim fixtures
make -C sim check
```

Run the gateway locally:

```sh
HOME_DISPLAY_HOST=127.0.0.1 HOME_DISPLAY_PORT=8787 \
  node server/dashboard.mjs
```

The serial bridge (`server/serial_bridge.py`) runs on whichever machine the
display is plugged into and is only used when Wi-Fi is unavailable. The
gateway's unauthenticated health check is:

```sh
curl http://127.0.0.1:8787/health
```

## Flash and validate the device

Create `include/secrets.h` locally (never commit it) with `WIFI_SSID`, `WIFI_PASSWORD`, `DASHBOARD_TOKEN`, `DASHBOARD_URL`, and `SETUP_PASSWORD`, then:

```sh
pio run -e reterminal_e1002 -t upload
pio device monitor -b 115200 -p /dev/cu.usbserial-10
```

A successful end-to-end update emits:

```text
WIFI_CONNECTED: …
DASHBOARD_FETCH_OK: timeline=…
DASHBOARD_RENDER_COMPLETE
SLEEP_SECONDS: 1800
```

Verified HTTPS Wi-Fi is the primary transport. While the display remains connected over USB,
the scoped serial bridge supplies the same versioned payload if Wi-Fi authentication is
unavailable; it holds no general PDW credential. If neither transport works, the screen shows a
temporary `Zach-Display-Setup` provisioning network and the local setup address. The setup AP
expires automatically after five minutes.
