# Amiibo Zero for Flipper Zero

Amiibo Zero is a C Flipper Zero FAP for browsing Amiibo metadata, generating Amiibo/NTAG215 data from an 8-byte figure ID and a user-supplied `key_retail.bin`, emulating it, saving native `.nfc` files, showing game compatibility, and persisting reader writes back to saved figures.

## Highlights

- **Pure C application code** for the Flipper FAP.
- **JSON is processed on the Flipper.** There is no host-side JSON indexing/preprocessing step.
- **lwJSON v1.9.0 streaming API** reads `amiibo.json` and `games_info.json` directly from the SD card with fixed parser memory and small file read buffers.
- **Category-first library:** Browse Library opens Amiibo series/categories first, then the Amiibo inside the selected category.
- Categories are sorted alphabetically.
- Figures inside each category are sorted alphabetically.
- Category names come from AmiiboAPI's `amiibo_series` map; the figure's series/category is derived from byte 6 of its full 8-byte Amiibo ID.
- Search still works globally by name or 16-hex-digit figure ID.
- **Readability-focused 128x64 UI:** selected long rows scroll, non-selected rows fit to width, long headers scroll, and detail text wraps with vertical scrolling.
- Header, content, and footer areas are separated so controls do not overlap list/detail text.
- Compatibility, Setup & Status, and About screens support Up/Down vertical text scrolling.
- **Generate from figure ID only:** no donor Amiibo dump is required.
- **NTAG215 emulation** using Flipper's NFC protocol/listener stack.
- **Persistent saved figures** as native `.nfc` files under the app data directory.
- **Automatic write-back:** saved-figure reader writes are copied from the NFC listener and saved back to the same `.nfc` file when emulation ends.
- **Fresh copy** creates a separate generated figure with a fresh UID/salt.
- No Nintendo key material or Amiibo dumps are bundled.

## SD-card layout

Amiibo Zero uses:

```text
/apps_data/amiibo_zero/
├── amiibo.json
├── games_info.json
├── key_retail.bin
├── amiibo.idx
└── figures/
    └── *.nfc
```

### Required database files

Copy these two files to `/apps_data/amiibo_zero/` on the Flipper SD card:

- `amiibo.json` from `8bitDream/AmiiboAPI`, branch `dev`, `database/amiibo.json`
- `games_info.json` from `8bitDream/AmiiboAPI`, branch `dev`, `database/games_info.json`

For generation, place your legally obtained 160-byte `key_retail.bin` at:

```text
/apps_data/amiibo_zero/key_retail.bin
```

The file has the following layout: the first 80 bytes are the data key and the second 80 bytes are the static/tag key.

## On-device database design

The FAP uses **lwJSON's streaming parser** rather than a DOM or an app-owned tokenizer. The vendored integration contains only the stream API/types and stream parser implementation needed by Amiibo Zero, based on lwJSON v1.9.0 commit `be2b042fae1401957dcc01860532e15b40d3eb66` under the MIT license.

1. `amiibo_series` is streamed into a small fixed category table.
2. `amiibos` is streamed again to count category membership. No figure object survives beyond its callback.
3. Category names are insertion-sorted alphabetically.
4. Figure/category and search screens retain only the four visible figure records. Alphabetical paging is implemented with repeated bounded successor/predecessor scans of `amiibo.json`, not a full in-memory figure array. The current four-row window is cached so 250 ms UI animation ticks do not rescan the SD card.
5. `games_info.json` is streamed only when Compatibility is opened. The parser retains only the current game/usage state plus the caller's bounded result array.

### Memory behavior

- JSON files are never loaded wholly into RAM.
- lwJSON's parser object is heap-allocated only for the duration of a scan.
- SD reads use a 512-byte buffer.
- Category storage is bounded by `AZ_MAX_CATEGORIES`.
- Figure/search display storage is bounded to `AZ_LIST_ROWS` (four records).
- String values are decoded directly into their final bounded destination fields, including common JSON escapes and `\uXXXX` BMP escapes; complete JSON values are not copied into intermediate objects.

## UX

### Home

- **Browse library**
- **Saved figures**
- **Setup & status**
- **About**

### Browse library

The browser opens with **Library categories** rather than a flat figure list.

