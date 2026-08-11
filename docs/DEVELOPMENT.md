# Amiibo Zero development notes

## Source ownership

The app itself is C17/gnu17 code under the project root. `third_party/lwjson/` is third-party MIT-licensed code derived from lwJSON v1.9.0 and intentionally contains only the streaming parser surface used by the app.

## JSON architecture

`amiibo_db.c` never loads either AmiiboAPI JSON file into RAM. It opens the file with Flipper Storage, reads 512-byte chunks, and feeds each byte to one heap-allocated `lwjson_stream_parser_t`. Callbacks interpret lwJSON's retained key stack and copy only fields needed by the current operation.

The default browsing path is direct JSON:

- categories: bounded category table only;
- figures/search: four-row sorted window only;
- find-by-ID: current figure only, early stop on match;
- compatibility: current game/current usage plus bounded result rows.

Animation ticks reuse the current four-row figure/search window and therefore do not repeatedly scan the SD card.

## Optional `.idx` feature

The historical on-device index remains available for comparison and slow-card experiments but is disabled by default:

```c
#ifndef AZ_FEATURE_JSON_INDEX
#define AZ_FEATURE_JSON_INDEX 0
#endif
```

Set the macro to `1` at build time to enable it. The index builder still uses lwJSON for raw JSON parsing. No hand-written JSON tokenizer is retained.

The optional index uses:

- `amiibo.idx`
- `amiibo.idx.tmp`
- `amiibo.raw.tmp`

The default build creates none of these.

## Memory rules

New database code should preserve these constraints:

1. Do not read a whole JSON file or object into a `malloc` buffer.
2. Do not retain the whole Amiibo figure list in RAM.
3. Prefer the final destination field over intermediate string copies.
4. Keep SD read-ahead buffers small and fixed-size.
5. Allocate large parser/compatibility contexts on the heap rather than the 6 KiB FAP stack.
6. Avoid libc functions that are disabled in the Flipper external-app API (`qsort`, locale ctype helpers, and similar imports previously caught by CI).
7. Keep both `AZ_FEATURE_JSON_INDEX=0` and `=1` warning-clean.

## Documentation rules

Every app-owned `.c`/`.h` file begins with an `@file` block. Every app-owned function, callback, struct, enum, and significant field/type has a Doxygen comment. Static/internal functions are documented too because `Doxyfile` enables static extraction.

When adding code:

- add a concise `@brief` before each new function;
- document parameters/return semantics in the public header for exported APIs;
- document struct and enum fields inline with `/**< ... */` where practical;
- document feature flags and non-obvious constants;
- preserve upstream license/documentation in third-party code.

## Validation

At minimum, check both database feature modes with warning-as-error C compilation and run the synthetic lwJSON database test. The authoritative final link remains the uFBT build against official Flipper firmware SDK channels.
