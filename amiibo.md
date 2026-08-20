# Amiibo flat binary formats, cryptography, and NFC emulation

This document is a general-purpose technical reference for constructing, validating, and emulating Amiibo-compatible NFC images from public reverse engineering and the underlying NXP tag documentation.

It intentionally describes **flat binary files**, not Flipper Zero `.nfc` files or any one application's internal representation.

| Amiibo generation | Flat file | Meaning |
| --- | ---: | --- |
| v2 / classic | **572 bytes (`0x23C`)** | 540-byte NTAG215 RF memory image followed by a 32-byte `READ_SIG` response |
| v3 | **2048 bytes (`0x800`)** | Sector 0 (`0x000..0x3FF`) followed by sector 1 (`0x400..0x7FF`) in the public v3 flat-image convention; invalid/register/SRAM addresses occupy byte positions even though they are not all EEPROM |

> [!IMPORTANT]
> Nintendo's Amiibo master-key bytes are not included here. The document describes how compatible software uses key material that the user has lawfully obtained.

> [!NOTE]
> There is no official public Nintendo specification for the Amiibo payload. NXP's tag behavior is documented by NXP; Amiibo payload fields are reverse engineered. v2 has broad independent implementation history. v3 is newer and some details remain unsettled.

## 1. Terminology and confidence levels

- **RF/raw image**: bytes in NFC page order as a reader sees them.
- **Switchbrew plain image**: the 540-byte (`0x21C`) reordered/decrypted representation, including the 20-byte lock/config tail.
- **amiitool cryptographic internal payload**: the first 520 bytes (`0x208`) of that reordered representation. Upstream `amiitool` performs Amiibo crypto only on this `0x208`-byte payload; the lock/config tail is outside the cryptographic transform.
- **Page**: four bytes in NFC Type 2 memory.
- **Sector**: for NTAG I²C Plus, a 256-page address space selected with `SECTOR_SELECT`.
- **Amiibo ID**: the 8-byte model/character identifier; it is not the NFC UID.
- **UID**: the NFC tag's 7-byte ISO14443A serial number.

> [!CAUTION]
> **`0x208` (520) and `0x21C` (540) are not interchangeable sizes.** Upstream `amiitool` signs/encrypts a reordered `0x208`-byte Amiibo payload. The extra `0x14` bytes in Switchbrew's `0x21C` plain/raw descriptions are the dynamic-lock/config tail and remain outside the AES/HMAC payload transformation.

Confidence labels used below:

- **NXP hardware** — documented in an NXP product data sheet.
- **Established Amiibo v2** — independently implemented and/or documented by Switchbrew, 3dbrew, and upstream `amiitool`.
- **v3 reverse engineered** — observed in current Nintendo software or a public implementation tested against v3 hardware/software.
- **Compatibility convention** — behavior used by an emulator for interoperability; it may not be identical to physical silicon.

## 2. Amiibo ID

Switchbrew describes the 8-byte Amiibo ID as:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 3 | Character ID field |
| `0x03` | 1 | Series ID |
| `0x04` | 2 | Numbering/model ID, big-endian when exposed through Switch model info |
| `0x06` | 1 | NFP type |
| `0x07` | 1 | Amiibo version; v2 is `0x02`, current v3 figures use `0x03` |

For the 24-bit character field, Switchbrew currently describes bits 0-9 as game ID, bits 10-15 as character ID, and bits 16-23 as character variant. In tooling, the safest interface is the complete known 8-byte ID rather than separately re-encoding those bit fields.

The ID is stored at the start of a 44-byte **Amiibo ID entry**. The remaining 36 bytes are not fully documented: Switchbrew labels four bytes unknown and the final 32 bytes unknown/possibly hash-like. A generator should preserve those bytes from a known-good template unless their semantics are understood.

