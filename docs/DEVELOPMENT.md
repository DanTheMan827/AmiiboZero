# Amiibo Zero development notes

## Build language and ownership

The core app logic remains C while the custom UI is C++. Each custom screen is a concrete C++ class in `src/ui/<screen>.h/.cpp`, derived from `AzUiScreen`, with its own `draw()` and `input()` methods. `src/amiibo_ui.cpp` owns ViewDispatcher integration and shared controller actions. The C/C++ boundary is declared with C linkage in `amiibo_zero.h`. App-owned code lives in `src/`; the minimal lwJSON stream parser in `third_party/lwjson/` is MIT-licensed third-party code.

The application manifest uses explicit source masks:

```python
sources=["src/*.c", "src/*.cpp", "src/ui/*.cpp", "third_party/lwjson/*.c"]
```

Do not replace this with a broad recursive pattern. The screen `src/ui/<screen>.cpp` files are normal C++ translation units and must be compiled once. A broad recursive pattern plus a second lwJSON pattern can add the same lwJSON object twice and produce multiple-definition linker errors.

## Size-build notes

- `-Os`, function/data sections, linker garbage collection, and final FAP stripping are supplied by the compact Flipper/uFBT SDK build.
- AmiiboZero implements HMAC-SHA256 directly with the SHA-256 primitive so the static mbedTLS link does not retain the generic digest dispatcher, MD5, or SHA-1 solely for HMAC.
- Database progress scaling must remain 32-bit; progress reporting is not precise enough to justify pulling 64-bit division helpers into the FAP.
- `tools/compact_fap.py` is an optional post-build operation that removes ordinary `.rel.*` sections only when matching `.fast.rel.*` sections are present. Keep the original FAP until the compact copy has been tested on-device.
- Do not add `-flto` only to a subset of sources. Current uFBT does not expose a top-level per-app LTO switch in `application.fam`; compile and link LTO flags must come from a consistent build environment.

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

The reverted unified index stores fixed-size records:

- header/source stamps;
- `AzCategory` records with sorted figure start ordinals;
- complete `AzFigure` metadata records with exact `amiibo.json` object offset/length;
- generalized compatibility ID patterns with exact `games_info.json` object offset/length.

The game table is a binary pattern table. Wildcard matching occurs against the indexed records and every matching JSON range is parsed into the compatibility result set.

### Index construction memory rule

`amiibo.json` is streamed once into the temporary fixed-record file `amiibo.raw.tmp`. Category descriptors keep only ID/count plus a pointer and `uint8_t` name length; the names themselves are packed at exact length into small pooled slabs. Categories are sorted alphabetically in RAM, serialized as an 8-byte prefix followed by `name_length` bytes, then the entire category descriptor/name pool is freed before figure sorting begins. The normal figure sorter targets batches of up to 512 figures, but it does not keep complete 112-byte index records resident. Each sort-key descriptor keeps only a pooled-name pointer, `uint8_t` name length, binary ID, and raw-file ordinal, while a contiguous `uint16_t` order vector supplies the movable ordering. Heap sort moves only those 2-byte order entries; complete fixed figure records stay in `amiibo.raw.tmp` and are read back through a four-record stack staging buffer in final sorted order.

If even the compact largest-category batch cannot be allocated, the build falls back to the bounded external run/merge sorter. Its run size is eight full records (896 bytes) and that run buffer lives on the already-reserved application stack, so the fallback does not need a record-array heap allocation. JSON full-file scans use a fixed 2 KiB heap buffer.

`games_info.json` is streamed directly into compatibility reference records in the transactional index. At runtime, category/figure/search windows are read on demand from `amiibo.idx`; the complete catalog is not retained in RAM. Compatibility parsing seeks only to matching stored game ranges.

## Physical NTAG215 programming

Physical tag operations use Flipper's exported `NfcPoller` / `MfUltralightPoller` APIs rather than raw ST25R traffic. `nfc_poller_start_ex()` is used for ordered page-level transactions so the app controls exactly when irreversible lock pages are written; normal `MfUltralightPollerModeWrite` is intentionally not used for blank Amiibo programming because its internal page order is not the app's lock-last contract.