- Up/Down: move through categories.
- OK: open the selected category.
- Left: search the complete library.

Inside a category:

- Up/Down: move through alphabetically sorted figures.
- OK: open the selected figure.
- Left: global search.
- Back: return to categories.

Search results use the same scrolling list UI and can be opened normally.

### Long text behavior

- Long headers horizontally scroll instead of staying permanently truncated.
- The currently selected list row horizontally scrolls so the full label can be read.
- Non-selected rows are fitted to the available pixel width.
- Figure names horizontally scroll on detail screens.
- Compatibility, Status, and About use pixel-width wrapping and vertical scrolling.
- Toast/status messages temporarily replace the header title instead of being drawn over page content.

### Figure screen

For an unsaved database figure:

- **Emulate once**
- **Save + emulate**
- **Compatibility**

For a saved figure:

- **Emulate + autosave**
- **Compatibility**
- **Fresh copy**
- **Delete**

The action area shows three rows at a time and scrolls when the fourth saved-figure action is selected, avoiding footer overlap.

### Compatibility

- Left/Right: previous/next compatibility record.
- Up/Down: scroll wrapped detail text.
- The detail includes game name, platform, read/write behavior, and usage description.

## Persistence model

Saved figures are native Flipper `NfcDevice` files. During persistent emulation:

1. Load or generate the figure into an `NfcDevice`.
2. Start an `NfcListener` using its MIFARE Ultralight / NTAG215 data.
3. Reader commands mutate the listener's emulated data.
4. On Back/exit, stop the listener.
5. Fetch the listener's current MIFARE Ultralight data.
6. Copy it back into the owned `NfcDevice`.
7. Save that device back to the same `.nfc` path.

That preserves game-side Amiibo writes between sessions.

## Generation algorithm

The C implementation uses the following generation algorithm:

- construct a 532-byte Amiibo memory image;
- place the selected 8-byte figure ID at offsets `84..91`;
- generate a random 9-byte raw NTAG UID representation with BCC bytes;
- create a random 32-byte key-generation salt at offsets `96..127`;
- derive AES/HMAC material from the static and data keys;
- generate tag and data HMACs;
- AES-CTR encrypt the protected data region;
- expose the data as a 135-page NTAG215 with password/PACK fields.

## Build

### uFBT

From the project directory:

```bash
ufbt
```

To launch directly on a connected Flipper:

```bash
ufbt launch
```

### GitHub Actions

`.github/workflows/build-fap.yml` builds against both the official Flipper **release** and **dev** SDK channels using the official uFBT GitHub Action and uploads the generated FAP artifacts.

## Source layout

```text
application.fam       FAP manifest
amiibo_zero.c         app lifetime / services
amiibo_zero.h         shared types and interfaces
amiibo_crypto.c       Amiibo generation and cryptography
amiibo_db.c           lwJSON streaming integration, direct category/search browsing, optional index
amiibo_nfc.c          NTAG215 / NfcDevice bridge
amiibo_storage.c      saved-figure library and unique file naming
amiibo_ui.c           category-first, scrolling 128x64 UI
icon.png              10x10 1-bit FAP icon
third_party/lwjson/   minimal vendored lwJSON stream subset (MIT)
tools/fetch_databases.py
docs/DEVELOPMENT.md
THIRD_PARTY_NOTICES.md
.github/workflows/build-fap.yml
```


## Documentation

All bundled source is documented: app-owned C functions, callbacks, structs, enums, public APIs, and important fields/constants use Doxygen comments.

See `docs/DEVELOPMENT.md` for the database feature flag, memory rules, source ownership, and documentation conventions.

## Reference projects

The implementation was designed against:

- DanTheMan827/AmiiTag and TagWallet for Amiibo data generation/crypto behavior.
- 8bitDream/AmiiboAPI `dev` database files.
- Flipper Devices' current firmware APIs for GUI, storage, NFC device/listener, and MIFARE Ultralight.

Review upstream licensing and terms before redistribution. Nintendo/Amiibo names and trademarks belong to their respective owners.

## Security and legal note

Amiibo Zero does not contain Nintendo key material or copyrighted Amiibo dumps. It expects users to provide their own key file and database files. Use it only with data and hardware you are authorized to use and in accordance with applicable law and platform terms.
