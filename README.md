# Amiibo Zero for Flipper Zero

Amiibo Zero is a mixed C/C++ Flipper Zero FAP for browsing Amiibo metadata, generating Amiibo data from an 8-byte figure ID plus a user-supplied `key_retail.bin`, emulating it, saving native `.nfc` files, showing game compatibility, and preserving game-side writes.

## Highlights

- **On-device JSON processing with lwJSON.** The raw `amiibo.json` and `games_info.json` remain authoritative and are never converted on a PC.
- **Unified on-device seek index.** One `amiibo.idx` stores fixed category/figure records plus exact byte ranges into `amiibo.json` and `games_info.json`, so normal browsing/searching reads compact index records and compatibility parsing seeks only matching JSON objects.
- **Boot-time validation.** The app validates the index when it starts and only rebuilds it when needed.
- **Cheap source identity.** Index validation checks each JSON file size from filesystem metadata first, then fingerprints only small beginning/middle/end windows when sizes match. Normal startup no longer rereads the complete databases just to validate the cache.
- **Category-first browser.** Categories are alphabetical; figures inside each category are alphabetical.
- **Persistent menu position.** Returning to menus restores the previously selected row instead of jumping to the first item.
- **Safe UID randomization for standard NTAG215 figures.** OK stops RF, synchronizes reader writes, authenticates/decrypts the dump, changes the UID, recomputes cryptographic state, saves persistent sessions, then restarts NFC. Type-3 figures deliberately hide and reject UID randomization because changing the v3 identity can make the figure unrecognizable to games.
- **Saved-dump management.** Rename, delete, create a fresh copy, inspect decrypted metadata, emulate with autosave, or write a saved v2 figure to a blank NTAG215.
- **Physical NTAG215 tools.** Generated and saved v2 figures can be written to verified blank NTAG215 tags; Advanced can read/save a retail Amiibo or clear its writable user state.
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
- alphabetically sorted variable-length category records with a fixed 8-byte prefix, one-byte name length, and first-figure ordinal;
- fixed `AzFigure` records containing ID, hexadecimal ID, name, North-American release date, category/type bytes, and the exact `amiibo.json` object offset/length;
- generalized compatibility ID patterns from `games_info.json`, each with the exact matching JSON member offset/length.

Compatibility lookup scans the compact binary pattern table. Every matching wildcard pattern is parsed from its indexed `games_info.json` range, preserving the supplied database implementation's combine-all-matches behavior.

Index replacement is transactional: a stale index is retained as a temporary backup until the new temporary index is promoted successfully.

Index construction follows the supplied database implementation. `amiibo.json` is streamed once into fixed figure records in `amiibo.raw.tmp` while categories are collected in RAM. Categories are sorted in RAM. The normal figure sort groups whole categories into a bounded heap batch, sequentially rescans the temporary raw-record file to fill that batch, heap-sorts each category segment in RAM, and writes the completed batch directly to the final index. The older external-run/merge implementation remains in the source but the supplied build path keeps its fallback disabled. `games_info.json` references are appended directly to the index.

## UI

The custom UI is organized as full C++ screen classes. Every `src/ui/<screen>.h/.cpp` pair defines one class derived from `AzUiScreen`; the class owns both drawing and input behavior for that screen. `src/amiibo_ui.cpp` owns the ViewDispatcher, shared actions/navigation, and selects the active screen object. `ui_common.*` contains shared drawing helpers. The screen sources are normal translation units rather than macro include fragments.

A bottom-right heap overlay is enabled by default for debugging and displays `free heap / largest contiguous free block` in KiB. It is compiled behind `#ifdef AZ_DEBUG_MEMORY_OVERLAY` and is drawn last in the same canvas callback as the active AmiiboZero screen, so it does not replace or clear the underlying fullscreen view. Define `AZ_DISABLE_MEMORY_OVERLAY` to omit it.

### Home

- Browse library
- Saved figures
- Advanced
- Setup & status
- About

### Figure details

The header is the Amiibo/character name. Type and ID are shown in the detail body.

For a database/manual v2 figure:

- Emulate once
- Save + emulate
- Write to blank tag
- Compatibility

Type-3 figures omit **Write to blank tag**.

For a saved standard v2 figure:

- Emulate + autosave
- Dump details
- Compatibility
- Write to blank tag
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

### Physical NTAG215 tools

