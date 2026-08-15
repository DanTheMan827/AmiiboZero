# Amiibo Zero for Flipper Zero

Amiibo Zero is a Flipper Zero FAP for browsing Amiibo metadata, generating Amiibo data from an 8-byte figure ID plus a user-supplied `key_retail.bin`, emulating it, saving native `.nfc` files, showing game compatibility, and preserving game-side writes.

## Highlights

- **On-device JSON processing with lwJSON.** The raw `amiibo.json` and `games_info.json` remain authoritative and are never converted on a PC.
- **Unified on-device seek index.** One compact `amiibo.idx` stores figure/category metadata plus byte offset/length references into both JSON files so normal browsing and compatibility lookups do not scan the full databases.
- **Boot-time validation.** The app validates the index when it starts and only rebuilds it when needed.
- **Cheap source identity.** Index validation checks each JSON file size from filesystem metadata first, then fingerprints only small beginning/middle/end windows when sizes match. Normal startup no longer rereads the complete databases just to validate the cache.
- **Category-first browser.** Categories are alphabetical; figures inside each category are alphabetical.
- **Persistent menu position.** Returning to menus restores the previously selected row instead of jumping to the first item.
- **Safe UID randomization while emulating.** OK stops RF, synchronizes reader writes, authenticates/decrypts the dump, changes the UID, recomputes cryptographic state, saves persistent sessions, then restarts NFC.
- **Saved-dump management.** Rename, delete, create a fresh copy, inspect decrypted metadata, and emulate with autosave.
- **Advanced manual ID generation.** Enter exactly eight hexadecimal bytes with Flipper's byte editor and generate the figure even when it is absent from the metadata database.
- **Type-3 lock-on support.** IDs whose final byte is `03` use an NTAG I2C Plus 2K layout plus a separate user-supplied lock-on/vehicle payload. Fresh generation prompts for the lock-on file, and saved type-3 figures can swap it later without regenerating the rider data.
- **No Nintendo keys or Amiibo dumps are bundled.**

## SD-card layout

```text
/apps_data/amiibo_zero/
├── amiibo.json
├── games_info.json
├── key_retail.bin
├── amiibo.idx                 # generated/validated on the Flipper
├── lock_on/                   # user-supplied type-3 lock-on/vehicle payloads
│   └── *
└── figures/
    ├── *.nfc                    # native Flipper rider/tag state
    └── *.nfc.lockon             # app-owned 64-byte companion for saved type-3 figures
```

Index validity uses a low-I/O freshness stamp. The index header stores each source file size plus a CRC32 fingerprint of up to three 256-byte windows from the beginning, middle, and end. On boot, `storage_common_stat()` supplies the size without reading file contents; a size mismatch invalidates immediately. Only when sizes match does the app read the bounded sample windows (at most 768 bytes per large JSON file). Flipper exposes a storage-wide timestamp, not a stable per-file modification timestamp, so it is not used for this cache check. No sidecar files are required.

## Database files

Use the AmiiboAPI `dev` database files:

- `database/amiibo.json`
- `database/games_info.json`

The included helper downloads those files byte-for-byte. It does **not** parse JSON, hash files, write sidecars, or build the index:

```bash
python3 tools/fetch_databases.py /path/to/sd/apps_data/amiibo_zero
```

For generation and authenticated dump inspection, place your legally obtained 160-byte `key_retail.bin` at:

```text
/apps_data/amiibo_zero/key_retail.bin
```

The first 80 bytes are the data key and the second 80 bytes are the static/tag key.

## Unified seek index

`amiibo.idx` is created on boot if absent, structurally invalid, or stale. It is also rebuilt when Setup & status → OK is selected. Rebuilding runs on a worker thread while the UI displays an animated hourglass, the current stage, and a determinate 0–100% progress bar. A manual refresh first releases the on-demand Saved, Games, and lock-on catalogs and clears loaded NFC device data so the index worker starts with launch-level runtime memory usage.

The binary index contains:

- source size and bounded beginning/middle/end sample fingerprint for both JSON files;
- alphabetically sorted category records and their first figure ordinal;
- figure records containing ID, name, release date, type, category, and the exact `amiibo.json` object byte offset/length;
- generalized compatibility ID patterns from `games_info.json` with each matching JSON member's byte offset/length.

Compatibility lookup scans only the small binary pattern table. For matching patterns, it seeks directly to the indexed JSON object ranges and feeds only those ranges through lwJSON. Wildcard zero bytes in AmiiboAPI compatibility IDs are preserved.

Index replacement is transactional: a stale index is retained as a temporary backup until the new temporary index is promoted successfully.