A blank-write transaction is split into two NFC phases with crypto between them on the application thread:

1. activate the tag, GET_VERSION, require NTAG215, read pages 0–3 and 130–133, reject any non-zero static/dynamic lock state or non-factory AUTH0, and capture the exact nine-byte raw UID/BCC layout;
2. stop/free the poller; authenticate/decrypt the selected v2 dump with the existing Amiibo crypto and re-sign/re-encrypt it for the captured UID;
3. reactivate the tag, revalidate NTAG215 + blank locks + identical UID, then program pages 3–129;
4. write PACK and UID-derived PWD, write AUTH0, authenticate with that password, then write ACCESS/CFGLCK;
5. write static lock page 2 and dynamic lock page 130 as the final two page transactions.

The scan/write split deliberately keeps KDF/HMAC/AES work off the NFC worker thread and its stack. `tag_work_dump` is a single 532-byte transient heap allocation released on completion/cancel.

For retail-tag read/save and user-data reset, do not run the generic MFUL discovery state machine. It probes READ_SIG, counters, tearing flags, and default passwords that are unrelated to the Amiibo payload and can cause an otherwise-readable tag to report `ReadFailed`. Use the extended poller at ISO14443A Ready, issue GET_VERSION, require NTAG215, read pages 0–3 to capture the live raw UID, authenticate with the UID-derived Amiibo password, then read pages 4–132 directly. Reconstruct the hidden PWD/PACK pages in the in-memory native model.

For clear, the authenticated encrypted dump is reset in plaintext by clearing bytes 17–51 and 160–519, then normal HMAC/encryption code rebuilds the ciphertext. Only physical pages 4–12 and 32–129 are written back. This mirrors the writable restore ranges used by established Amiibo tooling and avoids touching UID/manufacturer/model/salt/lock/config/PWD/PACK pages. Read & save verifies the Amiibo HMACs with the loaded retail keys and saves the reconstructed native model through `NfcDevice`.

## NFC/UID mutation invariant

Never mutate authenticated UID-dependent Amiibo state while the listener is active.

UID randomization is a standard NTAG215-only feature. Type-3 screens hide the action, the UI controller rejects it, and `az_nfc_randomize_uid()` refuses non-NTAG215 devices so v3 identity cannot be changed accidentally.

For standard NTAG215, the UID-randomization sequence is:

1. stop the active `NfcListener`;
2. synchronize listener writes into the owned device;
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

Flipper's firmware emulates NTAG215 and NTAG I2C Plus through the same `MfUltralightListener` state machine. Amiibo Zero mirrors its framing/state behavior for type-3 tags: exact command-length dispatch, standard response CRC-A, four-bit ACK/NAK, hidden Sector-0 PWD/PACK reads, lock/write rules, `NfcCommandSleep` after invalid or unsupported commands, and reset of sector/auth state on raw frames, HALT, or field loss. For v3 interoperability, `PWD_AUTH` deliberately follows pixl.js and returns PACK without requiring the locally derived password; physical lock/config rules still gate writes.

Do not apply the generic NTAG21x access structure blindly to I2C Plus 2K. `AUTH0` at `E3` protects only Sector 0 through `EB` and explicitly excludes the dynamic-lock page; `ACCESS.NFC_DIS_SEC1` at `E4` controls whether Sector 1 is visible; `PT_I2C.2K_PROT`/`SRAM_PROT` at `E7` independently protect Sector 1/SRAM; the dynamic-lock bytes are at `E2` with 32-page granularity; and `REG_LOCK_NFC` is `E9` byte 2 bit 0. `EC/ED` and mirrored `F8/F9` are read-only through NFC. These are physical RF addresses, not `pages_total - N` offsets from Flipper's compact backing array.

