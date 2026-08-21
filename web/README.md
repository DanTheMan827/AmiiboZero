# Amiibo Zero Web Installer

`web/` is a static **Vite + React + TypeScript** installer for Amiibo Zero. It communicates directly with a Flipper Zero through the browser's **Web Serial API**; there is no desktop bridge and the user's retail key never leaves the browser except over the USB serial connection to the selected Flipper.

## Browser support and manual installation

Direct installation requires Web Serial in a secure context. In practice, use desktop Chrome or Edge over HTTPS (GitHub Pages satisfies this) or localhost.

The release page also exposes a **Download `.fap` manually** link. It points to the exact `amiibo_zero.fap` embedded in that Pages release build, so users without Web Serial can download the same binary and install it with qFlipper or another supported method.

## Install transaction

All network and CPU work completes **before the first upload**. One install operation proceeds as follows:

1. Fetch `release.json`, `amiibo.json`, and `games_info.json` with a `_ts=<current timestamp>` query parameter and `cache: "no-store"`.
2. Parse both databases and reserialize them with `JSON.stringify()`. The exact minified UTF-8 byte streams are retained for upload.
3. Fetch the release FAP with the same timestamp cache buster and verify its byte length and SHA-256 against `release.json`.
4. Generate a complete native-compatible `amiibo.idx` in browser memory from those exact minified JSON bytes.
5. Only after all preparation succeeds, inspect the Flipper loader. If **Amiibo** / **Amiibo Zero** is running, request `loader close`. Unrelated running applications are not forcibly closed.
6. Remove every native active/temporary index artifact:
   - `amiibo.idx`
   - `amiibo.idx.tmp`
   - `amiibo.idx.bak`
   - `amiibo.raw.tmp`
   - `amiibo.sort.tmp`
   - `games.raw.tmp`
7. Upload and size-verify the FAP, optional replacement key, minified `amiibo.json`, and minified `games_info.json`.
8. Upload and size-verify `amiibo.idx` **last**, so a complete index can never point at JSON files whose transfers have not finished.
9. Run `loader open "/ext/apps/NFC/amiibo_zero.fap"` and verify through `loader info` that Amiibo is running.

If `/ext/apps_data/amiibo_zero/key_retail.bin` already exists and `storage stat` reports exactly **160 bytes**, the installer reuses it. A local key selection is then optional and acts as an explicit replacement. A selected key is read locally and sent only to the Flipper.

## Native index compatibility

The browser generator intentionally reproduces the binary contract in `src/amiibo_db.c`, rather than defining a web-only index format. Native `_Static_assert`s guard the ABI assumptions used by the TypeScript generator.

Current index v11 uses the 32-bit ARM Flipper ABI:

| Structure | Serialized size | Important details |
| --- | ---: | --- |
| `AzIndexHeader` | 80 bytes | little-endian; includes four ARM alignment-padding bytes before the first `AzSourceStamp` |
| `AzSourceStamp` | 16 bytes | 64-bit byte size + sampled Flipper CRC-32 |
| `AzIndexCategoryPrefix` | 8 bytes | followed immediately by `uint8_t name_length` raw name bytes |
| `AzIndexFigureRecord` | 112 bytes | fixed native figure layout |
| `AzIndexGameRef` | 16 bytes | 8-byte ID pattern + JSON byte offset/length |

Compatibility details handled by `src/lib/indexGenerator.tsx` include:

- all integer fields serialized little-endian;
- source fingerprints computed over the **exact minified UTF-8 files that are uploaded**, using the same sample offsets and incremental `crc32_calc_buffer()` behavior as Flipper firmware;
- JSON record offsets measured in UTF-8 **bytes**, not JavaScript UTF-16 code units;
- object ranges include the same opening `{` through closing `}` bytes recorded by the native lwJSON streaming callbacks;
- figure names use the native 64-byte C buffer semantics (at most 63 payload bytes), and release dates use the native 12-byte buffer semantics;
- truncation occurs at raw UTF-8 byte capacity, even if that cuts a multibyte code point, matching the native streaming decoder's bounded byte writes;
- control characters and lone surrogate handling mirror the native field-cleaning path;
- sorting performs ASCII-only case folding over unsigned raw bytes and then the same Amiibo-ID tie-break;
- v3 `(&...)` display suffix removal matches native processing;
- category names are variable length with a `uint8_t` serialized byte length;
- game-reference offsets point into the exact uploaded minified `games_info.json`.