**Sources:** [Switchbrew NFC services](https://www.switchbrew.org/wiki/NFC_services), [3dbrew Amiibo](https://www.3dbrew.org/wiki/Amiibo).

---

# Part I — v2 / NTAG215

## 3. The 572-byte v2 container

A v2 flat file in this document uses this archive/emulator convention:

| File range | Size | Meaning |
| --- | ---: | --- |
| `0x000..0x21B` | 540 bytes | NTAG215 pages `0x00..0x86` |
| `0x21C..0x23B` | 32 bytes | Response returned by `READ_SIG 3C 00` |

The last 32 bytes are **not NTAG215 EEPROM**. NXP documents the originality signature as a chip-specific ECC signature programmed at production and immutable afterward. It is returned by a command. Therefore:

- a physical NTAG215 has 540 addressable page bytes, not 572;
- a normal NFC `WRITE` cannot program the 32-byte signature;
- `key_retail.bin` cannot derive or manufacture that NXP signature;
- an emulator may store the signature after the 540-byte image so it can answer `READ_SIG`.

**NXP hardware source:** [NTAG213/215/216 data sheet](https://www.nxp.com/docs/en/data-sheet/NTAG213_215_216.pdf), section `READ_SIG`.

## 4. NTAG215 physical memory map

NTAG215 has 135 pages (`0x00..0x86`) of four bytes each, for 540 bytes total. NXP specifies 504 bytes of user memory at pages `0x04..0x81`.

| Page(s) | Raw offset | Meaning |
| --- | ---: | --- |
| `00` | `0x000` | `UID0 UID1 UID2 BCC0` |
| `01` | `0x004` | `UID3 UID4 UID5 UID6` |
| `02` | `0x008` | `BCC1 INTERNAL LOCK0 LOCK1` |
| `03` | `0x00C` | Capability Container (CC) |
| `04..81` | `0x010..0x207` | 504 bytes user memory |
| `82` | `0x208` | Dynamic lock bytes 0-2; byte 3 reads as `BD` |
| `83` | `0x20C` | Configuration page 0 |
| `84` | `0x210` | Configuration page 1 / ACCESS |
| `85` | `0x214` | 32-bit PWD |
| `86` | `0x218` | 16-bit PACK + 2 RFU bytes |

### 4.1 UID and BCC

For a seven-byte UID `U0..U6`:

```text
page 00 = U0 U1 U2 BCC0
page 01 = U3 U4 U5 U6
page 02 = BCC1 INTERNAL LOCK0 LOCK1

BCC0 = 0x88 ^ U0 ^ U1 ^ U2
BCC1 = U3 ^ U4 ^ U5 ^ U6
```

NXP-programmed NTAG21x normally has `UID0 = 0x04`, NXP's manufacturer ID. During ISO14443A anticollision, BCC/cascade handling is part of the RF protocol; do not pass BCC bytes as if they were extra UID bytes.

### 4.2 Capability Container

A commonly observed Amiibo CC is:

```text
F1 10 FF EE
```

This is an **Amiibo-personalized value**, not the NTAG215 factory default. NXP documents a factory default CC of `E1 10 3E 00` for NTAG215. An emulator targeting Amiibo should reproduce the image being emulated, not substitute factory defaults.

### 4.3 Static lock bytes — page `02`, bytes 2-3

NXP's static lock bytes are one-time-programmable from the NFC side. When written, incoming bits are bitwise ORed with existing lock bits: a `1` cannot later be cleared to `0` over NFC.

The lock layout is:

```text
LOCK0 bit7  L7       locks page 7
      bit6  L6       locks page 6
      bit5  L5       locks page 5
      bit4  L4       locks page 4
      bit3  LCC      locks page 3 / CC
      bit2  BL15-10  freezes lock configuration for pages 10..15
      bit1  BL9-4    freezes lock configuration for pages 4..9
      bit0  BLCC     freezes the page-3 CC lock bit

LOCK1 bit7..0 = L15..L8, one bit per page
```

When writing page 2, bytes 0-1 are unaffected by the NFC `WRITE`/`COMPATIBILITY_WRITE`; only bytes 2-3 affect static locks.

The common Amiibo static-lock bytes `0F E0` decode as:

- `LOCK0 = 0x0F`: lock the CC page and set all three block-lock bits;
- `LOCK1 = 0xE0`: lock pages 15, 14, and 13;
- pages 4-12 are not statically marked read-only by these bits, though password/config rules can still restrict them.

**NXP hardware source:** data sheet section 8.5.2 and Figure 9.

### 4.4 Dynamic lock bytes — page `82`

For NTAG215, page `0x82` is:

```text
byte0 = dynamic lock bits
byte1 = RFU
byte2 = block-lock bits in low nibble; upper nibble RFU
byte3 = always reads as 0xBD
```

The individual bits in byte 0 protect 16-page groups:

| Byte 0 bit | Pages locked |
| ---: | --- |
| 0 | `16..31` |
| 1 | `32..47` |
| 2 | `48..63` |
| 3 | `64..79` |
| 4 | `80..95` |
| 5 | `96..111` |
| 6 | `112..127` |
| 7 | `128..129` |

Byte 2 freezes future changes to larger groups:

| Byte 2 bit | Lock configuration frozen for |
| ---: | --- |
| 0 | `16..47` |
| 1 | `48..79` |
| 2 | `80..111` |
| 3 | `112..129` |
| 4..7 | RFU |

A common Amiibo page-82 value is:

```text
01 00 0F BD
```

which locks pages `16..31` and freezes all four dynamic-lock groups. As with static locks, these bits are irreversible from the NFC interface.

A lock bit changes **writeability**, not ordinary readability: locking a page makes it read-only from the NFC interface. The block-lock (`BL`) bits do not directly lock the user pages themselves; they freeze the corresponding lock-bit configuration so those lock bits can no longer be changed over NFC.

### 4.5 Password/config pages and common Amiibo values

Common v2 Amiibo personalization uses:

```text
page 83: 00 00 00 04
page 84: 5F 00 00 00
page 85: <UID-derived 4-byte PWD>
page 86: 80 80 00 00
```

For NTAG215:

- page 83 byte 3 is `AUTH0`; `04` means password protection starts at page 4;
- page 84 byte 0 is `ACCESS`;
- `ACCESS = 0x5F` decodes as:
  - `PROT=0`: password protects **write**, not normal read access;
  - `CFGLCK=1`: after a power cycle, the first two configuration pages become permanently write-protected (PWD/PACK remain separately writable under NXP's rules);
  - `NFC_CNT_EN=1`: the 24-bit NFC counter is enabled;
  - `NFC_CNT_PWD_PROT=1`: `READ_CNT` requires authentication;
  - `AUTHLIM=7`: negative password attempts are limited using NXP's maximum encoded nonzero setting.

NXP specifies that reading PWD or PACK through `READ`/`FAST_READ` returns zeros, never the stored secret bytes.

### 4.6 Amiibo UID-derived PWD and PACK

Amiibo tooling commonly derives the NTAG password from the seven-byte UID as:

```text
PWD0 = UID1 ^ UID3 ^ 0xAA
PWD1 = UID2 ^ UID4 ^ 0x55
PWD2 = UID3 ^ UID5 ^ 0xAA
PWD3 = UID4 ^ UID6 ^ 0x55
PACK = 0x80 0x80
```

Proxmark3 identifies this as its Amiibo password algorithm (`ul_ev1_pwdgenB`) and PACK generator (`ul_ev1_packgenB`). NXP specifies that PWD and PACK are written/transmitted least-significant byte first as stored.

**Source:** [Proxmark3 `common/generator.c`](https://github.com/RfidResearchGroup/proxmark3/blob/master/common/generator.c).

## 5. v2 Amiibo raw and plain layouts

### 5.1 Raw 540-byte layout

Switchbrew's raw layout is:

| Raw offset | Size | Meaning |
| ---: | ---: | --- |
| `0x000` | `0x08` | NTAG data 1 |
| `0x008` | `0x08` | NTAG data 2 |
| `0x010` | `0x04` | Amiibo header |
| `0x014` | `0x20` | Encrypted section 1 |
| `0x034` | `0x20` | Tag HMAC-SHA256 |
| `0x054` | `0x2C` | Amiibo ID entry |
| `0x080` | `0x20` | Data HMAC-SHA256 |
| `0x0A0` | `0x114` | Encrypted section 2 |
| `0x1B4` | `0x54` | Encrypted section 3 |
| `0x208` | `0x04` | Dynamic lock page |
| `0x20C` | `0x10` | Config, PWD, PACK |

### 5.2 Switchbrew plain layout and upstream `amiitool` crypto payload

Switchbrew describes a 540-byte (`0x21C`) **plain** representation: the first `0x208` bytes are reordered/decrypted Amiibo content and the final `0x14` bytes are the dynamic-lock/config tail. Upstream [`socram8888/amiitool`](https://github.com/socram8888/amiitool) defines `NFC3D_AMIIBO_SIZE` as **520 bytes (`0x208`)** and performs its rearrangement, HMAC, and AES operations only on that cryptographic payload.

The `0x208`-byte cryptographic internal payload is:

| Internal offset | Size | Meaning |
| ---: | ---: | --- |
| `0x000` | `0x08` | NTAG data 2 |
| `0x008` | `0x20` | Data HMAC |
| `0x028` | `0x04` | Header |
| `0x02C` | `0xB0` | Settings |
| `0x0DC` | `0xD8` | Application/game area |
| `0x1B4` | `0x20` | Tag HMAC |
| `0x1D4` | `0x08` | NTAG data 1 |
| `0x1DC` | `0x2C` | Amiibo ID entry |

In the 540-byte Switchbrew plain representation only, the unencrypted tail remains at:

| Plain offset | Size | Meaning |
| ---: | ---: | --- |
| `0x208` | `0x04` | Dynamic lock |
| `0x20C` | `0x10` | Tag config / PWD / PACK |

The exact raw-to-internal mapping in upstream `amiitool` is:

```text
internal 0x000..0x007 <- raw 0x008..0x00F
internal 0x008..0x027 <- raw 0x080..0x09F
internal 0x028..0x04B <- raw 0x010..0x033
internal 0x04C..0x1B3 <- raw 0x0A0..0x207
internal 0x1B4..0x1D3 <- raw 0x034..0x053
internal 0x1D4..0x1DB <- raw 0x000..0x007
internal 0x1DC..0x207 <- raw 0x054..0x07F
```

The reverse mapping writes those regions back to their original raw offsets. The lock/config tail is not copied by upstream `amiitool`'s raw/internal conversion and is not part of the `0x208`-byte cryptographic payload.

**Primary implementation source:** [upstream `amiitool` `amiibo.c`](https://github.com/socram8888/amiitool/blob/master/amiibo.c).

## 6. Header, settings, nickname, Mii, and application data

### 6.1 Amiibo header

At internal `0x028` / raw `0x010`:

| Header offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 1 | Magic `0xA5` |
| `0x01` | 2 | Amiibo write counter, big-endian |
| `0x03` | 1 | Header version, normally `0x00` |

The header version is not the same field as byte 7 of the 8-byte Amiibo ID.

### 6.2 Settings block

The settings block is `0xB0` bytes at internal `0x02C`:

| Settings offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 1 | Font region in bits 0-3; Amiibo flags in bits 4-7 |
| `0x01` | 1 | Country-code ID |
| `0x02` | 2 | Terminal-ID CRC change counter, big-endian |
| `0x04` | 2 | First-write packed date, big-endian storage |
| `0x06` | 2 | Last-write packed date, big-endian storage |
| `0x08` | 4 | NFC terminal-ID CRC32, big-endian |
| `0x0C` | `0x14` | Amiibo nickname: 10 UTF-16BE code units, not NUL-terminated |
| `0x20` | `0x5C` | Mii `Ver3StoreData` base record |
| `0x7C` | 2 | Padding |
| `0x7E` | 2 | Mii CRC16 |
| `0x80` | 8 | Modified application ID, big-endian |
| `0x88` | 2 | Application-area write counter, big-endian |
| `0x8A` | 4 | Access ID, big-endian |
| `0x8E` | 1 | Original application-ID byte `(ApplicationID >> 28) & 0xFF` |
| `0x8F` | 1 | `Unknown1` |
| `0x90` | 8 | Mii `StoreDataExtension` |
| `0x98` | `0x14` | `Unknown2`; semantics are not settled |
| `0xAC` | 4 | Mii/settings CRC32, big-endian |

Important flag bits in settings byte `0x00`:

- `0x10`: figure is initialized/registered;
- `0x20`: application area exists.

The packed date encodes day in bits 0-4, month in bits 5-8, and year relative to 2000 in bits 9-15. The two bytes are stored big-endian in the settings block.

Current Switchbrew notes are important for `Unknown2`: although its purpose is still uncertain, firmware 20.5.0+ is observed to **zero it for v3** during several write/register operations. Therefore it should not be described as a known v3 extension field.

### 6.3 Amiibo nickname

The Amiibo nickname field at settings `0x0C` is exactly 20 bytes / 10 UTF-16BE code units. Switchbrew describes it as **not NUL-terminated**. When shorter names are represented by a higher-level API, remaining code units should follow behavior of the source/template being emulated rather than assuming a C-string terminator is part of the on-tag format.

### 6.4 Mii `Ver3StoreData`

The embedded base Mii record at settings `0x20` is 92 bytes (`0x5C`). 3dbrew states that its Mii page uses **little-endian by default unless a field is explicitly noted otherwise**.

Useful offsets in the base record are:

| Mii offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 1 | Mii version, normally 3 |
| `0x01` | 1 | Copy/profanity/region/character-set flags |
| `0x02` | 1 | Selection-screen page/slot |
| `0x03` | 1 | Includes original-device bits |
| `0x04` | 8 | System ID |
| `0x0C` | 4 | Mii ID; documented as big-endian |
| `0x10` | 6 | Creator MAC address |
| `0x16` | 2 | Padding |
| `0x18` | 2 | Sex/birthday/favorite-color/favorite bitfield |
| `0x1A` | `0x14` | Mii name, UTF-16, max 10 characters, `0000` terminated when shorter |
| `0x2E` | 2 | Width and height |
| `0x30..0x47` | 24 | Face/hair/eye/eyebrow/nose/mouth/etc. bitfields |
| `0x48` | `0x14` | Creator/author name, UTF-16, max 10 characters |

Because the record's default endianness is little-endian, its UTF-16 name fields are UTF-16LE in the raw Mii substructure unless a consuming API converts them. This is separate from the **Amiibo nickname**, which is UTF-16BE.

3dbrew's generic `CFLStoreData` is 0x60 bytes: the `0x5C` base record, 2 bytes padding, then a 2-byte CRC16. Amiibo stores the same pieces at settings `0x20`, `0x7C`, and `0x7E`.

There are two related public descriptions that should not be conflated. 3dbrew's generic `CFLStoreData` stores a CRC16 at offset `0x5E` after `0x5C` bytes of Mii data and two padding bytes, and identifies the family algorithm as CRC-16/XMODEM. Switchbrew describes the Amiibo service's Mii CRC operation as covering a `0x60`-byte Mii container with the final four bytes zeroed for that calculation. When generating Amiibo data, follow a tested Amiibo implementation or the exact Nintendo-service behavior being targeted rather than assuming that every generic Mii container is byte-for-byte interchangeable.

**Sources:** [3dbrew Mii](https://www.3dbrew.org/wiki/Mii), [Switchbrew NFC services](https://www.switchbrew.org/wiki/NFC_services).

### 6.5 Application/game area

The legacy Amiibo application area is `0xD8` / **216 bytes** at internal `0x0DC`. The NFC system service treats the payload as opaque game-specific data. Only one normal application area is active at a time, and games identify ownership with the settings application ID and Access ID.

Switch firmware can fill unused bytes with random data when setting a shorter application payload. Therefore "unused" bytes are not necessarily zero.

Switch 2 also exposes an **Extended Application Area** API for v3-era behavior. Public reverse engineering currently does not provide a complete, stable byte-for-byte mapping of that API to every raw v3 sector-1 byte, so do not equate it blindly with the legacy 216-byte area.

## 7. Amiibo key files

### 7.1 `unfixed-info.bin`

`unfixed-info.bin` is the conventional filename for the **80-byte data master-key descriptor**.

Its derived keys are used for mutable/unfixed Amiibo data: AES-CTR encryption/decryption of the encrypted data region and the data HMAC covering settings/application content and related metadata.

### 7.2 `locked-secret.bin`

`locked-secret.bin` is the conventional filename for the **80-byte tag master-key descriptor**.

Its derived HMAC key is used for the tag HMAC that authenticates fixed/tag identity material.

### 7.3 `key_retail.bin`

`key_retail.bin` is a common **de facto filename** used by Amiibo tools for the two 80-byte descriptors concatenated into one 160-byte file:

```text
offset 0x00..0x4F  (80 bytes) = unfixed-info.bin / data master descriptor
offset 0x50..0x9F  (80 bytes) = locked-secret.bin / tag master descriptor
```

Equivalent shell notation is:

```text
unfixed-info.bin || locked-secret.bin
```

Upstream `amiitool` does **not** require the filename `key_retail.bin`; its `-k` option accepts a key file containing the concatenated descriptors. Its README describes the key as the concatenation of "unfixed infos" then "locked secret". PyAmiibo independently documents each file as 80 bytes and the combined file as 160 bytes in the same order.

So these are **not three independent keys**:

- `unfixed-info.bin`: 80-byte **data** master descriptor;
- `locked-secret.bin`: 80-byte **tag** master descriptor;
- `key_retail.bin`: common name for the same two descriptors concatenated **data first, tag second**, for 160 bytes total.

The filenames are conventions, not cryptographic transformations. Concatenating the two 80-byte files does not derive a new key, and splitting a valid 160-byte combined file at offset `0x50` recovers the two descriptors. Reversing the order is not equivalent because the first descriptor is consumed as the data master and the second as the tag master. Other tools may call the same 160-byte blob `retail.bin`, `key.bin`, or similar.

**Sources:** [upstream `amiitool` README](https://github.com/socram8888/amiitool/blob/master/README.md), [PyAmiibo key documentation](https://github.com/tobywf/pyamiibo/blob/master/docs/keys.rst), [PyAmiibo `keys.py`](https://github.com/tobywf/pyamiibo/blob/master/amiibo/keys.py).

### 7.4 Exact 80-byte master-key descriptor

Upstream `amiitool` defines each packed descriptor as:

| Offset | Size | Field |
| ---: | ---: | --- |
| `0x00` | 16 | Base HMAC key |
| `0x10` | 14 | Type string |
| `0x1E` | 1 | RFU |
| `0x1F` | 1 | `magicBytesSize` |
| `0x20` | 16 | Magic-byte storage |
| `0x30` | 32 | XOR pad |

This totals 80 bytes. The combined `nfc3d_amiibo_keys` structure is `data` followed by `tag`.

**Source:** [upstream `amiitool` `include/nfc3d/keygen.h`](https://github.com/socram8888/amiitool/blob/master/include/nfc3d/keygen.h) and [`include/nfc3d/amiibo.h`](https://github.com/socram8888/amiitool/blob/master/include/nfc3d/amiibo.h).

## 8. Cryptographic derivation and packing

### 8.1 Base seed

Upstream `amiitool` forms a 64-byte key-generation seed from the internal/plain image:

```text
seed[0x00..0x01] = internal[0x029..0x02A]
seed[0x02..0x0F] = 14 zero bytes
seed[0x10..0x17] = internal[0x1D4..0x1DB]
seed[0x18..0x1F] = internal[0x1D4..0x1DB] again
seed[0x20..0x3F] = internal[0x1E8..0x207]
```

The final 32 bytes are per-tag key-generation material/salt and should be preserved for an existing tag. A new synthetic image needs unpredictable/appropriate bytes there; do not copy one fixed salt across every generated figure.

**Source:** [upstream `amiitool` `amiibo.c`](https://github.com/socram8888/amiitool/blob/master/amiibo.c).

### 8.2 Per-master seed preparation

For each 80-byte master descriptor, upstream `amiitool` prepares the derivation input by:

1. copying the descriptor's type string through its terminating NUL, bounded by the 14-byte field;
2. appending `16 - magicBytesSize` leading bytes from the 64-byte base seed;
3. appending `magicBytesSize` bytes from the descriptor's magic-byte field;
4. appending base seed `0x10..0x1F`;
5. appending base seed `0x20..0x3F` XORed with the descriptor's 32-byte XOR pad.

It then uses a custom HMAC-SHA256 counter generator: a 16-bit big-endian counter is prepended to the prepared seed for each HMAC output block. The first 48 generated bytes are interpreted as:

```text
16 bytes AES key
16 bytes AES IV / CTR initial counter
16 bytes HMAC key
```

It is more precise to call this the **amiitool HMAC-SHA256 counter/DRBG construction** than to claim it is a particular standardized HMAC-DRBG profile.

**Sources:** [upstream `amiitool` `keygen.c`](https://github.com/socram8888/amiitool/blob/master/keygen.c), [`drbg.c`](https://github.com/socram8888/amiitool/blob/master/drbg.c).

### 8.3 AES and HMAC coverage

Upstream `amiitool` uses:

- AES-128 in CTR mode;
- encrypted internal range starting at `0x02C`, length `0x188`;
- data HMAC stored at internal `0x008`;
- tag HMAC stored at internal `0x1B4`.

For **packing** a plain image:

1. derive tag and data key sets from the plain image;
2. calculate tag HMAC-SHA256 using the tag-derived HMAC key over `plain[0x1D4..0x207]` (`0x34` bytes); store it at internal `0x1B4`;
3. calculate the data HMAC-SHA256 using the data-derived key over the concatenation:
   - `plain + 0x029`, length `0x18B`;
   - the newly calculated 32-byte tag HMAC;
   - `plain + 0x1D4`, length `0x34`;
   store it at internal `0x008`;
4. AES-CTR encrypt internal `0x02C..0x1B3` (`0x188` bytes);
5. keep the noncipher regions as defined by `amiitool`;
6. reorder internal form back to raw RF form.

For **unpacking**, the inverse rearrangement/decryption is performed, then both HMACs are recomputed and compared.

**Primary implementation source:** [upstream `amiitool` `amiibo.c`](https://github.com/socram8888/amiitool/blob/master/amiibo.c).

## 9. Generating a v2 image

A generator can build from a carefully constructed plain template, but because some Nintendo fields remain undocumented, a known-good decrypted blank template is safer than assuming every unknown byte is zero.

### 9.1 Inputs

- lawful data/tag master-key descriptors, either separate 80-byte files or a 160-byte combined file;
- complete 8-byte v2 Amiibo ID ending in `02`;
- 7-byte NFC UID;
- optional registration nickname/Mii/settings;
- optional 216-byte game/application data and its metadata;
- 32-byte physical-tag originality signature if the 572-byte archive format requires one.

### 9.2 Recommended procedure

1. Create or load a `0x208`-byte internal cryptographic payload (or a 540-byte Switchbrew-style plain container whose final 20 bytes are handled separately as lock/config state).
2. Put header magic `A5` and suitable counters in the header.
3. Put the 8-byte Amiibo ID at internal `0x1DC`, the beginning of the 44-byte ID entry. Preserve the other 36 entry bytes from a valid template unless you have a documented reason to change them.
4. Populate the seven-byte UID in the RF/manufacturer representation and compute `BCC0`/`BCC1`.
5. Set the 32-byte key-generation salt/material at internal `0x1E8..0x207`.
6. Populate settings. If registered, encode the **Amiibo nickname** as UTF-16BE at settings `0x0C`; separately construct/preserve the Mii subrecord at settings `0x20`.
7. If game data is present, set the application-area flag, application ID/Access ID/counters, and the `0xD8` application bytes.
8. Recompute the Mii/settings CRC fields with a tested implementation if any covered fields changed.
9. Derive keys, calculate tag HMAC, calculate data HMAC, AES-CTR encrypt, and reorder to RF/raw form.
10. Install the intended NTAG215 CC, lock bytes, AUTH0/ACCESS, UID-derived PWD, and PACK.
11. Verify the first 540 bytes by running a complete unpack/HMAC verification.
12. Append the 32-byte `READ_SIG` value at file offset `0x21C`. Do not treat it as EEPROM or derive it from Nintendo master keys.

For programming an ordinary physical NTAG215, only step 12 is archive metadata: the actual chip provides its own manufacturer signature.

---

# Part II — v3 / NTAG I²C Plus 2K

## 10. Status of the v3 format

Public v3 support first became practical through reverse engineering around Kirby Air Riders figures. Pixl.js PR #381 reports real Switch 2 testing and defines a useful flat-file convention:

> a 2048-byte image containing sector 0 and sector 1 as seen by an NFC reader, with the expected SRAM response already present in the sector-0 SRAM window.

This is a **community interoperability convention**, not a Nintendo-published file format.

The physical tag behavior matches **NTAG I²C Plus 2K / NT3H2211** sufficiently that its memory map and NFC command set are the appropriate hardware reference.

**Sources:** [Pixl.js PR #381](https://github.com/solosky/pixl.js/pull/381), [NXP NTAG I²C Plus data sheet](https://www.nxp.com/docs/en/data-sheet/NT3H2111_2211.pdf).

## 11. 2048-byte v3 RF image

```text
file 0x000..0x3FF = sector 0, pages 00..FF
file 0x400..0x7FF = sector 1, pages 00..FF
```

The file is a **page-address-space image**. Sector 0 includes addresses that are EEPROM, registers, invalid/reserved gaps, and SRAM. Therefore `0x800` bytes does not mean 2048 bytes of ordinary writable EEPROM.

### 11.1 Sector 0 map

| Page(s) | Flat offset | NXP function |
| --- | ---: | --- |
| `00` | `0x000` | `UID0 UID1 UID2 UID3` |
| `01` | `0x004` | `UID4 UID5 UID6 SAK` |
| `02` | `0x008` | `ATQA0 ATQA1 LOCK0 LOCK1` |
| `03` | `0x00C` | Capability Container |
| `04..E1` | `0x010..0x387` | 888 bytes sector-0 user EEPROM |
| `E2` | `0x388` | Dynamic lock bytes |
| `E3` | `0x38C` | RFU RFU RFU AUTH0 |
| `E4` | `0x390` | ACCESS RFU RFU RFU |
| `E5` | `0x394` | PWD |
| `E6` | `0x398` | PACK RFU RFU |
| `E7` | `0x39C` | PT_I2C RFU RFU RFU |
| `E8..E9` | `0x3A0..0x3A7` | Persistent configuration registers |
| `EA..EB` | `0x3A8..0x3AF` | Invalid NFC address region |
| `EC..ED` | `0x3B0..0x3B7` | Session registers |
| `EE..EF` | `0x3B8..0x3BF` | Invalid NFC address region |
| `F0..FF` | `0x3C0..0x3FF` | 64-byte SRAM when pass-through mode maps it there; otherwise invalid at these addresses |

Unlike NTAG215, NTAG I²C Plus's first NFC pages do **not** contain BCC bytes in page memory. NXP maps the seven UID bytes directly across pages 0 and 1, followed by SAK, then ATQA and static lock bytes in page 2. ISO14443A anticollision still has its own cascade/BCC framing at the RF-protocol level.

### 11.2 Sector 1

On the 2K part, sector 1 pages `00..FF` are 1024 bytes of user EEPROM.

The total NFC-accessible user EEPROM is therefore:

```text
sector 0 pages 04..E1 = 888 bytes
sector 1 pages 00..FF = 1024 bytes
                          ----------
                          1912 bytes
```

NXP also states that the 2K **dynamic lock bytes cover 1864 data bytes**. That is not the total user-memory capacity: the dynamic lock mechanism starts after the first 48 user bytes, which are covered by static locking.

## 12. NTAG I²C Plus lock and protection model

### 12.1 Static lock bytes

Static locks are still page-2 bytes 2 and 3. As on NTAG21x, NFC writes OR new lock bits into old ones; once set via NFC they cannot be cleared through NFC. NXP notes that the I²C interface can clear these bits, which is a hardware distinction from a standalone NTAG215.

The static lock map follows the Type-2 arrangement covering the first 12 user/CC pages. A hardware-faithful NFC emulator should enforce these bits for writes.

### 12.2 2K dynamic lock bytes at E2

For NTAG I²C Plus 2K, NXP's dynamic-lock figure maps the lock bits across the user-memory pages after the first 48 user bytes. The part has **1912 bytes of NFC user EEPROM total** (`888` bytes in sector 0 pages `04..E1`, plus `1024` bytes in sector 1), while NXP describes **1864 data bytes** as covered by the dynamic-lock mechanism because the first 48 user bytes are covered by the static-lock mechanism. `E2` byte 0 and byte 1 contain individual lock bits; byte 2 contains block-lock bits; byte 3 reads `00`.

**E2 byte 0:**

| Bit | Global pages protected |
| ---: | --- |
| 0 | `16..47` |
| 1 | `48..79` |
| 2 | `80..111` |
| 3 | `112..143` |
| 4 | `144..175` |
| 5 | `176..207` |
| 6 | `208..225` |
| 7 | `256..271` |

**E2 byte 1:**

| Bit | Global pages protected |
| ---: | --- |
| 0 | `272..303` |
| 1 | `304..335` |
| 2 | `336..367` |
| 3 | `368..399` |
| 4 | `400..431` |
| 5 | `432..463` |
| 6 | `464..495` |
| 7 | `496..511` |

Here `global_page = sector * 256 + page`.

**E2 byte 2 block-lock bits:**

| Bit | Future lock configuration frozen for |
| ---: | --- |
| 0 | global pages `16..79` |
| 1 | `80..143` |
| 2 | `144..207` |
| 3 | `208..271` |
| 4 | `272..335` |
| 5 | `336..399` |
| 6 | `400..463` |
| 7 | `464..511` |

NXP describes these dynamic lock changes as irreversible from NFC, but reversible through I²C. Byte 3 reads `00`, unlike NTAG215 where page-82 byte 3 reads `BD`.

### 12.3 AUTH0 / ACCESS / sector-1 protection

At E3 byte 3, `AUTH0` defines where sector-0 password protection begins. NXP states that sector-0 password protection runs from `AUTH0` through `EB`; a value above `EB` disables it.

E4 byte 0 is `ACCESS`:

- bit 7 `NFC_PROT`: `0` protects writes only; `1` protects reads and writes;
- bit 6 RFU;
- bit 5 `NFC_DIS_SEC1`: `1` makes sector 1 inaccessible from NFC;
- bits 4-3 RFU;
- bits 2-0 `AUTHLIM`: negative-authentication attempt limit.

E7 byte 0 is `PT_I2C`. NXP defines it exactly as:

| Bit(s) | Field | Meaning |
| ---: | --- | --- |
| 7..4 | RFU | must be `0000b` |
| 3 | `2K_PROT` | `0`: sector 1 does not require password authentication; `1`: password authentication is required to access sector 1 |
| 2 | `SRAM_PROT` | `0`: pass-through/mirror SRAM is not password protected; `1`: authentication is required to access SRAM in pass-through/mirror mode |
| 1..0 | `I2C_PROT` | controls I2C-side access to the NFC-protected area |

`I2C_PROT` values are:

| Value | I2C access |
| ---: | --- |
| `00b` | entire user memory read/write |
| `01b` | unprotected area read/write; protected area read-only |
| `1Xb` | unprotected area read/write; protected area inaccessible |

NXP notes that the I2C interface still has read/write access to session registers and SRAM regardless of `I2C_PROT`, and configuration-page access is additionally governed by the register-lock mechanism. For NFC emulation, `2K_PROT` and `SRAM_PROT` are the relevant E7 bits; do not treat sector 1 or SRAM as globally accessible when the saved protection state says otherwise.

PWD/PACK readback is zero on real hardware.

## 13. v3 Amiibo cryptographic placement

The **base v2 cryptographic algorithm** should be referenced to upstream `socram8888/amiitool`. Upstream `amiitool` predates v3 and does not define a 2K mapping.

The public v3 extension used by Pixl.js modifies only the raw/internal placement of two regions while reusing the established **520-byte (`0x208`) Amiibo cryptographic payload**. Pixl's modified header separately distinguishes `NFC3D_AMIIBO_SIZE = 520` from `NTAG215_SIZE = 540`:

```text
internal 0x000..0x007 <- v3 raw 0x008..0x00F
internal 0x008..0x027 <- v3 raw 0x0C0..0x0DF
internal 0x028..0x04B <- v3 raw 0x010..0x033
internal 0x04C..0x1B3 <- v3 raw 0x0E0..0x247
internal 0x1B4..0x1D3 <- v3 raw 0x034..0x053
internal 0x1D4..0x1DB <- v3 raw 0x000..0x007
internal 0x1DC..0x207 <- v3 raw 0x054..0x07F
```

Compared with v2, the data-HMAC and main encrypted-body placements are shifted by `+0x40` in raw RF space, leaving raw `0x080..0x0BF` as an inserted 64-byte region.

The complete semantics of that inserted region are not publicly settled. Public generation code has zero-filled it for fresh synthetic images; a real dump should preserve it.

**Sources:** base algorithm: [upstream `amiitool` `amiibo.c`](https://github.com/socram8888/amiitool/blob/master/amiibo.c). The v3 raw/internal placement delta is documented by the [Pixl.js v3 support change, PR #381](https://github.com/solosky/pixl.js/pull/381); upstream `amiitool` itself predates v3.

> [!NOTE]
> Pixl.js is cited here only as the public source of the **v3 extension**. The underlying Amiibo crypto/keygen algorithm is sourced from `socram8888/amiitool`.

## 14. Known public v3 compatibility values

A public Pixl.js v3 generator has used these sector-0 bytes:

```text
E2: 01 00 FF 00
E3: 00 00 00 04
E4: 07 00 00 00
E5: 00 00 00 00
E6: 00 00 00 00
E7: 00 00 00 00
E8..EB: zero in that generated buffer before the session/config region
EC: 41 00 F8 48
ED: 08 01 29 00
EE..EF: 00 00 00 00
```

Treat this as a **known emulator/console compatibility recipe**, not as NXP factory defaults or a universal specification for genuine v3 figures. In particular:

- NXP defines E5/E6 as PWD/PACK, not generic zeros;
- EA/EB and EE/EF are invalid NFC address ranges;
- E8/E9 are persistent configuration pages, whereas EC/ED expose session-register values;
- a physical tag's session register values are dynamic, not persistent flat-file constants.

### 14.1 Session register interpretation

The session registers at EC/ED correspond to:

```text
EC byte0 NC_REG
EC byte1 LAST_NDEF_BLOCK
EC byte2 SRAM_MIRROR_BLOCK
EC byte3 WDT_LS
ED byte0 WDT_MS
ED byte1 I2C_CLOCK_STR / NEG_AUTH_REACHED status field
ED byte2 NS_REG
ED byte3 RFU
```

NXP's `NS_REG` bits are:

| Bit | Name | Meaning when set |
| ---: | --- | --- |
| 7 | `NDEF_DATA_READ` | configured NDEF data-read condition reached |
| 6 | `I2C_LOCKED` | memory arbitration locked to I²C |
| 5 | `RF_LOCKED` | memory arbitration locked to NFC |
| 4 | `SRAM_I2C_READY` | SRAM data ready for I²C |
| 3 | `SRAM_RF_READY` | SRAM data ready for NFC |
| 2 | `EEPROM_WR_ERR` | EEPROM write/erase high-voltage error |
| 1 | `EEPROM_WR_BUSY` | EEPROM write cycle in progress |
| 0 | `RF_FIELD_PRESENT` | RF field present |

For the console-compatibility path used publicly for v3, `SRAM_RF_READY` is synthesized/set while returning ED so the reader sees precomputed SRAM data as ready.

## 15. v3 SRAM and CRC convention

NXP hardware has a 64-byte SRAM used for I²C/NFC transfer. In pass-through mode, the NFC command `FAST_WRITE A6 F0 FF` writes all 64 bytes at sector-0 pages F0-FF. NXP warns that data is written directly into SRAM while it is received, so even a final CRC failure/NAK can leave received/corrupted bytes in SRAM.

Pixl.js' v3 flat-image convention takes a different **emulator compatibility** approach: the expected response is precomputed in the flat image's F0-FF window, and its emulator ACKs the console's FAST_WRITE while intentionally retaining the precomputed response.

Its synthetic blank response generation has used:

1. 64 bytes initialized to zero;
2. CRC-16/MCRF4XX over bytes `0..61`;
3. CRC stored big-endian in bytes `62..63`.

That CRC scheme is a property of the observed/higher-level v3 exchange, not the NFC-A frame CRC-A itself.

**Sources:** [NXP NTAG I²C Plus data sheet](https://www.nxp.com/docs/en/data-sheet/NT3H2111_2211.pdf), [Pixl.js v3 generator](https://github.com/solosky/pixl.js/blob/main/fw/application/src/amiibo_helper.c), [Pixl.js emulator](https://github.com/solosky/pixl.js/blob/main/fw/application/src/ntag/ntag_emu_v2.c).

## 16. Generating a v3 flat image

Because v3 is still reverse engineered, the safest generator is template-based.

### 16.1 Inputs

- lawful 80+80 or combined 160-byte Amiibo master-key material;
- full 8-byte v3 Amiibo ID ending in `03`;
- 7-byte NFC UID appropriate for the v3 image;
- known-good v3 blank/template bytes where available;
- optional settings/Mii/legacy game data;
- any required game-specific/extended sector-1 data or precomputed SRAM response.

### 16.2 Procedure

1. Start with a verified v3 2048-byte template if possible.
2. Construct the same `0x208`-byte internal Amiibo cryptographic payload used by the v2 crypto code; keep lock/config/session/SRAM bytes outside that payload.
3. Set the 8-byte ID at internal `0x1DC`; preserve the remainder of the 44-byte ID entry unless understood.
4. Set header, settings, Mii, application metadata, and per-tag keygen salt.
5. Derive keys and perform HMAC/AES packing using the upstream `amiitool` algorithm.
6. Place the packed content into the v3 raw map using the `+0x40` v3 mapping delta described above.
7. Preserve or initialize the inserted raw `0x080..0x0BF` bytes according to a known-good v3 template. Zeroing is a compatibility choice, not a proven universal rule.
8. Preserve/configure E2-E9 and EC-ED according to the target emulation model. Do not confuse persistent configuration registers with session-register state.
9. Populate F0-FF with the expected SRAM response if using the 2048-byte precomputed-response convention.
10. Preserve sector-1 bytes from a known-good image unless the target game's extended-data format is understood.
11. Validate the Amiibo-authenticated portion by inverse mapping, AES decrypt, and both HMAC checks.
12. During emulation, preserve acknowledged console writes. Do not reconstruct unknown sector-1 data from v2 assumptions.

A v3 file remains exactly `0x800` bytes. Unlike the chosen v2 archive convention, it does **not** append a 32-byte `READ_SIG` tail; `READ_SIG` remains a separate NFC command response.

---

# Part III — NFC emulation

## 17. ISO14443A and Type-2 fundamentals

A useful emulator needs a tag state machine, not only a byte array.

At a minimum it must model:

- REQA/WUPA activation;
- two-level anticollision/select for a seven-byte UID;
- ATQA and SAK;
- ACTIVE/HALT behavior;
- CRC-A/parity as required by the NFC stack;
- 4-bit Type-2 ACK/NAK responses;
- authentication state;
- lock/protection state where hardware fidelity matters;
- v3 sector-selection and session/SRAM state.

For both NTAG215 and NTAG I²C Plus, ATQA is `00 44` as a value, transmitted least-significant byte first on RF (`44` first), and SAK is `00`.

### 17.1 ACK/NAK values

NTAG215/21x and NTAG I²C Plus use 4-bit responses; `A` is ACK. Error-nibble meanings differ slightly by family.

For NTAG21x, NXP documents:

| Nibble | Meaning |
| ---: | --- |
| `A` | ACK |
| `0` | invalid argument/page |
| `1` | parity or CRC error |
| `4` | negative-password-attempt counter overflow/limit |
| `5` | EEPROM write error |

For NTAG I²C Plus, NXP documents:

| Nibble | Meaning |
| ---: | --- |
| `A` | ACK |
| `0` | invalid argument/page or wrong password |
| `1` | parity or CRC error |
| `3` | arbiter locked to I²C |
| `4` | negative `PWD_AUTH` limit reached |
| `7` | EEPROM write error |

## 18. NTAG215 command set for v2 emulation

NXP's relevant NTAG215 command set includes:

| Command | Opcode | Parameters | Normal response |
| --- | ---: | --- | --- |
| `REQA` | `26` (7-bit frame) | — | ATQA |
| `WUPA` | `52` (7-bit frame) | — | ATQA |
| Anticollision/Select CL1 | `93` | ISO14443A framing | UID cascade data / SAK progression |
| Anticollision/Select CL2 | `95` | ISO14443A framing | remaining UID / SAK |
| `HLTA` | `50 00` | CRC-A | enter HALT; no ordinary response |
| `GET_VERSION` | `60` | CRC-A | 8 version bytes |
| `READ` | `30` | start page | 16 bytes / 4 pages |
| `FAST_READ` | `3A` | start, end | all pages inclusive |
| `WRITE` | `A2` | page + 4 bytes | ACK/NAK |
| `COMPATIBILITY_WRITE` | `A0` | two-part sequence | ACK/NAK |
| `READ_CNT` | `39` | counter address `02` | 3-byte counter |
| `PWD_AUTH` | `1B` | 4-byte PWD | 2-byte PACK on success |
| `READ_SIG` | `3C` | address `00` | 32-byte ECC signature |

### 18.1 `GET_VERSION`

NTAG215's documented eight-byte response is:

```text
00 04 04 02 01 00 11 03
```

An Amiibo emulator should return this exact tag-family identity when emulating NTAG215.

### 18.2 `READ 30`

`READ <Addr>` returns four pages / 16 bytes.

In the initial unprotected state, NTAG215 accepts start pages `00..86`. A start above `86` yields NAK.

NTAG21x implements **roll-over** at the end of accessible memory. Thus for NTAG215, examples are conceptually:

```text
READ 84 -> pages 84,85,86,00
READ 85 -> pages 85,86,00,01
READ 86 -> pages 86,00,01,02
```

If read protection is enabled (`PROT=1`) and the tag is not authenticated:

- starting at or above `AUTH0` is NAK;
- starting below `AUTH0` rolls over before the protected boundary rather than exposing protected pages.

Reads of PWD/PACK locations return zero bytes, not the stored secrets.

### 18.3 `FAST_READ 3A`

`FAST_READ <Start> <End>` returns every page from Start through End inclusive. There is no chaining: the reader must have enough receive buffer.

For NTAG215, both requested bounds must be legal/accessible under the current protection state. If read protection is enabled and any requested page is at/above `AUTH0` before authentication, NXP specifies NAK.

PWD/PACK bytes still read as zeros.

### 18.4 `WRITE A2`

`WRITE <Addr> <4 bytes>` writes one page. NTAG215 accepts addresses `02..86` in the initial state. Locked or password-protected pages are rejected as specified by the lock/config state.

Specially:

- writing page 2 does not overwrite UID/BCC bytes 0-1; data bytes corresponding to lock bytes are ORed into static locks;
- lock-bit writes are effectively one-way over NFC;
- configuration write protection (`CFGLCK`) and AUTH0/PROT rules must be considered.

A hardware-faithful emulator should enforce these rules. A compatibility emulator that intentionally relaxes them should document that as an emulation deviation.

### 18.5 `COMPATIBILITY_WRITE A0`

This is a two-part legacy command. The reader first sends the command/address and receives ACK; then sends 16 data bytes. NXP specifies that only bytes `0..3` are programmed to the addressed page and explicitly requires bytes `0x04..0x0F` of the 16-byte transfer to be `00`. The remaining twelve bytes are therefore compatibility payload, not writes to three additional pages.

It obeys the same lock/password restrictions as normal writes, with tearing protection for lock/CC pages documented by NXP.

### 18.6 `READ_CNT 39 02`

NTAG215 has a 24-bit one-way NFC counter. The command is:

```text
39 02 <CRC-A>
```

and returns three counter bytes plus CRC-A at the RF framing layer.

If `NFC_CNT_PWD_PROT=1`, an unauthenticated `READ_CNT` returns NAK; after valid password authentication it returns the counter.

With the Amiibo-common `ACCESS=0x5F`, both counter-enable and counter-password-protection bits are set, so a faithful emulator should account for authentication before exposing the counter.

### 18.7 `PWD_AUTH 1B`

Request:

```text
1B PWD0 PWD1 PWD2 PWD3 <CRC-A>
```

On correct password, the chip enters authenticated state and returns the two-byte PACK. Amiibo commonly uses the UID-derived password and `PACK = 80 80` described earlier.

Authentication state ends according to the tag's RF/state-machine reset behavior; an emulator should reset it when the tag leaves the relevant session/field state.

### 18.8 `READ_SIG 3C 00`

Request:

```text
3C 00 <CRC-A>
```

Response: 32-byte chip-specific ECC originality signature. For the 572-byte convention in this document, answer with file bytes `0x21C..0x23B`.

The signature is NXP silicon identity material, not the Amiibo data HMAC and not derivable from `key_retail.bin`.

## 19. NTAG I²C Plus 2K command set for v3 emulation

NXP's NFC command overview for NTAG I²C Plus includes:

| Command | Opcode | Purpose |
| --- | ---: | --- |
| `REQA` / `WUPA` | `26` / `52` | activation |
| anticollision/select | ISO14443A | select 7-byte UID |
| `HLTA` | `50 00` | halt |
| `GET_VERSION` | `60` | identify device |
| `READ_SIG` | `3C` | 32-byte originality signature |
| `PWD_AUTH` | `1B` | password authentication |
| `READ` | `30` | four-page read |
| `FAST_READ` | `3A` | multi-page read |
| `WRITE` | `A2` | four-byte write |
| `FAST_WRITE` | `A6` | 64-byte SRAM write |
| `SECTOR_SELECT` | `C2` | choose 256-page sector |

Unlike NTAG215, the official NTAG I²C Plus command overview does **not** define NTAG21x `COMPATIBILITY_WRITE A0` or `READ_CNT 39` as part of this product's NFC command set.

### 19.1 UID, ATQA, and SAK

For NTAG I²C Plus:

```text
page 00: UID0 UID1 UID2 UID3
page 01: UID4 UID5 UID6 SAK
page 02: ATQA0 ATQA1 LOCK0 LOCK1
```

The seven UID bytes should be supplied to ISO14443A anticollision as the UID; protocol-level cascade/BCC is generated by the RF stack/state machine.

NXP specifies ATQA value `00 44` (least-significant byte `44` transmitted first) and SAK `00`.

### 19.2 `GET_VERSION`

The NTAG I²C Plus 2K response used by NXP/public v3 emulators is:

```text
00 04 04 05 02 02 15 03
```

### 19.3 `READ 30`

`READ` still returns 16 bytes / four pages, but the selected sector changes the page address space.

In the initial state, valid start pages include:

- sector 0: `00..E9`, plus session pages `EC..ED`;
- sector 1 on 2K: `00..FF`;
- SRAM addresses when pass-through mode makes them accessible.

NXP specifies an important difference from NTAG215 end-of-memory rollover: if `READ` starts in a **valid** region but the four-page response crosses an invalid region, bytes belonging to the invalid addresses are returned as `00`. If the start page itself is invalid, the tag returns NAK.

### 19.4 `FAST_READ 3A`

`FAST_READ <Start> <End>` is inclusive and restricted to the currently selected sector. `End >= Start` is required. NXP explicitly allows an entire 256-page sector to be requested in one command if the reader can buffer the resulting **1024 data bytes**; there is no protocol-level chaining for the response.

Valid initial start pages are the same categories as `READ`: sector-0 `00..E9` and `EC..ED`, sector-1 `00..FF`, and SRAM when enabled. A valid start that extends through an invalid area returns zero bytes for invalid addresses; an invalid start returns NAK.

### 19.5 `WRITE A2`

NXP accepts, in the initial state:

- sector 0 pages `02..E9`;
- sector 1 pages `00..FF` on 2K;
- SRAM addresses when pass-through mode is enabled.

Locked pages cannot be reprogrammed. Password protection and memory arbitration can also reject access. Pages `EC/ED` are session registers that NXP marks **read-only from NFC**; they are not valid `WRITE` targets even though `READ` may start there. The invalid gaps `EA/EB` and `EE/EF` are likewise not valid `WRITE` targets.

For page 2, a hardware-faithful emulator should preserve UID/ATQA manufacturer bytes and apply NFC lock-byte semantics only to the lock-byte portion.

### 19.6 `FAST_WRITE A6`

Physical NTAG I²C Plus defines exactly this SRAM operation; `F0` and `FF` are fixed command parameters, not arbitrary start/end addresses:

```text
A6 F0 FF <64 bytes> <CRC-A>
```

It writes the complete 64-byte SRAM in pass-through mode and responds with 4-bit ACK/NAK. NXP explicitly warns that bytes are written directly to SRAM during reception; if the final frame CRC is wrong, a NAK can be returned even though received/corrupted data remains in SRAM.

#### v3 compatibility convention

The public v3 Pixl.js emulator intentionally **does not overwrite** its precomputed F0-FF response when the console sends `FAST_WRITE F0 FF`; it ACKs the command and retains the prepared response. That is not the NXP hardware rule. It is an observed console-emulation strategy tied to the 2048-byte flat-image convention.

An emulator should choose explicitly between:

- **hardware-faithful mode**: write the supplied 64 bytes into SRAM and model ready/arbiter state;
- **known v3 compatibility mode**: ACK the request while keeping the precomputed console response in F0-FF.

### 19.7 `SECTOR_SELECT C2`

NXP's transaction is two packets:

**Packet 1**

```text
C2 FF <CRC-A>
```

`FF` is required by the NXP command format. The tag responds with normal 4-bit ACK.

**Packet 2**

```text
SecNo 00 00 00 <CRC-A>
```

The three trailing bytes are RFU and should be zero when issuing the command.

On success, the tag gives a **passive ACK**: no reply for more than 1 ms. NXP states that any reply in less than 1 ms is interpreted as an error for packet 2.

For a 2K v3 image, sectors `0` and `1` are the meaningful memory sectors. The NXP command field itself is specified as `00..FE`.

A common emulator bug is to feed packet 2 to the ordinary opcode decoder. Maintain an explicit `waiting_for_sector_packet_2` state and consume that raw frame before normal command parsing.

### 19.8 `PWD_AUTH 1B`

Physical hardware compares the supplied 32-bit PWD to E5 and returns E6's two-byte PACK on success, subject to AUTHLIM and protection configuration. Reads of E5/E6 return zeros.

Public v3 console-compatibility emulation has returned:

```text
80 80
```

for the authentication exchange even when a generated flat file represents E5/E6 as zeros. That is an emulator convenience/compatibility behavior, not a statement that genuine NTAG I²C Plus ignores its password.

### 19.9 `READ_SIG 3C 00`

NTAG I²C Plus also supports `READ_SIG 3C 00` and returns a 32-byte ECC originality signature programmed at chip production.

The 2048-byte flat v3 convention does **not** place this signature at a defined extra file tail. An emulator needs to source/synthesize its `READ_SIG` response separately from the `0x800` sector image.

### 19.10 Session registers and SRAM-ready behavior

In I²C-to-NFC pass-through, NXP describes `SRAM_RF_READY` as becoming set when the I²C side completes the terminator-block write. After NFC `READ`/`FAST_READ` consumes the SRAM through the terminator page, `SRAM_RF_READY` and `RF_LOCKED` are cleared so the I²C side may write again.

The public Switch 2/v3 compatibility implementation observes that the console polls ED/NS_REG before reading F0-FF. It therefore sets/synthesizes NS_REG bit 3 (`0x08`) in responses to indicate that the precomputed SRAM response is ready.

A full hardware emulator should model transitions; a simplified v3 console emulator can expose the state needed by the reader as long as it is internally consistent.

### 19.11 Persistent versus volatile state

Do not persist everything in sector-0 page-address space as if it were EEPROM:

- ordinary user EEPROM and persistent configuration are nonvolatile;
- session registers EC/ED are volatile/session state;
- SRAM is volatile on real hardware;
- EA/EB and EE/EF are invalid NFC addresses.

A 2048-byte emulator container may store representative bytes for all these addresses for convenience, but that storage convention does not change physical semantics.

## 20. Lock enforcement versus compatibility

There are two defensible emulator goals:

### Hardware-faithful

- enforce static and dynamic lock bits;
- enforce configuration locks and password protection;
- update NFC counter/state as the chip does;
- model SRAM/session register transitions;
- reject writes the silicon would reject.

### Console-compatibility / image-preserving

Some reverse-engineered v3 readers write complete logical images even when saved lock fields would make a literal silicon model awkward. Public working v3 emulators have therefore used relaxed write handling and special SRAM responses.

If using this mode:

- ACK only operations your target workflow is known to accept;
- once an ordinary persistent write has been ACKed, preserve it rather than later rolling it back because an unrelated probe is unknown;
- keep compatibility deviations clearly separate from claims about NXP hardware.

This distinction is especially important when interpreting v3 `FAST_WRITE`, PWD/PACK zeros in a flat image, and lock-byte values.

## 21. RF field/session lifecycle

On a new RF activation, initialize transient state such as:

- selected sector = 0;
- authentication = false;
- pending `SECTOR_SELECT` packet-2 state = false;
- volatile register/SRAM state according to the chosen model.

On RF field-off:

- end authentication/session state;
- reset selected sector for the next activation;
- persist acknowledged EEPROM-like writes if the emulator is backed by a file;
- do not confuse volatile session-register/SRAM state with permanent tag content unless the chosen flat-file convention deliberately snapshots it.

For timing-sensitive NFC implementations, avoid filesystem writes and verbose log formatting inside the receive callback. Defer persistence to field-off or another safe transport boundary.

---

# Part IV — Validation

## 22. v2 validation checklist

A 572-byte v2 image should satisfy:

- exact file size `0x23C`;
- first `0x21C` bytes parse as NTAG215 pages `00..86`;
- UID/BCC are consistent;
- CC, static locks, dynamic locks, AUTH0/ACCESS are intentional;
- UID-derived PWD and PACK policy are coherent;
- header magic is `A5`;
- Amiibo ID is correct and version byte is `02` for v2;
- ID entry's unknown bytes are preserved/intentional;
- raw-to-internal conversion succeeds;
- AES decryption produces a structurally valid plain image;
- tag HMAC verifies;
- data HMAC verifies;
- nickname encoding is UTF-16BE if used;
- Mii record uses its own documented endian/layout rules;
- Mii/settings CRCs are coherent if related fields changed;
- application metadata matches whether an application area exists;
- final 32 bytes are treated only as `READ_SIG` metadata, never EEPROM.

## 23. v3 validation checklist

A 2048-byte v3 image should satisfy:

- exact file size `0x800`;
- sector 0 and sector 1 are each exactly `0x400` bytes;
- page-0/1 UID layout follows NTAG I²C Plus representation, not NTAG215 BCC-in-memory representation;
- Amiibo ID is correct and version byte is `03` for current v3 figures;
- the `+0x40` raw placement shift is accounted for;
- unknown inserted region and ID-entry bytes are preserved unless deliberately generated;
- Amiibo crypto maps back to internal form and both HMACs verify;
- E2-E9 values are intentional and are not mislabeled as factory defaults;
- EC/ED behavior presented by the emulator is coherent with its chosen session model;
- F0-FF contains the expected SRAM state/response for the chosen compatibility model;
- `SECTOR_SELECT` packet 2 receives passive ACK behavior;
- sector-1 reads/writes map to flat offsets `0x400..0x7FF`;
- `READ_SIG` is handled separately from the `0x800` file;
- successful writes to unknown v3/extended regions are preserved rather than regenerated from v2 assumptions.

---

# Fact-check methodology and corrections

This revision was checked claim-by-claim against the source layer appropriate to each statement:

- **NXP data sheets** for physical memory maps, lock granularity, configuration fields, valid command addresses, ACK/NAK behavior, authentication, session registers, SRAM, and sector selection;
- **Switchbrew / 3dbrew** for Nintendo's reverse-engineered Amiibo settings, ID, application-area and Mii integration;
- **upstream `socram8888/amiitool`** for the established v2 `0x208` crypto payload, raw/internal mapping, seed construction, HMAC ordering, AES-CTR and master-descriptor format;
- **PyAmiibo** as an independent check of the two 80-byte key-file roles and concatenation order;
- **Proxmark3** as an independent check of the UID-derived Amiibo PWD and `0x8080` PACK convention;
- **Pixl.js v3 work** only where v3 is genuinely newer than upstream amiitool: the `0x800` flat-image convention, `+0x40` raw mapping delta, and empirically compatible Switch 2 NFC behavior.

Corrections made during this pass include:

1. distinguishing upstream amiitool's **520-byte (`0x208`) crypto payload** from the 540-byte NTAG215/Switchbrew representation;
2. distinguishing the I²C Plus 2K part's **1912 total user EEPROM bytes** from the **1864 bytes covered by dynamic locks**;
3. removing unsupported claims that Switchbrew `Unknown2` is a defined v3 payload field; current Switchbrew specifically notes write paths that zero it for v3;
4. keeping the embedded Mii's UTF-16 fields under the 3dbrew **little-endian** default, while the Amiibo nickname is independently documented as **UTF-16BE**;
5. distinguishing NXP hardware command semantics from Pixl-style v3 interoperability behaviors such as `PWD_AUTH -> 80 80` and ACK-without-overwrite `FAST_WRITE`;
6. making invalid I²C Plus address ranges and NFC-read-only session registers explicit instead of treating every sector-0 page as ordinary EEPROM.

Where public sources disagree or remain incomplete, the text now labels the point as uncertain rather than presenting it as established fact.

---

# Sources and further reading

The references below are separated by layer so it is clear which claims come from silicon documentation, Nintendo reverse engineering, original Amiibo crypto tooling, or newer v3 interoperability work.

## NXP hardware documentation

1. **NXP — NTAG213/215/216 data sheet**  
   <https://www.nxp.com/docs/en/data-sheet/NTAG213_215_216.pdf>  
   Primary source for NTAG215 memory, static/dynamic locks, configuration, PWD/PACK, NFC counter, command frames, ACK/NAK, `GET_VERSION`, `READ_SIG`, and protection behavior.

2. **NXP — NTAG I²C Plus (`NT3H2111/NT3H2211`) data sheet**  
   <https://www.nxp.com/docs/en/data-sheet/NT3H2111_2211.pdf>  
   Primary source for v3's underlying 2K tag model: sector map, UID/SAK/ATQA page layout, static/dynamic locks, password/access configuration, persistent/session registers, NS_REG, SRAM, `FAST_WRITE`, and `SECTOR_SELECT`.

## Nintendo-format reverse engineering

3. **Switchbrew — NFC services**  
   <https://www.switchbrew.org/wiki/NFC_services>  
   Current Switch/Switch 2 reverse engineering for raw/plain layouts, header/settings/ID fields, nickname, Mii integration, application area, v3-aware firmware changes, and Extended Application Area APIs.

4. **3dbrew — Amiibo**  
   <https://www.3dbrew.org/wiki/Amiibo>  
   Classic Amiibo page/data observations and 3DS-era settings/application behavior.

5. **3dbrew — Mii**  
   <https://www.3dbrew.org/wiki/Mii>  
   `0x5C` Mii format, default little-endian interpretation, UTF-16 fields, appearance bitfields, and CFLStoreData layout.

## Original/public Amiibo cryptographic implementation

6. **socram8888/amiitool — upstream repository**  
   <https://github.com/socram8888/amiitool>  
   Original public Amiibo encryption/decryption/copy implementation used as the reference here for the established v2 cryptographic algorithm.

7. **upstream `amiitool` — `amiibo.c`**  
   <https://github.com/socram8888/amiitool/blob/master/amiibo.c>  
   Raw/internal mapping, key-seed construction, AES-CTR region, HMAC coverage/order, pack/unpack behavior.

8. **upstream `amiitool` — `keygen.c`**  
   <https://github.com/socram8888/amiitool/blob/master/keygen.c>  
   Master-descriptor seed preparation.

9. **upstream `amiitool` — `drbg.c`**  
   <https://github.com/socram8888/amiitool/blob/master/drbg.c>  
   HMAC-SHA256 counter/DRBG output generation.

10. **upstream `amiitool` — `keygen.h` / `amiibo.h`**  
    <https://github.com/socram8888/amiitool/blob/master/include/nfc3d/keygen.h>  
    <https://github.com/socram8888/amiitool/blob/master/include/nfc3d/amiibo.h>  
    Packed 80-byte master descriptor and data-then-tag combined-key structure.

11. **PyAmiibo — key documentation**  
    <https://github.com/tobywf/pyamiibo/blob/master/docs/keys.rst>  
    Independent documentation of `unfixed-info.bin` as the 80-byte data master, `locked-secret.bin` as the 80-byte tag master, and concatenation order.

12. **PyAmiibo — `keys.py`**  
    <https://github.com/tobywf/pyamiibo/blob/master/amiibo/keys.py>  
    Independent parser/validation of the 80-byte files and 160-byte combined file.

13. **Proxmark3 — `common/generator.c`**  
    <https://github.com/RfidResearchGroup/proxmark3/blob/master/common/generator.c>  
    Independent Amiibo UID-derived PWD formula and `0x8080` PACK convention.

## v3 reverse engineering and interoperability

14. **Pixl.js PR #381 — v3 support**  
    <https://github.com/solosky/pixl.js/pull/381>  
    Primary public source for the 2048-byte sector0+sector1 flat-image convention and reported real Switch 2 emulation/save/load testing.

15. **Pixl.js — v3 NFC emulator**  
    <https://github.com/solosky/pixl.js/blob/main/fw/application/src/ntag/ntag_emu_v2.c>  
    Public interoperability behavior for v3 sector select, SRAM-ready polling, `PWD_AUTH`, and precomputed-SRAM `FAST_WRITE` handling.

16. **Pixl.js — v3 image generator**  
    <https://github.com/solosky/pixl.js/blob/main/fw/application/src/amiibo_helper.c>  
    Public v3 construction recipe for the raw `+0x40` insertion, compatibility register values, and SRAM response CRC.

17. **AmiiboAPI issue #243 — early public v3 research**  
    <https://github.com/N3evin/AmiiboAPI/issues/243>  
    Community discussion around early v3 figures/IDs.

## 24. Fact-check status and known uncertainties

This revision deliberately removes or narrows claims that are not adequately established.

| Topic | Status |
| --- | --- |
| NTAG215 physical command/lock/config behavior | **High confidence** — NXP data sheet |
| NTAG I²C Plus 2K page/command/lock/register behavior | **High confidence** — NXP data sheet |
| v2 raw/internal mapping and crypto | **High confidence** — upstream amiitool + Nintendo RE |
| `unfixed-info.bin` / `locked-secret.bin` roles and 80+80 order | **High confidence** — amiitool structure + independent PyAmiibo docs |
| `key_retail.bin` filename | **Convention**, not a distinct third key or a required upstream amiitool filename |
| v2 nickname/settings/application offsets | **High confidence** — Switchbrew/3dbrew |
| Mii base layout | **High confidence** for offsets documented by 3dbrew; use its endian rules |
| 2048-byte v3 container | **Medium-high** — public implementation tested on real Switch 2; not an official Nintendo container spec |
| v3 raw `+0x40` mapping | **Medium-high** — public interoperable implementation |
| Meaning of raw v3 `0x080..0x0BF` | **Uncertain** — preserve real/template data |
| Settings `Unknown2` | **Uncertain** — current Switchbrew notes v3 write paths zero it; do not label it a known v3 payload |
| Exact mapping of Switch 2 Extended Application Area to raw sector 1 | **Uncertain/incomplete publicly** — preserve unknown bytes |
| Pixl-style ACK-and-ignore `FAST_WRITE` | **Compatibility convention**, explicitly different from NXP hardware SRAM semantics |
| Pixl-style `PWD_AUTH -> 80 80` with zero flat PWD/PACK | **Compatibility convention**, not genuine-chip password behavior |
