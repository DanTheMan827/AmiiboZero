# Amiibo Zero

Amiibo Zero is a Flipper Zero NFC application for browsing Amiibo metadata, generating fresh Amiibo instances, emulating them over NFC, and managing persistent saved figures.

## Features

* Browse Amiibo by category.
* Search by figure name or hexadecimal Amiibo ID.
* Look up a figure by manually entering its 8-byte ID.
* Generate fresh Amiibo NFC data with randomized UIDs.
* Emulate generated Amiibo without saving them.
* Save generated Amiibo and emulate them persistently.
* Load and emulate previously saved Amiibo NFC files.
* Automatically save changes made to persistent Amiibo during emulation.
* Randomize the UID of the currently emulated Amiibo.
* Rename and delete saved figures.
* Create a fresh copy of an existing figure.
* View decrypted Amiibo metadata when valid retail keys are available.
* Browse per-game compatibility information.
* Support version-3 / Lock-On figures and associated SRAM payloads.
* Build and maintain a compact local database index directly on the Flipper Zero.
* Display database, key, and index status from inside the application.

## Requirements

Amiibo Zero targets the **Flipper Zero (`f7`)** and is built as an external FAP application.

To use the complete feature set, the application expects the following files on the Flipper Zero SD card:

```text
apps_data/amiibo_zero/
├── amiibo.json
├── games_info.json
├── key_retail.bin
├── figures/
└── lock_on/
```

The application creates its data, `figures`, and `lock_on` directories automatically.

### Amiibo database

`amiibo.json` contains the figure metadata used for browsing, searching, and generating Amiibo.

This file is required for the application database to become available.

### Game compatibility database

`games_info.json` contains game compatibility and usage information.

The main Amiibo library can be indexed without this file, but compatibility information will not be available.

### Retail key file

Amiibo Zero expects:

```text
key_retail.bin
```

at:

```text
apps_data/amiibo_zero/key_retail.bin
```

The file must be exactly **160 bytes** and contain the two 80-byte key sections expected by the Amiibo cryptographic routines.

Valid key material is required for operations such as:

* Generating a fresh Amiibo.
* Generating a new UID.
* Decrypting authenticated Amiibo data.
* Reading encrypted dump details.
* Re-keying saved Amiibo data after UID randomization.

Keys are not required simply to browse the metadata database.

Provide your own key material and Amiibo data from sources you are authorized to use.

## Downloading the databases

A helper script is included under:

```text
tools/fetch_databases.py
```

It downloads the raw Amiibo and game-information JSON databases used by the application.

Run it with the destination `apps_data/amiibo_zero` directory:

```bash
python3 tools/fetch_databases.py /path/to/sd/apps_data/amiibo_zero
```

For example, if the Flipper SD card is mounted locally, point the script at the corresponding directory on that card.

The script downloads:

```text
amiibo.json
games_info.json
```

It does **not** download keys or Amiibo dumps.

## Database indexing

Amiibo Zero does not require a pre-generated binary database.

On startup, the application checks the JSON database files and prepares its own binary index:

```text
apps_data/amiibo_zero/amiibo.idx
```

The index contains the information necessary for fast category browsing, figure lookup, searching, and game-reference lookup without loading the complete JSON documents into memory.

The application checks whether the existing index still matches the source databases. If the source data has changed or the index is invalid, it rebuilds the index in the background.

Database preparation progresses through several stages:

```text
Checking
Amiibo
Sorting
Games
Finalizing
Done
```

Temporary and backup index files may be created during this process. They are managed automatically by the application.

From **Setup & status**, pressing **OK** reloads the key file and forces a new background database-index build.

## Using Amiibo Zero

The main menu contains:

```text
Browse library
Saved figures
Advanced
Setup & status
About
```

### Browse library

The library groups indexed Amiibo into categories.

Select a category to browse its figures.

While browsing categories or figures, press **Left** to open search.

Search is case-insensitive and matches both:

* Amiibo names.
* Hexadecimal Amiibo IDs.

Selecting a figure opens its action menu.

For a figure from the database, the available actions are:

```text
Emulate once
Save + emulate
Compatibility
```

### Emulate once

Generates a fresh Amiibo instance and begins NFC emulation without creating a persistent saved file.

Because a fresh encrypted Amiibo is generated, valid retail keys are required.

Changes made during a temporary session are not written to a saved figure file.

### Save + emulate

Generates a fresh Amiibo, saves it under the application's `figures` directory, and starts NFC emulation.

Generated files use the Flipper NFC file format and receive unique filenames based on the figure name and Amiibo ID.

Persistent emulation synchronizes modified NFC data back to the saved file when emulation stops.

## Saved figures

Saved Amiibo files are stored in:

```text
apps_data/amiibo_zero/figures/
```

Amiibo Zero scans this directory for `.nfc` files and builds the Saved Figures list from recognized Amiibo records.

A saved figure provides actions including:

```text
Emulate + autosave
Dump details
Compatibility
Rename
Fresh copy
Delete
```

Version-3 figures also provide:

```text
Change lock-on
```

### Emulate + autosave

Loads the existing saved NFC file and begins persistent emulation.

