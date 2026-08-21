import test from 'node:test';
import assert from 'node:assert/strict';
import {FlipperSerial} from '../src/lib/flipperSerial';
import {
    DEVICE_PATHS,
    INDEX_PATHS,
    cacheBustUrl,
    fetchMinifiedJson,
    loadReleaseManifest,
    installPayload,
    readKeyFile,
    type PreparedPayload,
} from '../src/lib/installer';

test('cacheBustUrl appends the supplied timestamp to HTTP JSON requests', () => {
    const busted = new URL(cacheBustUrl('https://example.com/manifest.json?x=1', 123456));
    assert.equal(busted.searchParams.get('x'), '1');
    assert.equal(busted.searchParams.get('_ts'), '123456');
});



test('release manifest request also uses the supplied timestamp cache buster', async () => {
    const originalFetch = globalThis.fetch;
    let requested = '';
    globalThis.fetch = (async (input: string | URL | Request) => {
        requested = String(input);
        return new Response('{"fap":"amiibo_zero.fap","version":"0.1.0"}', {status: 200});
    }) as typeof fetch;
    try {
        const result = await loadReleaseManifest(24680);
        assert.equal(result.fap, 'amiibo_zero.fap');
        assert.equal(new URL(requested).searchParams.get('_ts'), '24680');
    } finally {
        globalThis.fetch = originalFetch;
    }
});

test('JSON downloads are cache-busted, parsed, and reserialized without formatting whitespace', async () => {
    const originalFetch = globalThis.fetch;
    let requested = '';
    globalThis.fetch = (async (input: string | URL | Request) => {
        requested = String(input);
        return new Response('{\n  "a": [1, 2],\n  "b": true\n}', {status: 200, headers: {'content-type': 'application/json'}});
    }) as typeof fetch;
    try {
        const result = await fetchMinifiedJson('https://example.com/amiibo.json', 9876);
        assert.equal(new TextDecoder().decode(result.bytes), '{"a":[1,2],"b":true}');
        assert.ok(result.minifiedBytes < result.sourceBytes);
        assert.equal(new URL(requested).searchParams.get('_ts'), '9876');
    } finally {
        globalThis.fetch = originalFetch;
    }
});

test('key_retail.bin must be exactly 160 bytes', async () => {
    const good = new File([new Uint8Array(160)], 'key_retail.bin');
    assert.equal((await readKeyFile(good)).length, 160);
    const bad = new File([new Uint8Array(159)], 'bad.bin');
    await assert.rejects(readKeyFile(bad), /exactly 160 bytes/);
});

class TransactionMock extends FlipperSerial {
    directories: string[] = [];
    removed: string[] = [];
    writes: string[] = [];
    launched = '';
    closed = false;

    override async ensureDirectory(path: string): Promise<void> { this.directories.push(path); }
    override async closeAmiiboIfRunning(): Promise<{closed: boolean; running: string | null}> { this.closed = true; return {closed: true, running: 'Amiibo'}; }
    override async removeIfExists(path: string): Promise<boolean> { this.removed.push(path); return true; }
    override async writeFile(path: string, _bytes: Uint8Array, onProgress: (fraction: number) => void): Promise<void> { this.writes.push(path); onProgress(1); }
    override async launchFap(path: string): Promise<string> { this.launched = path; return 'Amiibo'; }
}

function payload(withKey = true): PreparedPayload {
    return {
        release: {manifest: {fap: 'amiibo_zero.fap'}, fap: new Uint8Array(12), manualUrl: 'https://example.com/amiibo_zero.fap'},
        key: withKey ? new Uint8Array(160) : null,
        amiibo: {parsed: {}, text: '{}', bytes: new Uint8Array(20), sourceBytes: 25, minifiedBytes: 20, requestUrl: 'a'},
        games: {parsed: {}, text: '{}', bytes: new Uint8Array(30), sourceBytes: 35, minifiedBytes: 30, requestUrl: 'g'},
        index: {bytes: new Uint8Array(40), figureCount: 1, categoryCount: 1, gameRefCount: 1, amiiboStamp: {size: 20, sampleCrc32: 1}, gamesStamp: {size: 30, sampleCrc32: 2}},
    };
}

test('transaction closes Amiibo and removes every index artifact before the first upload, uploads index last, then launches app', async () => {
    const flipper = new TransactionMock();
    const states: string[] = [];
    await installPayload(flipper, payload(true), (id, state) => states.push(`${id}:${state}`), () => {});
    assert.equal(flipper.closed, true);
    assert.deepEqual(flipper.removed, [...INDEX_PATHS]);
    assert.deepEqual(flipper.writes, [DEVICE_PATHS.app, DEVICE_PATHS.key, DEVICE_PATHS.amiibo, DEVICE_PATHS.games, DEVICE_PATHS.index]);
    assert.equal(flipper.launched, DEVICE_PATHS.app);
    assert.ok(states.indexOf('cleanup:done') < states.indexOf('fap:working'));
    assert.ok(states.indexOf('index:done') < states.indexOf('launch:working'));
});

test('transaction reuses an existing key when no replacement is supplied', async () => {
    const flipper = new TransactionMock();
    const states: Array<[string, string, string]> = [];
    await installPayload(flipper, payload(false), (id, state, message) => states.push([id, state, message]), () => {});
    assert.deepEqual(flipper.writes, [DEVICE_PATHS.app, DEVICE_PATHS.amiibo, DEVICE_PATHS.games, DEVICE_PATHS.index]);
    assert.ok(states.some(([id, state, message]) => id === 'key' && state === 'done' && /existing/i.test(message)));
});
