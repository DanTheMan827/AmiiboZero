import test from 'node:test';
import assert from 'node:assert/strict';
import {FlipperSerial} from '../src/lib/flipperSerial.js';

class MockFlipper extends FlipperSerial {
    constructor() {
        super();
        this.files = new Map();
        this.pendingPath = null;
        this.pendingSize = 0;
        this.commands = [];
    }

    async stat(path) {
        if (!this.files.has(path)) return 'Storage error: file/dir not exist\r\n';
        return `File, size: ${this.files.get(path).length}b\r\n`;
    }

    async removeIfExists(path) {
        this.files.delete(path);
    }

    async writeText(text) {
        this.commands.push(text);
        const match = text.match(/^storage write_chunk "([^"]+)" (\d+)\r$/);
        assert.ok(match, `unexpected CLI command: ${JSON.stringify(text)}`);
        this.pendingPath = match[1];
        this.pendingSize = Number(match[2]);
    }

    async readUntilEither(markers) {
        assert.ok(markers.includes('Ready\r\n'));
        return {marker: 'Ready\r\n', before: new Uint8Array(0)};
    }

    async writeBytes(bytes) {
        assert.equal(bytes.length, this.pendingSize);
        const old = this.files.get(this.pendingPath) || new Uint8Array(0);
        const next = new Uint8Array(old.length + bytes.length);
        next.set(old, 0);
        next.set(bytes, old.length);
        this.files.set(this.pendingPath, next);
    }

    async readUntil(marker) {
        assert.equal(marker, '>: ');
        return new Uint8Array(0);
    }
}

test('writeFile mirrors Flipper write_chunk append protocol and replaces old data', async () => {
    const flipper = new MockFlipper();
    flipper.files.set('/ext/test.bin', new Uint8Array([9, 9, 9]));

    const input = new Uint8Array(20000);
    for (let i = 0; i < input.length; i += 1) input[i] = i & 0xff;

    let lastProgress = 0;
    await flipper.writeFile('/ext/test.bin', input, (progress) => {
        lastProgress = progress;
    });

    assert.deepEqual(flipper.files.get('/ext/test.bin'), input);
    assert.equal(lastProgress, 1);
    assert.equal(flipper.commands.length, 3);
    assert.match(flipper.commands[0], / 8192\r$/);
    assert.match(flipper.commands[1], / 8192\r$/);
    assert.match(flipper.commands[2], / 3616\r$/);
});
