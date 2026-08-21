const encoder = new TextEncoder();
const decoder = new TextDecoder();

export const INDEX_FORMAT = Object.freeze({
    magic: 'AZIDX34',
    version: 11,
    headerSize: 80,
    categoryPrefixSize: 8,
    figureRecordSize: 112,
    gameRefSize: 16,
    nameSize: 64,
    releaseSize: 12,
    maxCategories: 96,
});

type JsonObject = Record<string, unknown>;

type JsonMemberRange = {
    key: string;
    valueStart: number;
    valueEnd: number;
};

type SourceStamp = {
    size: number;
    sampleCrc32: number;
};

type CategoryBuild = {
    id: number;
    name: Uint8Array;
    count: number;
};

type FigureBuild = {
    id: Uint8Array;
    idHex: string;
    name: Uint8Array;
    releaseNa: Uint8Array;
    category: number;
    type: number;
    jsonOffset: number;
    jsonLength: number;
};

type GameRefBuild = {
    pattern: Uint8Array;
    jsonOffset: number;
    jsonLength: number;
};

export type GeneratedIndex = {
    bytes: Uint8Array;
    figureCount: number;
    categoryCount: number;
    gameRefCount: number;
    amiiboStamp: SourceStamp;
    gamesStamp: SourceStamp;
};

function isObject(value: unknown): value is JsonObject {
    return Boolean(value) && typeof value === 'object' && !Array.isArray(value);
}

function skipWhitespace(bytes: Uint8Array, position: number): number {
    while (position < bytes.length) {
        const byte = bytes[position];
        if (byte !== 0x20 && byte !== 0x09 && byte !== 0x0a && byte !== 0x0d) break;
        position += 1;
    }
    return position;
}

function parseStringEnd(bytes: Uint8Array, start: number): number {
    if (bytes[start] !== 0x22) throw new Error(`Expected JSON string at byte ${start}.`);
    let escaped = false;
    for (let position = start + 1; position < bytes.length; position += 1) {
        const byte = bytes[position];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (byte === 0x5c) {
            escaped = true;
            continue;
        }
        if (byte === 0x22) return position + 1;
    }
    throw new Error(`Unterminated JSON string at byte ${start}.`);
}

function parseValueEnd(bytes: Uint8Array, start: number): number {
    start = skipWhitespace(bytes, start);
    if (start >= bytes.length) throw new Error('Unexpected end of JSON while locating a value.');

    const first = bytes[start];
    if (first === 0x22) return parseStringEnd(bytes, start);

    if (first === 0x7b || first === 0x5b) {
        const stack: number[] = [first];
        let inString = false;
        let escaped = false;
        for (let position = start + 1; position < bytes.length; position += 1) {
            const byte = bytes[position];
            if (inString) {
                if (escaped) escaped = false;
                else if (byte === 0x5c) escaped = true;
                else if (byte === 0x22) inString = false;
                continue;
            }
            if (byte === 0x22) {
                inString = true;
                continue;
            }
            if (byte === 0x7b || byte === 0x5b) {
                stack.push(byte);
                continue;
            }
            if (byte === 0x7d || byte === 0x5d) {
                const expected = byte === 0x7d ? 0x7b : 0x5b;
                if (stack.pop() !== expected) throw new Error(`Mismatched JSON delimiter at byte ${position}.`);
                if (stack.length === 0) return position + 1;
            }
        }
        throw new Error(`Unterminated JSON container at byte ${start}.`);
    }

    let position = start;
    while (position < bytes.length) {
        const byte = bytes[position];
        if (byte === 0x2c || byte === 0x7d || byte === 0x5d || byte === 0x20 || byte === 0x09 || byte === 0x0a || byte === 0x0d) break;
        position += 1;
    }
    if (position === start) throw new Error(`Invalid JSON value at byte ${start}.`);
    return position;
}

function decodeJsonKey(bytes: Uint8Array, start: number, end: number): string {
    return JSON.parse(decoder.decode(bytes.subarray(start, end))) as string;
}

function objectMembers(bytes: Uint8Array, objectStart: number): JsonMemberRange[] {
    let position = skipWhitespace(bytes, objectStart);
    if (bytes[position] !== 0x7b) throw new Error(`Expected JSON object at byte ${position}.`);
    position += 1;

    const members: JsonMemberRange[] = [];
    while (true) {
        position = skipWhitespace(bytes, position);
        if (bytes[position] === 0x7d) return members;

        const keyStart = position;
        const keyEnd = parseStringEnd(bytes, keyStart);
        const key = decodeJsonKey(bytes, keyStart, keyEnd);
        position = skipWhitespace(bytes, keyEnd);
        if (bytes[position] !== 0x3a) throw new Error(`Expected ':' after JSON key ${JSON.stringify(key)}.`);
        position = skipWhitespace(bytes, position + 1);

        const valueStart = position;
        const valueEnd = parseValueEnd(bytes, valueStart);
        members.push({key, valueStart, valueEnd});

        position = skipWhitespace(bytes, valueEnd);
        if (bytes[position] === 0x2c) {
            position += 1;
            continue;
        }
        if (bytes[position] === 0x7d) return members;
        throw new Error(`Expected ',' or '}' at byte ${position}.`);
    }
}

