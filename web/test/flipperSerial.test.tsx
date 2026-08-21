import test from 'node:test';
import assert from 'node:assert/strict';
import {FlipperSerial} from '../src/lib/flipperSerial';

class MockFlipper extends FlipperSerial {
    files = new Map<string, Uint8Array>();
    pendingPath = '';
    pendingSize = 0;
    commands: string[] = [];

    override async stat(path: string): Promise<string> {
        if (!this.files.has(path)) return 'Storage error: file/dir not exist\r\n';
        return `File, size: ${this.files.get(path)!.length}b\r\n`;
    }
    override async removeIfExists(path: string): Promise<boolean> { return this.files.delete(path); }
    override async writeText(text: string): Promise<void> {
        this.commands.push(text);
        const match = text.match(/^storage write_chunk "([^"]+)" (\d+)\r$/);
        assert.ok(match, `unexpected CLI command: ${JSON.stringify(text)}`);
        this.pendingPath = match[1];
        this.pendingSize = Number(match[2]);
    }
    override async readUntilEither(markers: string[]): Promise<{marker: string; before: Uint8Array}> {
        assert.ok(markers.includes('Ready\r\n'));
        return {marker: 'Ready\r\n', before: new Uint8Array(0)};
    }
    override async writeBytes(bytes: Uint8Array): Promise<void> {
        assert.equal(bytes.length, this.pendingSize);
        const old = this.files.get(this.pendingPath) || new Uint8Array(0);
        const next = new Uint8Array(old.length + bytes.length);
        next.set(old, 0);
        next.set(bytes, old.length);
        this.files.set(this.pendingPath, next);
    }
    override async readUntil(marker: string): Promise<Uint8Array> {
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
    await flipper.writeFile('/ext/test.bin', input, (progress) => { lastProgress = progress; });
    assert.deepEqual(flipper.files.get('/ext/test.bin'), input);
    assert.equal(lastProgress, 1);
    assert.equal(flipper.commands.length, 3);
    assert.match(flipper.commands[0], / 8192\r$/);
    assert.match(flipper.commands[1], / 8192\r$/);
    assert.match(flipper.commands[2], / 3616\r$/);
});

test('canonical Flipper USB CDC identity is VID 0483 PID 5740', () => {
    assert.equal(FlipperSerial.isCanonicalUsbDevice({usbVendorId: 0x0483, usbProductId: 0x5740}), true);
    assert.equal(FlipperSerial.isCanonicalUsbDevice({usbVendorId: 0x0483, usbProductId: 0xdf11}), false);
    assert.equal(FlipperSerial.isCanonicalUsbDevice({usbVendorId: 0x1234, usbProductId: 0x5740}), false);
});

test('readDeviceInfo waits for hardware_model instead of a stale CLI prompt', async () => {
    const encoder = new TextEncoder();
    class InfoMock extends FlipperSerial {
        sent: string[] = [];
        reads: string[] = [];
        override async writeText(text: string): Promise<void> { this.sent.push(text); }
        override async readUntil(marker: string): Promise<Uint8Array> {
            this.reads.push(marker);
            if (marker === 'hardware_model') return encoder.encode('>: device_info\r\ndevice_info_major             : 2\r\n');
            if (marker === '>: ') return encoder.encode('                         : Flipper Zero\r\nhardware_uid                   : 0123456789ABCDEF\r\n');
            throw new Error(`unexpected marker ${marker}`);
        }
    }
    const flipper = new InfoMock();
    const info = await flipper.readDeviceInfo();
    assert.deepEqual(flipper.sent, ['device_info\r']);
    assert.deepEqual(flipper.reads, ['hardware_model', '>: ']);
    assert.match(info, /hardware_model\s*:\s*Flipper Zero/i);
});

test('fileSize returns the installed size or null for a missing file', async () => {
    class StatMock extends FlipperSerial {
        override async stat(path: string): Promise<string> {
            return path.endsWith('key_retail.bin') ? 'File, size: 160b\r\n' : 'Storage error: file/dir not exist\r\n';
        }
    }
    const flipper = new StatMock();
    assert.equal(await flipper.fileSize('/ext/apps_data/amiibo_zero/key_retail.bin'), 160);
    assert.equal(await flipper.fileSize('/ext/apps_data/amiibo_zero/missing.bin'), null);
});

test('closeAmiiboIfRunning only closes the Amiibo application', async () => {
    class LoaderMock extends FlipperSerial {
        running = 'Amiibo';
        closeCalls = 0;
        override async runningApplication(): Promise<string | null> { return this.running; }
        override async command(command: string): Promise<string> {
            if (command === 'loader close') { this.closeCalls += 1; this.running = ''; return 'Application "Amiibo" was closed\r\n'; }
            throw new Error(command);
        }
    }
    const amiibo = new LoaderMock();
    assert.equal((await amiibo.closeAmiiboIfRunning()).closed, true);
    assert.equal(amiibo.closeCalls, 1);

    const unrelated = new LoaderMock();
    unrelated.running = 'NFC';
    assert.equal((await unrelated.closeAmiiboIfRunning()).closed, false);
    assert.equal(unrelated.closeCalls, 0);
});


test('unexpected serial disconnect clears connected state and notifies the UI handler', async () => {
    const flipper = new FlipperSerial();
    let reason = '';
    flipper.onDisconnect((message) => { reason = message; });
    (flipper as unknown as {connectedState: boolean}).connectedState = true;
    await (flipper as unknown as {handleUnexpectedDisconnect(message: string): Promise<void>})
        .handleUnexpectedDisconnect('Flipper Zero was disconnected from USB.');
    assert.equal(flipper.connected, false);
    assert.match(reason, /disconnected from USB/i);
});
