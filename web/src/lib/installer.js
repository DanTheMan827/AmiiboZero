export const SOURCE_URLS = {
    amiibo: 'https://dantheman827.github.io/AmiiboData/database/amiibo.json',
    games: 'https://dantheman827.github.io/AmiiboData/database/games_info.json',
};

export const DEVICE_PATHS = {
    appDirectory: '/ext/apps/NFC',
    app: '/ext/apps/NFC/amiibo_zero.fap',
    dataDirectory: '/ext/apps_data/amiibo_zero',
    key: '/ext/apps_data/amiibo_zero/key_retail.bin',
    amiibo: '/ext/apps_data/amiibo_zero/amiibo.json',
    games: '/ext/apps_data/amiibo_zero/games_info.json',
};

const encoder = new TextEncoder();

function hex(bytes) {
    return Array.from(bytes, (value) => value.toString(16).padStart(2, '0')).join('');
}

function absoluteAsset(name) {
    return new URL(name, document.baseURI).toString();
}

export async function loadRelease() {
    const manifestResponse = await fetch(absoluteAsset('release.json'), {cache: 'no-store'});
    if (!manifestResponse.ok) {
        throw new Error('This site does not contain a release manifest. Build the web installer through GitHub Actions.');
    }
    const manifest = await manifestResponse.json();
    if (!manifest.fap) throw new Error('The release manifest does not identify a FAP artifact.');

    const fapResponse = await fetch(absoluteAsset(manifest.fap), {cache: 'no-store'});
    if (!fapResponse.ok) throw new Error(`Unable to download the bundled Amiibo Zero FAP (${fapResponse.status}).`);
    const fap = new Uint8Array(await fapResponse.arrayBuffer());
    if (!fap.length) throw new Error('The bundled FAP is empty.');
    if (manifest.fapSize && Number(manifest.fapSize) !== fap.length) {
        throw new Error(`Bundled FAP size mismatch: expected ${manifest.fapSize}, received ${fap.length}.`);
    }
    if (manifest.fapSha256) {
        const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', fap));
        if (hex(digest) !== String(manifest.fapSha256).toLowerCase()) {
            throw new Error('Bundled FAP SHA-256 does not match the release manifest.');
        }
    }

    return {manifest, fap};
}

export async function fetchMinifiedJson(url) {
    const response = await fetch(url, {cache: 'no-store'});
    if (!response.ok) throw new Error(`Unable to fetch ${url} (${response.status}).`);

    const source = await response.text();
    let parsed;
    try {
        parsed = JSON.parse(source);
    } catch (error) {
        throw new Error(`Downloaded JSON from ${url} could not be parsed: ${error.message}`);
    }

    const minified = JSON.stringify(parsed);
    return {
        bytes: encoder.encode(minified),
        sourceBytes: encoder.encode(source).length,
        minifiedBytes: encoder.encode(minified).length,
    };
}

export async function readKeyFile(file) {
    if (!file) throw new Error('Choose your key_retail.bin first.');
    if (file.size !== 160) {
        throw new Error(`key_retail.bin must be exactly 160 bytes; selected file is ${file.size} bytes.`);
    }
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (bytes.length !== 160) throw new Error('Unable to read the complete 160-byte key file.');
    return bytes;
}

export async function prepareInstallPayload(keyFile, onStatus = () => {}) {
    onStatus('Downloading release FAP and Amiibo databases…');
    const [release, amiibo, games, key] = await Promise.all([
        loadRelease(),
        fetchMinifiedJson(SOURCE_URLS.amiibo),
        fetchMinifiedJson(SOURCE_URLS.games),
        readKeyFile(keyFile),
    ]);

    return {release, amiibo, games, key};
}

export async function installPayload(flipper, payload, onStep, onProgress) {
    await flipper.ensureDirectory('/ext/apps');
    await flipper.ensureDirectory(DEVICE_PATHS.appDirectory);
    await flipper.ensureDirectory('/ext/apps_data');
    await flipper.ensureDirectory(DEVICE_PATHS.dataDirectory);

    const files = [
        {id: 'fap', label: 'Amiibo Zero FAP', path: DEVICE_PATHS.app, bytes: payload.release.fap},
        {id: 'key', label: 'key_retail.bin', path: DEVICE_PATHS.key, bytes: payload.key},
        {id: 'amiibo', label: 'amiibo.json', path: DEVICE_PATHS.amiibo, bytes: payload.amiibo.bytes},
        {id: 'games', label: 'games_info.json', path: DEVICE_PATHS.games, bytes: payload.games.bytes},
    ];

    for (const file of files) {
        onStep(file.id, 'uploading', `Uploading ${file.label}…`);
        await flipper.writeFile(file.path, file.bytes, (fraction) => onProgress(file.id, fraction));
        onStep(file.id, 'done', `${file.label} installed`);
    }
}

export function formatBytes(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
    return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}