Index construction is optimized for SD-card throughput: `amiibo.json` is parsed once for both figures and series names. Figure ordering is now category-batched: a bounded group of complete figure records is loaded from the unsorted temporary file, each category is heap-sorted in RAM, and the sorted records are written directly to the final index exactly once. The normal path therefore avoids rewriting the full figure table on every merge pass; a lower-memory external merge remains only as an allocation-failure fallback. `games_info.json` references are appended directly to the new index, and JSON reads use a fixed 2 KiB buffer. The raw JSON files remain authoritative.

## UI

### Home

- Browse library
- Saved figures
- Advanced
- Setup & status
- About

### Figure details

The header is the Amiibo/character name. Type and ID are shown in the detail body.

For a database/manual figure:

- Emulate once
- Save + emulate
- Compatibility

For a saved standard figure:

- Emulate + autosave
- Dump details
- Compatibility
- Rename
- Fresh copy
- Delete

Saved type-3 figures add **Change lock-on** before Rename. If a type-3 `.nfc` has no companion lock-on yet, choosing Emulate prompts for one and attaches it before starting RF.

### Saved dump details

When the dump authenticates with the supplied retail keys, the detail screen shows data when present, including:

- nickname;
- owner Mii name;
- registration and last-write dates;
- main write counter;
- application-data initialized state;
- application ID, application area ID, and application write counter.

For saved type-3 figures the detail screen also reports whether the companion lock-on response is attached or missing. Uninitialized Amiibo may legitimately contain none of the user fields above.

### Advanced manual ID

Advanced → Manual figure ID opens Flipper's `ByteInput` editor for exactly 8 bytes. Because the editor operates on bytes, entry is hexadecimal-only and cannot exceed the required 16 hexadecimal digits. If the entered ID exists in the index its metadata is used; otherwise the app names it from its raw ID and generates it normally.

### Status refresh

Setup & status → OK reloads `key_retail.bin` and forces a background index rebuild from the current raw JSON files. The working screen remains responsive and shows the active stage plus a 0–100% progress bar.

## NFC persistence and UID randomization

Saved figures use native Flipper `NfcDevice` files. Ordinary NTAG215 reader writes live in the active `NfcListener` and are synchronized into the owned `NfcDevice` on Back. Type-3 emulation uses the app-owned Type-3 command handler described below, so writable EEPROM/sector-1 pages update the owned native device directly. Persistent sessions save that device back to the same `.nfc` path; the selected lock-on SRAM remains a separate companion `.nfc.lockon` file.

Pressing OK while NFC emulation is active performs UID rotation in this order:

1. stop the NFC listener;
2. retrieve and copy the listener's latest reader-written data;
3. authenticate and decrypt the current Amiibo dump;
4. generate a fresh UID and BCC bytes;
5. replace the UID in plaintext;
6. derive UID-dependent keys, recompute both HMACs, and AES-CTR encrypt the protected regions;
7. update the physical UID, NTAG password, and page image;
8. save the same `.nfc` path for persistent sessions;
9. restart the NFC listener only after all mutation is complete.

Protected tag state is therefore never changed while RF emulation is active.

## Standard and v3/lock-on Amiibo

Normal figures are generated as 135-page NTAG215 devices.

For IDs ending in `03`, the rider/figure data and the lock-on/vehicle data are deliberately kept separate. Put your own lock-on payload files in `/ext/apps_data/amiibo_zero/lock_on/`. Files containing 1–62 payload bytes are zero-padded to the 62-byte SRAM body and receive a CRC16-MCRF4XX automatically; a complete 64-byte SRAM response is also accepted and its CRC is normalized. No lock-on data is bundled with the app.

Whenever Amiibo Zero generates a fresh type-3 figure, it opens a lock-on picker first. The selected source is normalized into the 64-byte SRAM response used at logical sector-0 pages `F0-FF`. For a persistent figure, the rider/tag state is saved as the native `.nfc` and that response is saved beside it as `<name>.nfc.lockon`. **Change lock-on** transactionally replaces only the companion response; it does not regenerate the authenticated rider payload, change the figure ID, or discard game-side rider writes. Renaming/deleting a saved type-3 figure carries its companion file with it.

Type-3 UID bytes are not stored like an NTAG215 UID. Amiibo Zero writes the seven RF UID bytes directly at physical bytes `00-06`, keeps byte `07` as RFU, and starts page 2 with `44 00 0F E0`; the same seven bytes are presented during ISO14443A anticollision. The v3 cryptographic image is signed with that direct-UID layout before the 64-byte nonce window is inserted.

The companion is necessary because Flipper's native `MfUltralightTypeNTAGI2CPlus2K` representation is compact rather than a flat 512-page physical image: sector-0 EEPROM pages `00-E9` use their natural indices, physical session pages `EC/ED` map to native pages `234/235`, and sector-1 page `xx` maps to native page `236 + xx`. Physical sector-0 SRAM `F0-FF` is not a persistent slot in that model. Keeping lock-on data external avoids corrupting/overlapping sector-1 native storage.

