import test from 'node:test';
import assert from 'node:assert/strict';
import {
    DEVICE_PATHS,
    fetchMinifiedJson,
    installPayload,
    readKeyFile,
} from '../src/lib/installer.js';

test('JSON downloads are parsed and reserialized without formatting whitespace', async () => {
    const url = 'data:application/json,%7B%0A%20%20%22a%22%3A%20%5B1%2C%202%5D%2C%0A%20%20%22b%22%3A%20true%0A%7D';
    const result = await fetchMinifiedJson(url);
    assert.equal(new TextDecoder().decode(result.bytes), '{"a":[1,2],"b":true}');
    assert.ok(result.minifiedBytes < result.sourceBytes);
});

test('key_retail.bin must be exactly 160 bytes', async () => {
    const good = new File([new Uint8Array(160)], 'key_retail.bin');
    assert.equal((await readKeyFile(good)).length, 160);

    const bad = new File([new Uint8Array(159)], 'bad.bin');
    await assert.rejects(readKeyFile(bad), /exactly 160 bytes/);
});

test('install payload uses the expected Flipper paths and ordering', async () => {
    const directories = [];
    const writes = [];
    const states = [];
    const flipper = {
        ensureDirectory: async (path) => directories.push(path),
        writeFile: async (path, bytes, onProgress) => {
            writes.push([path, bytes.length]);
            onProgress(1);
        },
    };

    await installPayload(
        flipper,
        {
            release: {fap: new Uint8Array(12)},
            key: new Uint8Array(160),
            amiibo: {bytes: new Uint8Array(20)},
            games: {bytes: new Uint8Array(30)},
        },
        (id, state) => states.push(`${id}:${state}`),
        () => {},
    );

    assert.deepEqual(directories, [
        '/ext/apps',
        DEVICE_PATHS.appDirectory,
        '/ext/apps_data',
        DEVICE_PATHS.dataDirectory,
    ]);
    assert.deepEqual(
        writes.map(([path]) => path),
        [DEVICE_PATHS.app, DEVICE_PATHS.key, DEVICE_PATHS.amiibo, DEVICE_PATHS.games],
    );
    assert.ok(states.includes('fap:done'));
    assert.ok(states.includes('games:done'));
});