function containerObjectMembers(bytes: Uint8Array, containerKey: string): JsonMemberRange[] {
    const rootStart = skipWhitespace(bytes, 0);
    const rootMembers = objectMembers(bytes, rootStart);
    const container = rootMembers.find((member) => member.key === containerKey);
    if (!container || bytes[container.valueStart] !== 0x7b) {
        throw new Error(`JSON source does not contain an object named ${containerKey}.`);
    }
    return objectMembers(bytes, container.valueStart);
}

function normalizeNativeString(value: string): string {
    let result = '';
    for (let index = 0; index < value.length; index += 1) {
        const codeUnit = value.charCodeAt(index);
        if (codeUnit === 0) break;
        if (codeUnit === 0x08 || codeUnit === 0x0c || codeUnit === 0x0a || codeUnit === 0x0d || codeUnit === 0x09) {
            result += ' ';
            continue;
        }
        if (codeUnit >= 0xd800 && codeUnit <= 0xdbff) {
            const next = index + 1 < value.length ? value.charCodeAt(index + 1) : 0;
            if (next >= 0xdc00 && next <= 0xdfff) {
                result += value[index] + value[index + 1];
                index += 1;
            }
            continue;
        }
        if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff) continue;
        result += value[index];
    }
    return result;
}

/** Encode exactly as the native streaming decoder stores a bounded C string. */
export function nativeFieldBytes(value: unknown, capacity: number): Uint8Array {
    if (typeof value !== 'string' || capacity <= 1) return new Uint8Array(0);
    const encoded = encoder.encode(normalizeNativeString(value));
    return encoded.length < capacity ? encoded : encoded.slice(0, capacity - 1);
}

function stripV3Suffix(name: Uint8Array, id: Uint8Array): Uint8Array {
    if (id[7] !== 0x03 || name.length === 0 || name[name.length - 1] !== 0x29) return name;
    for (let index = 0; index + 1 < name.length; index += 1) {
        if (name[index] === 0x28 && name[index + 1] === 0x26) return name.slice(0, index);
    }
    return name;
}

function parseHexId(text: string): Uint8Array | null {
    let normalized = text;
    if (/^0x/i.test(normalized)) normalized = normalized.slice(2);
    if (!/^[0-9a-fA-F]{16}$/.test(normalized)) return null;
    const result = new Uint8Array(8);
    for (let index = 0; index < 8; index += 1) {
        result[index] = Number.parseInt(normalized.slice(index * 2, index * 2 + 2), 16);
    }
    return result;
}

function parseHexByte(text: string): number | null {
    let normalized = text;
    if (/^0x/i.test(normalized)) normalized = normalized.slice(2);
    if (!/^[0-9a-fA-F]{2}$/.test(normalized)) return null;
    return Number.parseInt(normalized, 16);
}

function canonicalIdHex(id: Uint8Array): string {
    return Array.from(id, (value) => value.toString(16).padStart(2, '0')).join('');
}

function compareBytesNativeNoCase(left: Uint8Array, right: Uint8Array): number {
    const count = Math.min(left.length, right.length);
    for (let index = 0; index < count; index += 1) {
        let a = left[index];
        let b = right[index];
        if (a >= 0x41 && a <= 0x5a) a += 0x20;
        if (b >= 0x41 && b <= 0x5a) b += 0x20;
        if (a !== b) return a < b ? -1 : 1;
    }
    return left.length < right.length ? -1 : left.length > right.length ? 1 : 0;
}

function compareRawBytes(left: Uint8Array, right: Uint8Array): number {
    const count = Math.min(left.length, right.length);
    for (let index = 0; index < count; index += 1) {
        if (left[index] !== right[index]) return left[index] < right[index] ? -1 : 1;
    }
    return left.length < right.length ? -1 : left.length > right.length ? 1 : 0;
}

/** Flipper toolbox/crc32_calc.c, including incremental seed semantics. */
export function flipperCrc32(crc: number, bytes: Uint8Array): number {
    const table = [
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    ];
    let value = (~crc) >>> 0;
    for (const byte of bytes) {
        value = ((value >>> 4) ^ table[(value ^ byte) & 0x0f]) >>> 0;
        value = ((value >>> 4) ^ table[(value ^ (byte >>> 4)) & 0x0f]) >>> 0;
    }
    return (~value) >>> 0;
}