**Write to blank tag** is available only for standard/v2 figures. The writer first issues NTAG GET_VERSION and rejects anything that does not identify as NTAG215. It also checks that static/dynamic lock bits are still clear before any programming. Amiibo Zero reads the destination tag UID, authenticates/decrypts the selected encrypted dump with the existing crypto implementation, substitutes that physical UID, recomputes both HMACs, and re-encrypts the payload. The tag is checked again immediately before writing so swapping tags between scan and programming is rejected.

Programming deliberately commits irreversible protection last. Pages 3–129 are written first, followed by PACK and the UID-derived password. AUTH0 is written next, the writer authenticates with the new password, and ACCESS/CFGLCK is written only after authentication. Page-2 static lock bytes and page-130 dynamic lock bytes are the final two writes. This follows the established NTAG215 Amiibo programming layout while honoring the stronger invariant that lock bits are never written before all ordinary data/configuration is complete.

Advanced also provides **Read & save Amiibo** and **Clear tag user data**. Both operations now use a direct NTAG215 scan: GET_VERSION, read the live UID, authenticate with the UID-derived Amiibo password, and read pages 0–132 directly. This deliberately bypasses the generic MFUL reader's unrelated READ_SIG/counter/tearing probes, so failure of those optional commands cannot abort an Amiibo read. Read & save verifies the encrypted payload with `key_retail.bin`, reconstructs hidden PWD/PACK fields in the native Flipper model, and saves it into `figures/`. Clear user data decrypts the authenticated page image, resets writable user-state plaintext, recomputes/encrypts it, and writes only pages 4–12 and 32–129. UID/manufacturer bytes, model/salt pages, lock bytes, password, PACK, and configuration pages are not rewritten by clear.

### Advanced manual ID

Advanced → Manual figure ID opens Flipper's `ByteInput` editor for exactly 8 bytes. Because the editor operates on bytes, entry is hexadecimal-only and cannot exceed the required 16 hexadecimal digits. If the entered ID exists in the index its metadata is used; otherwise the app names it from its raw ID and generates it normally.

### Status refresh

Setup & status → OK reloads `key_retail.bin` and forces a background index rebuild from the current raw JSON files. The working screen remains responsive and shows the active stage plus a 0–100% progress bar.

## NFC persistence and UID randomization

Saved figures use native Flipper `NfcDevice` files. Ordinary NTAG215 reader writes live in the active `NfcListener` and are synchronized into the owned `NfcDevice` on Back. Type-3 emulation uses the app-owned Type-3 command handler described below, so writable EEPROM/sector-1 pages update the owned native device directly. Persistent sessions save that device back to the same `.nfc` path; the selected lock-on SRAM remains a separate companion `.nfc.lockon` file.

For a standard NTAG215 figure, pressing OK while NFC emulation is active performs UID rotation in this order. Type-3 figures do not expose this action and the controller/NFC layer reject it defensively:

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

Type-3 emulation uses Flipper's public ISO14443A transport with a command state machine modeled on Flipper's stock `MfUltralightListener`, with I2C Plus 2K behavior corrected at the physical RF addresses. `AUTH0` applies only to Sector 0 through `EB`; Sector 1 uses `ACCESS.NFC_DIS_SEC1` and `PT_I2C.2K_PROT`; SRAM uses `PT_I2C.SRAM_PROT`; the dynamic-lock bytes are at `E2` with 32-page granularity; and `REG_LOCK_NFC` is `E9` byte 2 bit 0. Session pages `EC/ED` and their Sector-3 mirrors are read-only over NFC. `PWD_AUTH` is accepted using the pixl.js v3 interoperability behavior so console writes do not depend on an unverified v3 password derivation, while static/dynamic locks and configuration locks are still enforced.

The I2C Plus extensions include `GET_VERSION = 00 04 04 05 02 02 15 03`, compact sector-1/session mapping, `FAST_WRITE F0-FF`, and the external 64-byte lock-on SRAM window. Following the pixl.js Joy-Con compatibility fix, reads that expose `NS_REG` always assert `SRAM_RF_READY`; this includes a normal `READ EC` window as well as direct `ED`/mirrored `F9` reads. The mailbox `FAST_WRITE F0-FF` is ACKed without overwriting the selected precomputed lock-on response, and reads of `F0-FF` return that response.

