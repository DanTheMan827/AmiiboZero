import {generateAmiiboIndex, type GeneratedIndex} from './indexGenerator';
import type {FlipperSerial} from './flipperSerial';

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
    index: '/ext/apps_data/amiibo_zero/amiibo.idx',
    indexTmp: '/ext/apps_data/amiibo_zero/amiibo.idx.tmp',
    indexBackup: '/ext/apps_data/amiibo_zero/amiibo.idx.bak',
    indexRaw: '/ext/apps_data/amiibo_zero/amiibo.raw.tmp',
    indexSort: '/ext/apps_data/amiibo_zero/amiibo.sort.tmp',
    gamesRaw: '/ext/apps_data/amiibo_zero/games.raw.tmp',
};

export const INDEX_PATHS = [
    DEVICE_PATHS.index,
    DEVICE_PATHS.indexTmp,
    DEVICE_PATHS.indexBackup,
    DEVICE_PATHS.indexRaw,
    DEVICE_PATHS.indexSort,
    DEVICE_PATHS.gamesRaw,
] as const;

const encoder = new TextEncoder();

export type ReleaseManifest = {
    version?: string;
    fap: string;
    fapSize?: number;
    fapSha256?: string;
    commit?: string;
};

export type MinifiedJson = {
    parsed: unknown;
    text: string;
    bytes: Uint8Array;
    sourceBytes: number;
    minifiedBytes: number;
    requestUrl: string;
};

export type PreparedPayload = {
    release: {manifest: ReleaseManifest; fap: Uint8Array; manualUrl: string};
    key: Uint8Array | null;
    amiibo: MinifiedJson;
    games: MinifiedJson;
    index: GeneratedIndex;
};

export type InstallStepId = 'cleanup' | 'fap' | 'key' | 'amiibo' | 'games' | 'index' | 'launch';
export type StepCallback = (id: InstallStepId, state: 'working' | 'done', message: string) => void;
export type ProgressCallback = (id: InstallStepId, progress: number) => void;

function hex(bytes: Uint8Array): string {
    return Array.from(bytes, (value) => value.toString(16).padStart(2, '0')).join('');
}

function pageBase(): string {
    return typeof document !== 'undefined' ? document.baseURI : 'https://example.invalid/';
}

function absoluteAsset(name: string): string {
    return new URL(name, pageBase()).toString();
}

export function cacheBustUrl(input: string, timestamp = Date.now()): string {
    const url = new URL(input, pageBase());
    if (url.protocol === 'http:' || url.protocol === 'https:') {
        url.searchParams.set('_ts', String(timestamp));
    }
    return url.toString();
}

export function manualFapUrl(manifest?: ReleaseManifest | null): string {
    return absoluteAsset(manifest?.fap || 'amiibo_zero.fap');
}

export async function loadReleaseManifest(timestamp = Date.now()): Promise<ReleaseManifest> {
    const response = await fetch(cacheBustUrl(absoluteAsset('release.json'), timestamp), {cache: 'no-store'});
    if (!response.ok) {
        throw new Error('This site does not contain a release manifest. Build the web installer through GitHub Actions.');
    }
    const manifest = await response.json() as ReleaseManifest;
    if (!manifest.fap) throw new Error('The release manifest does not identify a FAP artifact.');
    return manifest;
}

export async function loadReleaseFap(manifest: ReleaseManifest, timestamp = Date.now()): Promise<{fap: Uint8Array; manualUrl: string}> {
    const manualUrl = manualFapUrl(manifest);
    const response = await fetch(cacheBustUrl(manualUrl, timestamp), {cache: 'no-store'});
    if (!response.ok) throw new Error(`Unable to download the bundled Amiibo Zero FAP (${response.status}).`);
    const fap = new Uint8Array(await response.arrayBuffer());
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
    return {fap, manualUrl};
}

export async function fetchMinifiedJson(url: string, timestamp = Date.now()): Promise<MinifiedJson> {
    const requestUrl = cacheBustUrl(url, timestamp);
    const response = await fetch(requestUrl, {cache: 'no-store'});
    if (!response.ok) throw new Error(`Unable to fetch ${url} (${response.status}).`);
    const source = await response.text();
    let parsed: unknown;
    try {
        parsed = JSON.parse(source);
    } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        throw new Error(`Downloaded JSON from ${url} could not be parsed: ${message}`);
    }
    const text = JSON.stringify(parsed);
    const bytes = encoder.encode(text);
    return {
        parsed,
        text,
        bytes,
        sourceBytes: encoder.encode(source).length,
        minifiedBytes: bytes.length,
        requestUrl,
    };
}