Type-3 emulation uses Flipper's public ISO14443A transport with a command state machine modeled on Flipper's stock `MfUltralightListener`, rather than the previous pixl.js-style flat-memory loop. That preserves Flipper's exact NTAG command lengths, four-bit ACK/NAK framing, standard-frame CRC behavior, HALT/field reset behavior, sleep-on-invalid-command handling, hidden PWD/PACK reads, write access checks, and two-frame SECTOR_SELECT state. The I2C Plus extensions are then layered on top: `GET_VERSION = 00 04 04 05 02 02 15 03`, compact sector-1/session mapping, `FAST_WRITE F0-FF`, and the external 64-byte lock-on SRAM window. After the mailbox FAST_WRITE, `NS_REG.SRAM_RF_READY` is exposed on `ED`/mirrored `F9` and reads of `F0-FF` return the selected lock-on response. The response path now also supports the full 64-page/256-byte FAST_READ size accepted by Flipper's stock listener.

Generation also follows observed v3 metadata rather than reusing the NTAG215 header verbatim: page 2 is `44 00 0F E0`, page 3 is `F1 10 FF EE`, and page 4 is `A5 00 00 00`. The supplied genuine I2C Plus 2K dumps consistently use that page-4 value. Generated configuration pages also match the observed `E7 = 08 00 00 00`, `E8 = 01 00 F8 48`, and `E9 = 08 01 00 00` values.

The type-3 implementation was independently written from observed format behavior and interoperability references; no pixl.js source or lock-on dump is bundled. Real-hardware compatibility should still be verified against the relevant Switch 2 game/reader.

## Memory design

- Neither JSON database is loaded wholly into RAM.
- lwJSON stream parsers are allocated only for active scans.
- Full JSON scans use 2 KiB reads; indexed object-range reads use 512-byte chunks.
- Index records are fixed-size and read by seek.
- Category memory is bounded by `AZ_MAX_CATEGORIES` during index construction.
- The UI retains only the visible four figure/category rows.
- Compatibility parsing only enters JSON ranges selected by the binary index.
- Saved-file and compatibility catalogs are allocated only while those workflows are open and are released when the user leaves them.
- Large database work is moved to a dedicated worker with its own bounded stack.

## Build

With uFBT:

```bash
ufbt
ufbt launch
```

The manifest intentionally uses explicit source masks so vendored `lwjson_stream.c` is compiled exactly once.

## Source layout

```text
application.fam
src/amiibo_zero.cpp        app lifetime and service ownership
src/amiibo_zero.h          shared runtime models and application entry declaration
src/amiibo_crypto.cpp/.h   generation, authentication, decrypt/re-key/encrypt
src/amiibo_db.cpp/.h       lwJSON scans and unified JSON seek index
src/amiibo_nfc.cpp/.h      NTAG215 plus stock-derived NTAG I2C Plus 2K emulation
src/amiibo_storage.cpp/.h  saved library, lock-on payloads, rename/delete, file naming
src/ui/ui_screen.cpp/.h       virtual Screen base class and Back/navigation contract
src/ui/ui_manager.cpp/.h      screen-stack owner, dispatcher bridge, modal inputs, UI actions
src/ui/ui_controls.cpp/.h     reusable drawing controls
src/ui/ui_screens.cpp/.h      concrete screen classes and per-screen input handlers
third_party/lwjson/        minimal MIT lwJSON streaming subset
tools/fetch_databases.py   raw database downloader only
docs/DEVELOPMENT.md
Doxyfile
THIRD_PARTY_NOTICES.md
```

## Documentation

Every app-owned C++ source/header begins with an `@file` block. Functions, callbacks, structs, enums, public APIs, and significant fields/constants are Doxygen documented, including static implementation helpers. `Doxyfile` extracts static symbols and treats documentation warnings as errors.

## References

Design/behavior was checked against:

- DanTheMan827/AmiiTag and TagWallet for standard Amiibo crypto behavior;
- 8bitDream/AmiiboAPI for figure/game metadata;
- Flipper Devices firmware APIs for GUI, Storage, NFC, ByteInput, and worker threads;
- solosky/pixl.js as an interoperability/behavioral reference for newer type-3 NTAG I2C Plus 2K Amiibo.

Review upstream licenses before redistribution. Nintendo, Amiibo, and game names/trademarks belong to their respective owners.

## Security and legal note

Amiibo Zero contains no Nintendo key material or copyrighted Amiibo dumps. It expects users to provide their own authorized key/database/tag data. Use it only with data and hardware you are authorized to use and in accordance with applicable law and platform terms.