export function sourceStamp(bytes: Uint8Array): SourceStamp {
    const size = bytes.length;
    if (size === 0) return {size: 0, sampleCrc32: 0};
    const offsets = [0];
    if (size > 256) {
        offsets.push(Math.floor((size - 256) / 2), size - 256);
    }

    let crc = 0;
    let previous = -1;
    for (const offset of offsets) {
        if (offset === previous) continue;
        previous = offset;
        const offsetBytes = new Uint8Array(4);
        new DataView(offsetBytes.buffer).setUint32(0, offset, true);
        crc = flipperCrc32(crc, offsetBytes);
        crc = flipperCrc32(crc, bytes.subarray(offset, Math.min(size, offset + 256)));
    }
    return {size, sampleCrc32: crc >>> 0};
}

function writeStamp(view: DataView, offset: number, stamp: SourceStamp): void {
    view.setBigUint64(offset, BigInt(stamp.size), true);
    view.setUint32(offset + 8, stamp.sampleCrc32, true);
    view.setUint32(offset + 12, 0, true);
}

function copyFixed(destination: Uint8Array, offset: number, source: Uint8Array, capacity: number): void {
    destination.set(source.subarray(0, Math.min(source.length, capacity)), offset);
}

function buildCategories(parsed: JsonObject, figures: FigureBuild[]): CategoryBuild[] {
    const categoryMap = new Map<number, CategoryBuild>();
    const series = isObject(parsed.amiibo_series) ? parsed.amiibo_series : {};

    for (const [idText, displayName] of Object.entries(series)) {
        const id = parseHexByte(idText);
        if (id == null || typeof displayName !== 'string') continue;
        categoryMap.set(id, {id, name: nativeFieldBytes(displayName, 256), count: 0});
    }

    for (const figure of figures) {
        let category = categoryMap.get(figure.category);
        if (!category) {
            category = {
                id: figure.category,
                name: nativeFieldBytes(`Series ${figure.category.toString(16).toUpperCase().padStart(2, '0')}`, 256),
                count: 0,
            };
            categoryMap.set(figure.category, category);
        }
        if (category.count < 0xffff) category.count += 1;
    }

    const categories = [...categoryMap.values()];
    if (categories.length > INDEX_FORMAT.maxCategories) {
        throw new Error(`Database contains ${categories.length} categories; native Amiibo Zero supports at most ${INDEX_FORMAT.maxCategories}.`);
    }
    categories.sort((left, right) => compareBytesNativeNoCase(left.name, right.name) || left.id - right.id);
    return categories;
}

function buildFigures(parsed: JsonObject, bytes: Uint8Array): FigureBuild[] {
    if (!isObject(parsed.amiibos)) throw new Error('amiibo.json does not contain an amiibos object.');
    const ranges = containerObjectMembers(bytes, 'amiibos');
    const figures: FigureBuild[] = [];

    for (const range of ranges) {
        const id = parseHexId(range.key);
        const value = parsed.amiibos[range.key];
        if (!id || !isObject(value) || bytes[range.valueStart] !== 0x7b) continue;

        const idHex = canonicalIdHex(id);
        let name = nativeFieldBytes(value.name, INDEX_FORMAT.nameSize);
        if (name.length === 0) name = encoder.encode(idHex);
        name = stripV3Suffix(name, id);

        let releaseNa = new Uint8Array(0);
        if (isObject(value.release)) releaseNa = nativeFieldBytes(value.release.na, INDEX_FORMAT.releaseSize) as Uint8Array<ArrayBuffer>;

        figures.push({
            id,
            idHex,
            name,
            releaseNa,
            category: id[6],
            type: id[3],
            jsonOffset: range.valueStart,
            jsonLength: range.valueEnd - range.valueStart,
        });
    }
    return figures;
}

function buildGameRefs(parsed: JsonObject, bytes: Uint8Array): GameRefBuild[] {
    if (!isObject(parsed.amiibos)) throw new Error('games_info.json does not contain an amiibos object.');
    const ranges = containerObjectMembers(bytes, 'amiibos');
    const refs: GameRefBuild[] = [];
    for (const range of ranges) {
        const pattern = parseHexId(range.key);
        const value = parsed.amiibos[range.key];
        if (!pattern || !isObject(value) || bytes[range.valueStart] !== 0x7b) continue;
        refs.push({pattern, jsonOffset: range.valueStart, jsonLength: range.valueEnd - range.valueStart});
    }
    return refs;
}