Generation follows the observed pixl.js v3 image rather than reusing the NTAG215 tail verbatim: page 2 is `44 00 0F E0`, page 3 is `F1 10 FF EE`, page 4 is `A5 00 00 00`, `E2 = 01 00 FF 00`, `E3 = 00 00 00 04`, `E4 = 07 00 00 00`, and the raw `E5-EB` region is zero before Amiibo Zero installs hidden PWD/PACK values in its native model. Session pages are `EC = 41 00 F8 48` and `ED = 08 01 29 00`.

V3 encrypted user metadata is treated as best-effort only. If the loaded retail key does not authenticate a v3 rider image after game-side writes, Amiibo Zero preserves those raw writes and reports that v3 user details are not decoded instead of implying that `key_retail.bin` is missing or that the figure itself is invalid. Figure identity and the separate lock-on response remain usable.

The type-3 implementation was independently written from observed format behavior and interoperability references; no pixl.js source or lock-on dump is bundled. Real-hardware compatibility should still be verified against the relevant Switch 2 game/reader.

## Memory design

- Neither JSON database is loaded wholly into RAM.
- lwJSON stream parsers are allocated only for active scans.
- Full JSON scans use 2 KiB reads; indexed standalone-object reads use 512-byte chunks.
- The complete figure/category catalog is not retained in RAM. Visible figure rows are read from fixed figure records; category records use a compact variable-length name section and only visible categories are expanded into UI structs.
- Each indexed figure stores its fixed metadata plus the exact `amiibo.json` object offset/length; compatibility patterns retain exact `games_info.json` object ranges.
- Category names are stored as `uint8_t` length-prefixed variable strings. Figure names remain in fixed on-disk figure records for cheap random access, while rebuild sorting stores figure names at exact length in pooled slabs.
- Compatibility parsing scans the binary wildcard table and seeks to each matching `games_info.json` offset/length range.
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
src/amiibo_zero.c          app lifetime and service ownership
src/amiibo_zero.h          shared documented models and public APIs
src/amiibo_crypto.c        generation, authentication, decrypt/re-key/encrypt
src/amiibo_db.c            lwJSON scans and unified JSON seek index
src/amiibo_nfc.c           NTAG215 plus stock-derived NTAG I2C Plus 2K emulation
src/amiibo_storage.c       saved library, lock-on payloads, rename/delete, file naming
src/amiibo_ui.cpp          central input/navigation controller + UI ownership
src/ui/*.cpp               per-screen C++ renderer translation units
src/ui/*.h                 per-screen renderer declarations
src/ui/ui_common.cpp       shared drawing helpers
third_party/lwjson/        minimal MIT lwJSON streaming subset
tools/fetch_databases.py   raw database downloader only
docs/DEVELOPMENT.md
Doxyfile
THIRD_PARTY_NOTICES.md
```

## Optional post-build FAP compaction

Current Flipper tooling adds `.fast.rel.*` relocation data but retains the original `.rel.*` sections for compatibility. After a successful uFBT build, `tools/compact_fap.py` can create a second FAP with the redundant standard relocation sections removed. It refuses to do so unless every removable `.rel.*` section has a matching `.fast.rel.*` section.

```powershell
python tools/compact_fap.py dist/amiibo_zero.fap
```

The normal uFBT-produced FAP is left untouched. Test the compact copy on the target firmware before distributing it. LTO is intentionally not forced by the project because current uFBT does not expose a top-level per-app compile/link flag in `application.fam`; LTO must be enabled consistently by the SDK/build environment if you choose to benchmark it.

## Documentation

Every app-owned C file begins with an `@file` block. Functions, callbacks, structs, enums, public APIs, and significant fields/constants are Doxygen documented, including static implementation helpers. `Doxyfile` extracts static symbols and treats documentation warnings as errors.

## References

Design/behavior was checked against:

- DanTheMan827/AmiiTag and TagWallet for standard Amiibo crypto behavior;
- 8bitDream/AmiiboAPI for figure/game metadata;
- Flipper Devices firmware APIs for GUI, Storage, NFC, ByteInput, and worker threads;
- solosky/pixl.js as an interoperability/behavioral reference for newer type-3 NTAG I2C Plus 2K Amiibo.

Review upstream licenses before redistribution. Nintendo, Amiibo, and game names/trademarks belong to their respective owners.

## Security and legal note

Amiibo Zero contains no Nintendo key material or copyrighted Amiibo dumps. It expects users to provide their own authorized key/database/tag data. Use it only with data and hardware you are authorized to use and in accordance with applicable law and platform terms.
