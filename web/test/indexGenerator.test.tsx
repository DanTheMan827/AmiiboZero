import test from 'node:test';
import assert from 'node:assert/strict';
import {crc32 as zlibCrc32} from 'node:zlib';
import {readFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {
    INDEX_FORMAT,
    flipperCrc32,
    generateAmiiboIndex,
    nativeFieldBytes,
    sourceStamp,
} from '../src/lib/indexGenerator';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function cString(bytes: Uint8Array): string {
    const end = bytes.indexOf(0);
    return decoder.decode(end >= 0 ? bytes.subarray(0, end) : bytes);
}

const amiiboObject = {
    amiibo_series: {
        '02': 'Zelda',
        '01': 'Café',
    },
    amiibos: {
        '0102030405060202': {name: 'beta', release: {na: '2024-01-02'}},
        '0102030405060103': {name: 'Alpha (& Warp Star)', release: {na: '2025-02-03'}},
        '0102030505060102': {name: 'éclair', release: {na: '2026-03-04'}},
    },
};
const gamesObject = {
    amiibos: {
        '0102030405060103': {Switch: [{gameName: 'Test', usage: [{Usage: 'Does something', write: false}]}]},
        '0102030405060202': {Switch: [{gameName: 'Other'}]},
    },
};
const amiiboText = JSON.stringify(amiiboObject);
const gamesText = JSON.stringify(gamesObject);
const amiiboBytes = encoder.encode(amiiboText);
const gamesBytes = encoder.encode(gamesText);

test('Flipper CRC32 implementation matches canonical standard CRC32 chaining', () => {
    const data = encoder.encode('Amiibo Zero');
    assert.equal(flipperCrc32(0, data), zlibCrc32(data));
    const first = data.subarray(0, 5);
    const second = data.subarray(5);
    assert.equal(flipperCrc32(flipperCrc32(0, first), second), zlibCrc32(second, zlibCrc32(first)));
});

test('native text encoding truncates by UTF-8 byte capacity, not JS character count', () => {
    const encoded = nativeFieldBytes('é'.repeat(40), 64);
    assert.equal(encoded.length, 63);
    assert.equal(encoded[62], 0xc3); // native byte-wise truncation can end mid-codepoint
    assert.deepEqual(nativeFieldBytes('a\n\tb\f', 64), encoder.encode('a  b '));
});

test('browser index uses the native ARM v11 binary layout and exact JSON ranges', () => {
    const generated = generateAmiiboIndex(amiiboObject, amiiboBytes, gamesObject, gamesBytes);
    const view = new DataView(generated.bytes.buffer);
    assert.equal(cString(generated.bytes.subarray(0, 8)), INDEX_FORMAT.magic);
    assert.equal(view.getUint16(8, true), 11);
    assert.equal(view.getUint16(10, true), 80);
    assert.equal(view.getUint16(12, true), 8);
    assert.equal(view.getUint16(14, true), 112);
    assert.equal(view.getUint16(16, true), 16);
    assert.equal(view.getUint32(56, true), 3);
    assert.equal(view.getUint32(60, true), 2);
    assert.equal(view.getUint16(64, true), 2);
    assert.equal(view.getUint32(68, true), 80);

    const figuresOffset = view.getUint32(72, true);
    const gamesOffset = view.getUint32(76, true);
    let cursor = 80;
    const firstNameLength = generated.bytes[cursor + 1];
    assert.equal(generated.bytes[cursor], 0x01);
    assert.equal(decoder.decode(generated.bytes.subarray(cursor + 8, cursor + 8 + firstNameLength)), 'Café');
    assert.equal(view.getUint16(cursor + 2, true), 2);
    assert.equal(view.getUint32(cursor + 4, true), 0);
    cursor += 8 + firstNameLength;
    const secondNameLength = generated.bytes[cursor + 1];
    assert.equal(generated.bytes[cursor], 0x02);
    assert.equal(decoder.decode(generated.bytes.subarray(cursor + 8, cursor + 8 + secondNameLength)), 'Zelda');
    assert.equal(view.getUint32(cursor + 4, true), 2);
    assert.equal(cursor + 8 + secondNameLength, figuresOffset);

    const firstFigure = generated.bytes.subarray(figuresOffset, figuresOffset + 112);
    assert.equal(cString(firstFigure.subarray(25, 89)), 'Alpha '); // v3 suffix stripped exactly like native
    assert.equal(firstFigure[101], 0x01);
    const firstJsonOffset = new DataView(firstFigure.buffer, firstFigure.byteOffset).getUint32(104, true);
    const firstJsonLength = new DataView(firstFigure.buffer, firstFigure.byteOffset).getUint32(108, true);
    assert.deepEqual(JSON.parse(decoder.decode(amiiboBytes.subarray(firstJsonOffset, firstJsonOffset + firstJsonLength))), amiiboObject.amiibos['0102030405060103']);

    const secondFigure = generated.bytes.subarray(figuresOffset + 112, figuresOffset + 224);
    assert.equal(cString(secondFigure.subarray(25, 89)), 'éclair');
    const thirdFigure = generated.bytes.subarray(figuresOffset + 224, figuresOffset + 336);
    assert.equal(cString(thirdFigure.subarray(25, 89)), 'beta');
    assert.equal(gamesOffset, figuresOffset + 3 * 112);

    const firstGameOffset = view.getUint32(gamesOffset + 8, true);
    const firstGameLength = view.getUint32(gamesOffset + 12, true);
    assert.deepEqual(JSON.parse(decoder.decode(gamesBytes.subarray(firstGameOffset, firstGameOffset + firstGameLength))), gamesObject.amiibos['0102030405060103']);
});

test('source stamps sample the exact minified bytes with little-endian sample offsets', () => {
    const stamp = sourceStamp(amiiboBytes);
    assert.equal(stamp.size, amiiboBytes.length);
    let expected = 0;
    const offsets = amiiboBytes.length > 256 ? [0, Math.floor((amiiboBytes.length - 256) / 2), amiiboBytes.length - 256] : [0];
    for (const offset of [...new Set(offsets)]) {
        const offsetBytes = new Uint8Array(4);
        new DataView(offsetBytes.buffer).setUint32(0, offset, true);
        expected = zlibCrc32(offsetBytes, expected);
        expected = zlibCrc32(amiiboBytes.subarray(offset, Math.min(amiiboBytes.length, offset + 256)), expected);
    }
    assert.equal(stamp.sampleCrc32, expected >>> 0);
});

test('web constants are guarded by explicit native layout assertions', async () => {
    const nativeDb = await readFile(resolve(process.cwd(), '../src/amiibo_db.c'), 'utf8');
    const nativeHeader = await readFile(resolve(process.cwd(), '../src/amiibo_zero.h'), 'utf8');
    assert.match(nativeDb, /#define AZ_INDEX_MAGIC "AZIDX34"/);
    assert.match(nativeDb, /#define AZ_INDEX_VERSION 11U/);
    assert.match(nativeDb, /sizeof\(AzIndexHeader\) == 80U/);
    assert.match(nativeDb, /sizeof\(AzIndexFigureRecord\) == 112U/);
    assert.match(nativeDb, /sizeof\(AzIndexGameRef\) == 16U/);
    assert.match(nativeHeader, /#define AZ_NAME_MAX 64/);
});