export function generateAmiiboIndex(
    amiiboParsed: unknown,
    amiiboBytes: Uint8Array,
    gamesParsed: unknown,
    gamesBytes: Uint8Array,
): GeneratedIndex {
    if (!isObject(amiiboParsed)) throw new Error('amiibo.json root must be a JSON object.');
    if (!isObject(gamesParsed)) throw new Error('games_info.json root must be a JSON object.');

    const figures = buildFigures(amiiboParsed, amiiboBytes);
    const categories = buildCategories(amiiboParsed, figures);
    const ranks = new Uint16Array(256);
    ranks.fill(0xffff);
    categories.forEach((category, index) => { ranks[category.id] = index; });

    figures.sort((left, right) => {
        const rankDiff = ranks[left.category] - ranks[right.category];
        if (rankDiff) return rankDiff;
        return compareBytesNativeNoCase(left.name, right.name) || compareRawBytes(left.id, right.id);
    });

    const gameRefs = buildGameRefs(gamesParsed, gamesBytes);
    const amiiboStamp = sourceStamp(amiiboBytes);
    const gamesStamp = sourceStamp(gamesBytes);
    const categoryBytes = categories.reduce((total, category) => total + INDEX_FORMAT.categoryPrefixSize + category.name.length, 0);
    const categoriesOffset = INDEX_FORMAT.headerSize;
    const figuresOffset = categoriesOffset + categoryBytes;
    const gamesOffset = figuresOffset + figures.length * INDEX_FORMAT.figureRecordSize;
    const totalSize = gamesOffset + gameRefs.length * INDEX_FORMAT.gameRefSize;
    if (totalSize > 0xffffffff) throw new Error('Generated index exceeds the native 32-bit file-offset limit.');

    const output = new Uint8Array(totalSize);
    const view = new DataView(output.buffer);
    output.set(encoder.encode(INDEX_FORMAT.magic), 0);
    view.setUint16(8, INDEX_FORMAT.version, true);
    view.setUint16(10, INDEX_FORMAT.headerSize, true);
    view.setUint16(12, INDEX_FORMAT.categoryPrefixSize, true);
    view.setUint16(14, INDEX_FORMAT.figureRecordSize, true);
    view.setUint16(16, INDEX_FORMAT.gameRefSize, true);
    view.setUint16(18, 0, true);
    // Bytes 20..23 are the native ARM alignment padding before AzSourceStamp.
    writeStamp(view, 24, amiiboStamp);
    writeStamp(view, 40, gamesStamp);
    view.setUint32(56, figures.length, true);
    view.setUint32(60, gameRefs.length, true);
    view.setUint16(64, categories.length, true);
    view.setUint16(66, 0, true);
    view.setUint32(68, categoriesOffset, true);
    view.setUint32(72, figuresOffset, true);
    view.setUint32(76, gamesOffset, true);

    let cursor = categoriesOffset;
    let firstFigure = 0;
    for (const category of categories) {
        output[cursor] = category.id;
        output[cursor + 1] = category.name.length;
        view.setUint16(cursor + 2, category.count, true);
        view.setUint32(cursor + 4, firstFigure, true);
        output.set(category.name, cursor + INDEX_FORMAT.categoryPrefixSize);
        cursor += INDEX_FORMAT.categoryPrefixSize + category.name.length;
        firstFigure += category.count;
    }

    for (const figure of figures) {
        const base = cursor;
        copyFixed(output, base, figure.id, 8);
        copyFixed(output, base + 8, encoder.encode(figure.idHex), 16);
        copyFixed(output, base + 25, figure.name, INDEX_FORMAT.nameSize - 1);
        copyFixed(output, base + 89, figure.releaseNa, INDEX_FORMAT.releaseSize - 1);
        output[base + 101] = figure.category;
        output[base + 102] = figure.type;
        // Byte 103 is native structure padding and remains zero.
        view.setUint32(base + 104, figure.jsonOffset, true);
        view.setUint32(base + 108, figure.jsonLength, true);
        cursor += INDEX_FORMAT.figureRecordSize;
    }

    for (const ref of gameRefs) {
        output.set(ref.pattern, cursor);
        view.setUint32(cursor + 8, ref.jsonOffset, true);
        view.setUint32(cursor + 12, ref.jsonLength, true);
        cursor += INDEX_FORMAT.gameRefSize;
    }

    if (cursor !== output.length) throw new Error('Internal index-size calculation mismatch.');
    return {
        bytes: output,
        figureCount: figures.length,
        categoryCount: categories.length,
        gameRefCount: gameRefs.length,
        amiiboStamp,
        gamesStamp,
    };
}