Logical sector-0 `EC/ED` maps to native pages `234/235`; sector-1 page `xx` maps to `236 + xx`; sector-3 `F8/F9` mirrors the session registers. Sector-0 SRAM `F0-FF` stays external in `current_lockon_sram`. `FAST_WRITE F0-FF` ACKs the 64-byte mailbox request without overwriting the preselected lock-on response. Following pixl.js's Joy-Con scan fix, every read exposing `NS_REG` asserts `SRAM_RF_READY`, including a normal `READ EC` response window; READ/FAST_READ of `F0-FF` returns the selected 64-byte response. The command path accepts up to 64 FAST_READ pages (256 payload bytes), matching Flipper's stock listener rather than the previous 255-byte cap.

The first SECTOR_SELECT frame receives a four-bit ACK. The second frame must be four bytes, updates the selected sector, and is completed with the same silent/reset outcome used by Flipper's composite command handler. `GET_VERSION` returns `00 04 04 05 02 02 15 03` from the native `MfUltralightData::version` field.

Generation reference metadata follows the pixl.js v3 image: `page 02 = 44 00 0F E0`, `page 03 = F1 10 FF EE`, `page 04 = A5 00 00 00`, `E2 = 01 00 FF 00`, `E3 = 00 00 00 04`, `E4 = 07 00 00 00`, raw `E5-EB = 00...00`, `EC = 41 00 F8 48`, and `ED = 08 01 29 00`. Amiibo Zero subsequently stores the hidden UID-derived PWD and `80 80` PACK at native `E5/E6`; reads of those Sector-0 pages still return zeroes.

The implementation uses Flipper firmware and pixl.js as interoperability references; neither project's source files nor lock-on payloads are bundled into Amiibo Zero.

Do not assume all future Amiibo revisions use exactly the same layout merely because they are newer than NTAG215. Add a format discriminator and hardware test before extending detection.

## Saved metadata

`az_nfc_read_details()` authenticates/decrypts the physical 532-byte Amiibo payload before displaying user/game metadata. Do not apply offsets from tools that first rearrange the tag into the common internal/decrypted layout without translating those offsets back to the physical layout. Treat fields as optional: fresh/unregistered figures may contain no nickname, owner Mii, dates, or application-area data.

Do not display unauthenticated encrypted bytes as decoded metadata.

## UI state rules

- Each custom screen is a full C++ class derived from `AzUiScreen`; drawing and non-navigation input handling live together in that screen's `src/ui/<screen>.cpp`. The base class also defines `backRequested()`, `onResume()`, and `onPopped()` lifecycle hooks so every screen follows the same framework contract. `src/amiibo_ui.cpp` owns the stack and shared controller actions. Shared drawing helpers live in `src/ui/ui_common.cpp`.
- `application.fam` compiles `src/ui/*.cpp` as normal translation units; screen sources are not included into `amiibo_ui.cpp`.
- `AZ_DEBUG_MEMORY_OVERLAY` is defined by default unless `AZ_DISABLE_MEMORY_OVERLAY` is defined. All overlay code is guarded with `#ifdef AZ_DEBUG_MEMORY_OVERLAY`; the counter is drawn last in the same custom-screen canvas callback and must never be implemented as a second fullscreen viewport.

- Custom screens are pushed onto the bounded `ui_stack`. Each stack entry owns its screen, selection, and detail-scroll state. Back is handled centrally: the active screen receives the actual request through `backRequested()` and may veto it; otherwise the framework pops the top entry.
- Popping the final stack entry stops the `ViewDispatcher`, which is the normal application-exit path. Screens must not call `view_dispatcher_stop()` for ordinary Back handling.
- `screen_selection[AzScreenCount]` remains a remembered default for a newly pushed instance. Returning through Back restores the exact state stored in that stack entry instead of reconstructing a destination from ad-hoc return-screen fields.
- `onResume()` may repair state that was intentionally released while a screen was covered. Saved figures use this to reload/clamp the catalog after memory-heavy workflows release it.
- `onPopped()` owns screen-specific transient cleanup. It must not free application-level services.
- Figure detail uses the character/figure name as the header; type belongs in body text.
- Potentially slow index validation/rebuild runs in `AmiiboIndex` worker thread while `AzScreenWorking` animates and displays stage/percentage progress sampled from lock-free scalar fields. `AzScreenWorking::backRequested()` rejects Back while that worker is active; completion pops the working screen and exposes its caller.
- Module views (TextInput/ByteInput) return to the existing custom-screen state without resetting its selection.