export async function readKeyFile(file: File | null): Promise<Uint8Array> {
    if (!file) throw new Error('Choose your key_retail.bin first.');
    if (file.size !== 160) throw new Error(`key_retail.bin must be exactly 160 bytes; selected file is ${file.size} bytes.`);
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (bytes.length !== 160) throw new Error('Unable to read the complete 160-byte key file.');
    return bytes;
}

export async function prepareInstallPayload(keyFile: File | null, onStatus: (message: string) => void = () => {}): Promise<PreparedPayload> {
    const timestamp = Date.now();
    onStatus('Downloading fresh release metadata and Amiibo databases…');
    const [manifest, amiibo, games, key] = await Promise.all([
        loadReleaseManifest(timestamp),
        fetchMinifiedJson(SOURCE_URLS.amiibo, timestamp),
        fetchMinifiedJson(SOURCE_URLS.games, timestamp),
        keyFile ? readKeyFile(keyFile) : Promise.resolve(null),
    ]);

    onStatus('Downloading and verifying the release FAP…');
    const releaseFap = await loadReleaseFap(manifest, timestamp);
    onStatus('Generating native-compatible amiibo.idx in the browser…');
    const index = generateAmiiboIndex(amiibo.parsed, amiibo.bytes, games.parsed, games.bytes);
    return {release: {manifest, ...releaseFap}, key, amiibo, games, index};
}

async function ensureInstallDirectories(flipper: FlipperSerial): Promise<void> {
    await flipper.ensureDirectory('/ext/apps');
    await flipper.ensureDirectory(DEVICE_PATHS.appDirectory);
    await flipper.ensureDirectory('/ext/apps_data');
    await flipper.ensureDirectory(DEVICE_PATHS.dataDirectory);
}

export async function installPayload(
    flipper: FlipperSerial,
    payload: PreparedPayload,
    onStep: StepCallback,
    onProgress: ProgressCallback,
): Promise<void> {
    await ensureInstallDirectories(flipper);

    // No file upload occurs before the native app is quiesced and every possible
    // stale index artifact has been removed.
    onStep('cleanup', 'working', 'Checking for a running Amiibo app…');
    const closeResult = await flipper.closeAmiiboIfRunning();
    if (closeResult.running && !closeResult.closed) {
        onStep('cleanup', 'working', `Leaving unrelated running app “${closeResult.running}” untouched; clearing Amiibo index files…`);
    } else {
        onStep('cleanup', 'working', closeResult.closed ? 'Amiibo closed; clearing stale index files…' : 'Clearing stale index files…');
    }
    for (const path of INDEX_PATHS) await flipper.removeIfExists(path);
    onStep('cleanup', 'done', 'Amiibo stopped if necessary and all stale index files removed.');

    const files: Array<{id: InstallStepId; label: string; path: string; bytes: Uint8Array}> = [
        {id: 'fap', label: 'Amiibo Zero FAP', path: DEVICE_PATHS.app, bytes: payload.release.fap},
        ...(payload.key ? [{id: 'key' as InstallStepId, label: 'key_retail.bin', path: DEVICE_PATHS.key, bytes: payload.key}] : []),
        {id: 'amiibo', label: 'amiibo.json', path: DEVICE_PATHS.amiibo, bytes: payload.amiibo.bytes},
        {id: 'games', label: 'games_info.json', path: DEVICE_PATHS.games, bytes: payload.games.bytes},
        // Index goes last so it can never refer to JSON bytes that have not yet completed transfer.
        {id: 'index', label: 'amiibo.idx', path: DEVICE_PATHS.index, bytes: payload.index.bytes},
    ];

    if (!payload.key) onStep('key', 'done', 'Using existing key_retail.bin.');

    for (const file of files) {
        onStep(file.id, 'working', `Uploading ${file.label}…`);
        await flipper.writeFile(file.path, file.bytes, (fraction) => onProgress(file.id, fraction));
        onStep(file.id, 'done', `${file.label} installed and size-verified.`);
    }

    onStep('launch', 'working', 'Launching Amiibo Zero…');
    await flipper.launchFap(DEVICE_PATHS.app);
    onStep('launch', 'done', 'Amiibo Zero launched successfully.');
}

export function formatBytes(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
    return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}