This means a browser-generated index is consumed by the same native readers and source-stamp validation as one built on the Flipper.

## Files installed

| File | Flipper path | Source |
| --- | --- | --- |
| `amiibo_zero.fap` | `/ext/apps/NFC/amiibo_zero.fap` | Release-channel FAP from the same GitHub Actions run |
| `key_retail.bin` | `/ext/apps_data/amiibo_zero/key_retail.bin` | Existing valid 160-byte device file or optional user replacement |
| `amiibo.json` | `/ext/apps_data/amiibo_zero/amiibo.json` | AmiiboData, fetched fresh and minified |
| `games_info.json` | `/ext/apps_data/amiibo_zero/games_info.json` | AmiiboData, fetched fresh and minified |
| `amiibo.idx` | `/ext/apps_data/amiibo_zero/amiibo.idx` | Generated locally in the browser from those exact minified files |

## Web Serial transport and disconnect detection

The serial layer mirrors Flipper's official host tooling and firmware behavior:

1. Request the canonical Flipper Zero CDC device (`VID 0x0483`, `PID 0x5740`).
2. Open USB CDC and explicitly assert **DTR**; current Flipper CLI VCP firmware treats DTR activation as the connection event.
3. Wait for the existing `>: ` prompt, then issue `device_info` and wait specifically for `hardware_model`.
4. Replace files through `storage remove` + repeated `storage write_chunk "<path>" <size>` commands, waiting for `Ready` before sending each raw byte chunk.
5. Verify final file sizes through `storage stat`.

The UI does not rely only on an internal “connected” flag. `FlipperSerial` maintains a background read pump and listens for the browser's `navigator.serial` **disconnect** event. Either a USB disconnect event, a closed readable stream, or a failed serial read/write immediately clears connection state and notifies React, so unplugging the Flipper changes the UI back to disconnected without requiring another button press.

### Canonical Flipper references

- [USB CDC descriptor](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/targets/f7/furi_hal/furi_hal_usb_cdc.c) — canonical CDC VID/PID.
- [CLI VCP service](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/applications/services/cli/cli_vcp.c) — CLI interface and DTR connection behavior.
- [Official host storage client](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/scripts/flipper/storage.py) — prompt synchronization and `storage write_chunk` protocol.
- [Loader CLI](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/applications/services/loader/loader_cli.c) — `loader info`, `loader close`, and `loader open` behavior.
- [Official runfap helper](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/scripts/runfap.py) — canonical close/install/launch workflow.
- [Flipper CRC-32 implementation](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/lib/toolbox/crc32_calc.c) — source-stamp CRC behavior reproduced by the browser index generator.

## Source layout

All browser application code is TypeScript/TSX. React components are split into individual files under `src/components/`, each with a colocated scoped CSS Module. `src/global.css` is limited to global theme variables and reset/base rules.

```text
web/
  src/
    main.tsx
    global.css
    components/
      App.tsx
      App.module.css
      ...one TSX + CSS module per component
    lib/
      flipperSerial.tsx
      indexGenerator.tsx
      installer.tsx
  test/
    *.test.tsx
  vite.config.ts
  tsconfig.json
```

## Development

```sh
npm install
npm test
npm run typecheck
npm run dev
```

A local development checkout does not contain `release.json` or `amiibo_zero.fap` unless they are placed in `web/public/`. GitHub Actions injects both from the release-channel FAP artifact created by the same workflow run.

## Build and deployment

```sh
npm install
npm test
npm run build
```

`npm run build` performs strict TypeScript checking before the Vite build. Vite uses a relative asset base (`./`) so the static output works from a GitHub Pages repository subpath.

The repository workflow builds the release and dev FAP channels, injects the **release-channel FAP from that same workflow run** into the web build, generates `release.json`, tests and builds the installer, and creates the GitHub Release. GitHub Pages deployment depends on both the web build and successful release job and runs only for a pushed semantic-version tag, so branch/PR/manual development runs cannot update the live installer.
