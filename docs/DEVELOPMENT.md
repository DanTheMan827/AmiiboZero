# Amiibo Zero development notes

## Build language and ownership

The FAP is C (`gnu17`) and uses the official Flipper external-app APIs. App-owned code lives in `src/`; the minimal lwJSON stream parser in `third_party/lwjson/` is MIT-licensed third-party code.

The application manifest uses explicit source masks:

```python
sources=["src/*.c", "third_party/lwjson/*.c"]
```

Do not replace this with a broad recursive pattern plus a second lwJSON pattern: uFBT can otherwise add the same lwJSON object twice and produce multiple-definition linker errors.

## JSON and unified index architecture

Raw JSON is always authoritative. `amiibo_db.c` feeds Flipper Storage bytes directly to `lwjson_stream_parser_t`; it never builds a DOM and never loads a complete database into memory.

The default runtime path is index-backed:

1. Read the existing index header and use `storage_common_stat()` to obtain source sizes without reading JSON contents.
2. A size mismatch invalidates immediately because different lengths cannot be byte-identical.
3. When sizes match, read only three fixed windows (beginning, middle, end; up to 256 bytes each) and compare their CRC32 fingerprint with the index header.
4. If both source stamps match, use the existing index immediately.
5. Otherwise stream the JSON and build `amiibo.idx.tmp`.
6. Recheck the same size/sample stamps before promotion so an ordinary source replacement during the rebuild is rejected without a second complete read.
7. Preserve the old `amiibo.idx` as `amiibo.idx.bak` during promotion, then promote the new index and remove temporary build files.

### Source identity

A source identity contains:

- exact 64-bit file size from filesystem stat metadata;
- a 32-bit CRC32 over up to three 256-byte sample windows from the beginning, middle, and end, with each window offset included in the fingerprint.

This stamp is deliberately a cache-freshness heuristic rather than a cryptographic integrity check. A size change is definitive and costs no content I/O. Same-size changes are detected when they affect any sampled window; adversarial or unusually localized same-size edits outside those windows can evade the stamp and require a manual refresh. Current Flipper firmware exposes `storage_common_timestamp()`, but it is storage-wide and is updated by ordinary writes (including index construction), so it cannot serve as a stable per-file modification time.

### Index records

The unified index stores fixed-size records:

- header/source stamps;
- categories with sorted figure start ordinals;
- figures with metadata and exact `amiibo.json` object offset/length;
- generalized compatibility ID patterns with exact `games_info.json` member offset/length.

The game table is intentionally a small binary pattern table rather than a duplicate materialized compatibility database. Wildcard matching occurs against the binary records; only matching raw JSON ranges are parsed.

### Index construction memory rule

`amiibo.json` is parsed once: figure records are written to one temporary fixed-record file while `amiibo_series` names update the same bounded category table later in that stream. Categories are already the outer sort key, so the normal sort path no longer performs a global external merge. It groups alphabetically ordered categories into a bounded RAM batch (target 512 figure records, about 57 KiB), sequentially scans the raw temporary records to fill that batch, heap-sorts each category segment in place, and writes the batch directly into the final index. Each figure is therefore written only once during sorting instead of once per merge pass. If the temporary batch allocation cannot be satisfied, the older bounded external merge remains as a low-memory fallback.

The batch sorter intentionally trades a small number of sequential rereads of the temporary figure file for eliminating repeated SD-card rewrites, tiny one-record merge reads, and per-pass syncs. This is substantially friendlier to slow microSD cards while still avoiding permanent retention of the complete figure database in RAM. The fallback also uses larger runs/chunks and relies on file close rather than redundant temporary-file syncs.

`games_info.json` references are appended directly to the transactional index file, avoiding a second temporary game-reference file and copy pass. JSON full-file scans use a fixed 2 KiB heap buffer so SD reads are less chatty without increasing worker-stack pressure. A manual Setup/status refresh first frees all on-demand Saved, Games, and lock-on catalog arrays and clears the loaded `NfcDevice`, restoring the large transient allocations to their launch-state baseline before the sorter allocates its batch.

## NFC/UID mutation invariant

Never mutate authenticated UID-dependent Amiibo state while the listener is active.

The UID-randomization sequence is:

1. stop whichever backend is active (`NfcListener` for NTAG215 or the raw NFC worker for type-3);
2. synchronize standard-listener writes into the owned device; custom type-3 writes already target the owned device directly;
3. free/clear the active listener state;
4. authenticate/decrypt the standard logical dump;
5. replace the UID in plaintext;
6. recompute tag/data HMACs and re-encrypt;
7. update UID/PWD/pages in the owned `MfUltralightData`;
8. save persistent sessions;
9. allocate/start a new listener.

If re-keying fails, restart from the synchronized unmodified device rather than leaving RF disabled when possible.

## Type-3 / lock-on format

`az_figure_is_v3()` identifies IDs with final byte `0x03`, matching the observed v3 identifier convention. The current experimental layout uses Flipper's `MfUltralightTypeNTAGI2CPlus2K` device type.

Do not treat the type-3 tag as merely a larger NTAG215. Amiibo Zero keeps two independent inputs:

- the normal authenticated rider/figure data generated from the eight-byte figure ID and `key_retail.bin`;
- a user-supplied lock-on/vehicle payload from `AZ_LOCKON_DIR`.

`az_lockon_load()` accepts a payload of 1–62 bytes or a complete 64-byte SRAM response. Short payloads are zero-padded to 62 bytes; in either case bytes 62–63 are regenerated as big-endian CRC16-MCRF4XX. Fresh type-3 generation must have a selected response before emulation begins.

Do **not** put that response into `MfUltralightData::page[240..255]`. Flipper's native I2C Plus representation is compact: sector-0 EEPROM `00-E9` uses native pages `0..233`, physical session `EC/ED` maps to native pages `234/235`, and sector-1 page `xx` maps to native page `236 + xx` (`236..491`). Sector-0 SRAM `F0-FF` is external to that native page array; treating native pages `240..255` as SRAM would overwrite sector-1 pages `04..13`.

The rider/tag `.nfc` therefore contains only the native `MfUltralightTypeNTAGI2CPlus2K` state. `az_saved_lockon_save()` stores the normalized 64-byte response beside it as `<saved>.nfc.lockon` using a temporary/backup replacement sequence, while `az_saved_lockon_load()` validates size and CRC before use. Rename/delete lifecycle operations handle the sidecar together with its `.nfc`. **Change lock-on** replaces only this sidecar, so rider state is not regenerated merely to change the attached vehicle/lock-on.

The rider image itself expands the regular authenticated Amiibo data by inserting a 0x40-byte nonce window at physical offset `0x80`, then installs the observed I2C Plus configuration/session regions into their correct compact native backing locations.

Generation must also use the v3 UID representation. Unlike NTAG215, real v3 metadata places the seven UID bytes directly at physical bytes `00-06`, byte `07` carries the zero SAK value in the I2C memory header, and page 2 begins `44 00 0F E0`. `az_nfc_set_identity_v3()` intentionally starts with `mf_ultralight_set_uid()` so ISO14443A identity setup follows Flipper's normal MFUL path, then restores Flipper's own I2C-style direct UID/SAK/ATQA memory header. It also installs the UID-derived password and `80 80` PACK into the hidden native config pages; reads of those pages still return zeroes.

### Stock-derived type-3 listener

Flipper's firmware emulates NTAG215 and NTAG I2C Plus through the same `MfUltralightListener` state machine. Amiibo Zero mirrors that listener's common behavior for type-3 tags: exact command-length dispatch, standard response CRC-A, four-bit ACK/NAK, PWD_AUTH state, hidden PWD/PACK reads, lock/write rules, `NfcCommandSleep` after invalid or unsupported commands, and reset of sector/auth state on raw frames, HALT, or field loss. This replaces the older flat 2048-byte pixl.js command loop.

The app-specific additions are limited to I2C Plus behavior that the stock listener cannot provide for this use case. Logical sector-0 `EC/ED` maps to native pages `234/235`; sector-1 page `xx` maps to `236 + xx`; sector-3 `F8/F9` mirrors the session registers. Sector-0 SRAM `F0-FF` stays external in `current_lockon_sram`. `FAST_WRITE F0-FF` ACKs the 64-byte mailbox request and marks the preselected lock-on response ready; subsequent reads of `ED`/`F9` expose `NS_REG.SRAM_RF_READY`, while READ/FAST_READ of `F0-FF` returns that 64-byte response. The command path accepts up to 64 FAST_READ pages (256 payload bytes), matching Flipper's stock listener rather than the previous 255-byte cap.

The first SECTOR_SELECT frame receives a four-bit ACK. The second frame must be four bytes, updates the selected sector, and is completed with the same silent/reset outcome used by Flipper's composite command handler. `GET_VERSION` returns `00 04 04 05 02 02 15 03` from the native `MfUltralightData::version` field.

