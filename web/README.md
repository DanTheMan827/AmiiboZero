# Amiibo Zero Web Installer

A static Vite/React installer for Amiibo Zero. It talks directly to a Flipper Zero over the browser's **Web Serial API** and does not require a desktop helper application.

## What it installs

| File | Flipper path | Source |
| --- | --- | --- |
| `amiibo_zero.fap` | `/ext/apps/NFC/amiibo_zero.fap` | Release-channel FAP from the same GitHub Actions run |
| `key_retail.bin` | `/ext/apps_data/amiibo_zero/key_retail.bin` | User-selected local file; must be 160 bytes |
| `amiibo.json` | `/ext/apps_data/amiibo_zero/amiibo.json` | `https://dantheman827.github.io/AmiiboData/database/amiibo.json` |
| `games_info.json` | `/ext/apps_data/amiibo_zero/games_info.json` | `https://dantheman827.github.io/AmiiboData/database/games_info.json` |

The two JSON files are fetched at install time, parsed with `JSON.parse()`, and serialized again with `JSON.stringify()` before transfer. That validates the payload as JSON and removes formatting whitespace.

The retail key is never sent to a server. JavaScript reads it locally and writes it only to the connected Flipper Zero over USB serial.

## Flipper transport

The installer mirrors Flipper's official `scripts/flipper/storage.py` behavior over Web Serial:

1. Open the USB CDC serial interface at 115200 baud.
2. Synchronize with the Flipper CLI prompt and verify `device_info` contains `hardware_model`.
3. Create target directories if required.
4. Remove an existing destination file because `storage write_chunk` opens in append mode.
5. For each binary chunk, send `storage write_chunk "<path>" <size>`, wait for `Ready`, then send exactly `<size>` raw bytes.
6. Wait for the CLI prompt and verify the final file size with `storage stat`.

The production site also verifies the bundled FAP byte size and SHA-256 against `release.json` before beginning an install.

Web Serial requires a secure context and a compatible Chromium-based desktop browser such as Chrome or Edge. GitHub Pages provides HTTPS automatically.

## Development

```sh
npm install
npm run dev
```

A local development build will not contain `release.json` or `amiibo_zero.fap` unless you place them in `web/public/`. GitHub Actions injects both from the release-channel FAP artifact before building the production site.

## Production build

```sh
npm install
npm run build
```

Vite uses a relative asset base (`./`) so the static output works from a GitHub Pages repository subpath without hard-coding the repository name. The repository must have GitHub Pages configured to use **GitHub Actions** as its publishing source.