### NFC lifetime across UI navigation

`Nfc` and the shared `NfcDevice` are application-owned resources allocated in `amiibo_zero_app()` and freed only during application shutdown. Generic screen push/pop/replace operations must never free, null, or otherwise disable those objects.

Screen cleanup may release only the active NFC *session* that belongs to that screen. Popping Emulate stops/synchronizes its listener so the same app-owned NFC worker is immediately available for later emulation or polling. Popping a physical tag-operation screen cancels/frees only its poller and transient operation buffers. This separation prevents ordinary navigation from accidentally making NFC unavailable while still ensuring mutually exclusive listener/poller sessions are released before another NFC workflow begins.

## Memory and API rules

1. Never read a full JSON file or arbitrary JSON object into a size-scaled heap buffer.
2. Prefer fixed record seeks and bounded final destination fields.
3. Keep JSON read buffers small and fixed-size.
4. Put large parser/compatibility/index contexts on the heap or a dedicated worker stack rather than the main 6 KiB FAP stack.
5. Do not retain the complete figure database in RAM; read bounded category/figure/search windows from the seek index.
6. Avoid unavailable external-app libc imports previously known to break FAP linking (`qsort`, `bsearch`, locale ctype helpers, `atexit`).
7. Keep native NFC writes synchronized before any save or UID mutation.
8. Keep the raw database helper download-only; indexing belongs on-device.

## Documentation rules

Every app-owned `.c`, `.cpp`, and `.h` file must have `@file`. Document:

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
- verify fixed figure records and variable-length category records match the source, including each figure's `amiibo.json` offset/length;
- verify wildcard compatibility scans only matching indexed `games_info.json` ranges and combines all matches;
- test generate → decrypt → UID re-key roundtrip with deterministic host fixtures when crypto test infrastructure is available;
- verify both NTAG215 and v3 page mappings;
- verify short lock-on payload padding/CRC, 64-byte normalization, and invalid-size rejection;
- verify v3 `FAST_WRITE` is ACKed without overwriting the selected mailbox response and `FAST_READ F0-FF` returns that response;
- verify two-step v3 `SECTOR_SELECT` maps sector-1 reads to the contiguous backing pages;
- verify v3 emulation hides/rejects UID randomization and leaves the selected lock-on SRAM untouched;
- verify native type-3 page mapping does not alias sector-0 SRAM onto sector-1 backing;
- verify saved lock-on sidecar load/save/CRC, transactional replacement, rename, and delete lifecycle;
- test saved rename collision handling and selection restoration;
- run Doxygen with warnings as errors when Doxygen is available;
- run uFBT against the target firmware SDK; this remains the authoritative FAP linker/API check.

## Database sort and debug overlay

The database sorter keeps full fixed records on storage and only materializes compact sort keys in RAM. Its normal path repeatedly performs sequential scans of `amiibo.raw.tmp` to fill bounded groups of whole categories, sorts a `uint16_t` order vector over stable key slabs, then rereads the selected full records in sorted order into a small stack staging buffer before appending them to the destination index. Keep this behavior aligned with `amiibo_db.c`; do not replace it with a full resident catalog or a single large contiguous record array.

When `AZ_DEBUG_MEMORY_OVERLAY` is enabled, the heap counter shows `free heap / largest contiguous free block` in KiB and is drawn after the active AmiiboZero screen renderer in the same canvas callback. Do not implement it as a second `GuiLayerFullscreen` viewport: Flipper GUI chooses one fullscreen viewport per redraw, so a debug viewport would replace the dispatcher rather than composite over it.
