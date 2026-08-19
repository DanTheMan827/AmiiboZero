# Third-party notices

## lwJSON

Amiibo Zero vendors a minimal streaming-only subset of **lwJSON** by Tilen MAJERLE.

- Upstream: `MaJerle/lwjson`
- Version: 1.9.0
- Integration base commit: `be2b042fae1401957dcc01860532e15b40d3eb66`
- License: MIT
- Bundled: streaming parser API/types and streaming parser implementation
- Not bundled: DOM/token parser, serializer, debug helpers, and unrelated utilities

The vendored files retain the MIT license notice.

### MIT License

Copyright (c) 2024 Tilen MAJERLE

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## pixl.js research reference

`solosky/pixl.js` (GPL-2.0) was reviewed as a behavioral/interoperability reference for newer NTAG I2C Plus 2K Amiibo behavior. **No pixl.js source files are bundled or linked into Amiibo Zero.** The app's v3 implementation is separately written against observed format/layout behavior and the Flipper NFC model.

## AmiiTag / TagWallet research reference

`DanTheMan827/AmiiTag` and its `DanTheMan827/TagWallet` dependency were reviewed as behavioral references for physical NTAG215 Amiibo programming, especially destination-UID rebinding and the conventional page/lock write sequence. TagWallet is MIT-licensed. **No source files from either project are bundled or linked into Amiibo Zero.**

## TagMo research reference

`HiddenRamblings/TagMo` was reviewed as an interoperability reference for blank-tag validation and the writable page ranges used when restoring state on an already locked Amiibo. **No TagMo source files are bundled or linked into Amiibo Zero.**