Generation reference metadata for current v3 figures is `page 02 = 44 00 0F E0`, `page 03 = F1 10 FF EE`, and `page 04 = A5 00 00 00`. The supplied genuine dumps also agree on configuration pages `E7 = 08 00 00 00`, `E8 = 01 00 F8 48`, and `E9 = 08 01 00 00`; preserve these values unless new hardware evidence shows a revision-specific difference.

The implementation uses Flipper firmware and pixl.js as interoperability references; neither project's source files nor lock-on payloads are bundled into Amiibo Zero.

Do not assume all future Amiibo revisions use exactly the same layout merely because they are newer than NTAG215. Add a format discriminator and hardware test before extending detection.

## Saved metadata

`az_nfc_read_details()` authenticates/decrypts the physical 532-byte Amiibo payload before displaying user/game metadata. Do not apply offsets from tools that first rearrange the tag into the common internal/decrypted layout without translating those offsets back to the physical layout. Treat fields as optional: fresh/unregistered figures may contain no nickname, owner Mii, dates, or application-area data.

Do not display unauthenticated encrypted bytes as decoded metadata.

## UI state rules

- `screen_selection[AzScreenCount]` retains the last selected row for each custom screen.
- Before navigating away, store the current selection.
- When returning, restore the target screen's remembered selection unless that collection shrank; clamp saved-file selection after rescans/deletes.
- Figure detail uses the character/figure name as the header; type belongs in body text.
- Potentially slow index validation/rebuild runs in `AmiiboIndex` worker thread while `AzScreenWorking` animates and displays stage/percentage progress sampled from lock-free scalar fields.
- Module views (TextInput/ByteInput) return to the existing custom-screen state without resetting its selection.

## Memory and API rules

1. Never read a full JSON file or arbitrary JSON object into a size-scaled heap buffer.
2. Prefer fixed record seeks and bounded final destination fields.
3. Keep JSON read buffers small and fixed-size.
4. Put large parser/compatibility/index contexts on the heap or a dedicated worker stack rather than the main 6 KiB FAP stack.
5. Do not retain the complete figure database in RAM.
6. Avoid unavailable external-app libc imports previously known to break FAP linking (`qsort`, `bsearch`, locale ctype helpers, `atexit`).
7. Keep native NFC writes synchronized before any save or UID mutation.
8. Keep the raw database helper download-only; indexing belongs on-device.

## Documentation rules

Every app-owned `.c` and `.h` file must have `@file`. Document:

- every function, including static helpers and callbacks;
- every struct/enum/important alias;
- struct and enum members;
- public API parameters and return semantics;
- non-obvious constants and invariants.

The vendored lwJSON subset must retain upstream copyright/license notices and its integration-facing types/functions must remain documented.

`Doxyfile` enables static extraction, scans app code plus the vendored subset/tooling, and uses warning-as-error behavior. A documentation regression should fail the documentation job rather than silently reducing generated coverage.

## Validation checklist

Before packaging:

- strict `gnu17` compile with `-Wall -Wextra -Werror`;
- check stack-usage reports for unexpectedly large frames;
- inspect unresolved imports for forbidden symbols;
- run the unified-index synthetic test, including size-change invalidation and sampled-window same-size mutation invalidation;
- verify indexed raw offsets begin at the expected JSON objects;
- verify wildcard compatibility uses matching indexed ranges only;
- test generate → decrypt → UID re-key roundtrip with deterministic host fixtures when crypto test infrastructure is available;
- verify both NTAG215 and v3 page mappings;
- verify short lock-on payload padding/CRC, 64-byte normalization, and invalid-size rejection;
- verify v3 `FAST_WRITE` is ACKed without overwriting the selected mailbox response and `FAST_READ F0-FF` returns that response;
- verify two-step v3 `SECTOR_SELECT` maps sector-1 reads to the contiguous backing pages;
- verify UID randomization preserves the selected lock-on SRAM;
- verify native type-3 page mapping does not alias sector-0 SRAM onto sector-1 backing;
- verify saved lock-on sidecar load/save/CRC, transactional replacement, rename, and delete lifecycle;
- test saved rename collision handling and selection restoration;
- run Doxygen with warnings as errors when Doxygen is available;
- run uFBT against the target firmware SDK; this remains the authoritative FAP linker/API check.