When the session ends, writable changes received during emulation are synchronized back to the same `.nfc` file.

### Dump details

When the dump can be authenticated and valid retail keys are available, Amiibo Zero can display information including:

* Figure type and ID.
* Filename.
* Nickname.
* Owner Mii name.
* Initialization state.
* Application-data state.
* Registration date.
* Last-write date.
* Write counter.
* Application ID.
* Application-area ID.
* Application-area write counter.

If the encrypted data cannot be authenticated or keys are unavailable, the application still displays the basic figure information but cannot display the encrypted details.

### Rename

Saved figures can be renamed from inside Amiibo Zero.

The application sanitizes filenames and automatically chooses a numbered variant if the requested filename already exists.

Any companion Lock-On state belonging to the saved figure is renamed with it.

### Fresh copy

Generates a completely fresh Amiibo instance for the selected figure, including a newly randomized UID, and saves it as a new figure rather than replacing the existing saved file.

### Delete

Deletes the selected `.nfc` file.

If the saved figure has associated Lock-On data, that companion data is removed as well.

## UID randomization

While NFC emulation is active, press **OK** to generate a new UID.

A valid `key_retail.bin` is required because changing the UID requires the encrypted Amiibo data to be regenerated correctly.

For a persistent figure, the newly randomized data is also written back to its saved NFC file.

## Game compatibility

If `games_info.json` is installed, Amiibo Zero can display games associated with the selected Amiibo.

Compatibility records may include:

* Platform.
* Game title.
* Amiibo usage description.
* Whether the game writes data back to the Amiibo.

Supported platform labels in the current application include:

```text
3DS
Wii U
Switch
Switch 2
```

Use **Left** and **Right** to move between compatibility entries and **Up/Down** to scroll longer descriptions.

## Lock-On support

Some version-3 Amiibo require an accompanying Lock-On SRAM payload.

Source Lock-On files are loaded from:

```text
apps_data/amiibo_zero/lock_on/
```

The application accepts Lock-On source payloads between **1 and 62 bytes**, as well as complete **64-byte** SRAM images.

Short payloads are expanded into the application's 64-byte SRAM representation and receive the required CRC data automatically.

When generating a version-3 figure, Amiibo Zero prompts you to select a Lock-On payload before emulation can begin.

Persistent version-3 figures keep their selected Lock-On state associated with the saved Amiibo. The Lock-On data can later be replaced using **Change lock-on**.

## Advanced menu

The Advanced menu currently provides:

```text
Manual figure ID
```

This opens an 8-byte input screen.

Enter the raw 8-byte Amiibo figure ID to look up the corresponding figure directly in the local database.

## Setup & status

The status screen reports whether Amiibo Zero can find and use its required data:

```text
Keys: ready / missing or invalid
Amiibo JSON: found / missing
Games JSON: found / missing
Index: ready / unavailable
```

It also displays the number of figures represented by the current index.

Press **OK** on this screen to:

1. Reload `key_retail.bin`.
2. Force the database index to be rebuilt.

This is useful after replacing database files or adding the key file while the application is already installed.

## Building

The repository is configured as a Flipper Zero external application through:

```text
application.fam
```

The application ID is:

```text
amiibo_zero
```

and its FAP category is:

```text
NFC
```

With a compatible uFBT environment installed, build from the repository root:

```bash
ufbt
```

The project uses the Flipper SDK and links against mbedTLS. A copy of lwJSON is included under `third_party` for streaming JSON parsing.

The included GitHub Actions workflow builds against both the release and development Flipper SDK channels.

## Project structure

```text
.
├── application.fam
├── icon.png
├── src/
│   ├── amiibo_zero.c
│   ├── amiibo_zero.h
│   ├── amiibo_crypto.c
│   ├── amiibo_db.c
│   ├── amiibo_nfc.c
│   ├── amiibo_storage.c
│   └── amiibo_ui.c
├── third_party/
│   └── lwjson/
└── tools/
    └── fetch_databases.py
```

### Source modules

`amiibo_zero.c`
: Application entry point and shared service lifetime.

`amiibo_crypto.c`
: Amiibo key handling, encryption, decryption, authentication, dump generation, and UID regeneration.

`amiibo_db.c`
: Streaming JSON processing, database-index construction, browsing, search, lookup, and compatibility loading.

`amiibo_nfc.c`
: Conversion between Amiibo data and Flipper NFC devices, NFC validation, file loading/saving, and emulation support.

`amiibo_storage.c`
: Application directories, saved-figure management, filenames, Lock-On storage, rename operations, and deletion.

`amiibo_ui.c`
: Screen rendering, navigation, user input, emulation actions, and background database preparation.

## Runtime data

A typical populated data directory looks like:

```text
apps_data/amiibo_zero/
├── amiibo.json
├── games_info.json
├── key_retail.bin
├── amiibo.idx
├── figures/
│   ├── Example_0123456789abcdef.nfc
│   └── ...
└── lock_on/
    ├── payload1.bin
    └── ...
```

`amiibo.idx` and temporary index files are internal implementation files. They should normally be left for Amiibo Zero to manage.