const encoder = new TextEncoder();
const decoder = new TextDecoder();

const CLI_PROMPT = '>: ';
const DEFAULT_TIMEOUT_MS = 15000;
const WRITE_TIMEOUT_MS = 60000;
const DEFAULT_CHUNK_SIZE = 8192;

function concatBytes(a, b) {
    const merged = new Uint8Array(a.length + b.length);
    merged.set(a, 0);
    merged.set(b, a.length);
    return merged;
}

function findSequence(haystack, needle) {
    if (needle.length === 0) return 0;
    const max = haystack.length - needle.length;
    outer: for (let i = 0; i <= max; i += 1) {
        for (let j = 0; j < needle.length; j += 1) {
            if (haystack[i + j] !== needle[j]) continue outer;
        }
        return i;
    }
    return -1;
}

function storageError(output) {
    const match = output.match(/Storage error:\s*([^\r\n]+)/i);
    return match ? match[1].trim() : null;
}

function quotePath(path) {
    if (path.includes('"')) throw new Error('Flipper paths may not contain double quotes.');
    return `"${path}"`;
}

export class FlipperSerial {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.buffer = new Uint8Array(0);
    }

    static isSupported() {
        return Boolean(navigator.serial && window.isSecureContext);
    }

    get connected() {
        return Boolean(this.port?.readable && this.port?.writable && this.reader && this.writer);
    }

    async connect() {
        if (!FlipperSerial.isSupported()) {
            throw new Error('Web Serial requires a Chromium-based desktop browser on HTTPS or localhost.');
        }

        this.port = await navigator.serial.requestPort();
        await this.port.open({baudRate: 115200, bufferSize: 65536});
        this.reader = this.port.readable.getReader();
        this.writer = this.port.writable.getWriter();
        this.buffer = new Uint8Array(0);

        // Wake/synchronize the CLI, then verify this is a Flipper Zero CLI.
        await this.writeText('\r');
        await this.readUntil(CLI_PROMPT, 8000);
        const info = await this.command('device_info', 10000);
        if (!/hardware_model/i.test(info)) {
            throw new Error('The selected serial device did not identify itself as a Flipper Zero.');
        }
        return info;
    }

    async disconnect() {
        try {
            await this.reader?.cancel();
        } catch {
            // Port may already be disconnected.
        }
        try {
            this.reader?.releaseLock();
        } catch {
            // Ignore stale lock state.
        }
        try {
            this.writer?.releaseLock();
        } catch {
            // Ignore stale lock state.
        }
        this.reader = null;
        this.writer = null;
        this.buffer = new Uint8Array(0);

        if (this.port) {
            try {
                await this.port.close();
            } catch {
                // Ignore a device that disappeared before close.
            }
        }
        this.port = null;
    }

    async writeText(text) {
        if (!this.writer) throw new Error('Flipper is not connected.');
        await this.writer.write(encoder.encode(text));
    }

    async writeBytes(bytes) {
        if (!this.writer) throw new Error('Flipper is not connected.');
        await this.writer.write(bytes);
    }

    async readWithTimeout(timeoutMs) {
        if (!this.reader) throw new Error('Flipper is not connected.');
        let timeoutId;
        try {
            return await Promise.race([
                this.reader.read(),
                new Promise((_, reject) => {
                    timeoutId = setTimeout(
                        () => reject(new Error('Timed out waiting for the Flipper Zero CLI.')),
                        timeoutMs,
                    );
                }),
            ]);
        } finally {
            clearTimeout(timeoutId);
        }
    }

    async readUntil(marker, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const markerBytes = encoder.encode(marker);
        const started = performance.now();

        while (true) {
            const index = findSequence(this.buffer, markerBytes);
            if (index >= 0) {
                const before = this.buffer.slice(0, index);
                this.buffer = this.buffer.slice(index + markerBytes.length);
                return before;
            }

            const elapsed = performance.now() - started;
            const remaining = Math.max(1, timeoutMs - elapsed);
            if (remaining <= 1 && elapsed >= timeoutMs) {
                throw new Error(`Timed out waiting for ${JSON.stringify(marker)} from the Flipper Zero CLI.`);
            }

            const {value, done} = await this.readWithTimeout(remaining);
            if (done) throw new Error('The Flipper Zero serial connection closed unexpectedly.');
            if (value?.length) this.buffer = concatBytes(this.buffer, value);
        }
    }

    async command(command, timeoutMs = DEFAULT_TIMEOUT_MS) {
        await this.writeText(`${command}\r`);
        const bytes = await this.readUntil(CLI_PROMPT, timeoutMs);
        return decoder.decode(bytes);
    }

    async stat(path) {
        return this.command(`storage stat ${quotePath(path)}`);
    }

    async ensureDirectory(path) {
        const stat = await this.stat(path);
        if (/\bDirectory\b/.test(stat) || /\bStorage\b/.test(stat)) return;

        const error = storageError(stat);
        if (error && !/not exist|invalid name\/path/i.test(error)) {
            throw new Error(`Unable to inspect ${path}: ${error}`);
        }

        const output = await this.command(`storage mkdir ${quotePath(path)}`);
        const mkdirError = storageError(output);
        if (mkdirError && !/already exist/i.test(mkdirError)) {
            throw new Error(`Unable to create ${path}: ${mkdirError}`);
        }
    }

    async removeIfExists(path) {
        const stat = await this.stat(path);
        if (/File, size:/i.test(stat)) {
            const output = await this.command(`storage remove ${quotePath(path)}`);
            const error = storageError(output);
            if (error) throw new Error(`Unable to replace ${path}: ${error}`);
            return;
        }

        const error = storageError(stat);
        if (error && !/not exist|invalid name\/path/i.test(error)) {
            throw new Error(`Unable to inspect ${path}: ${error}`);
        }
    }

    async writeFile(path, bytes, onProgress = () => {}, chunkSize = DEFAULT_CHUNK_SIZE) {
        if (!(bytes instanceof Uint8Array)) bytes = new Uint8Array(bytes);
        if (!bytes.length) throw new Error(`Refusing to upload empty file to ${path}.`);

        await this.removeIfExists(path);
        let written = 0;

        while (written < bytes.length) {
            const chunk = bytes.subarray(written, Math.min(bytes.length, written + chunkSize));
            await this.writeText(`storage write_chunk ${quotePath(path)} ${chunk.length}\r`);

            // The official Flipper storage client waits for "Ready" before sending
            // each raw chunk. If the command failed, the CLI returns to the prompt.
            const first = await this.readUntilEither(['Ready\r\n', CLI_PROMPT], WRITE_TIMEOUT_MS);
            if (first.marker === CLI_PROMPT) {
                const output = decoder.decode(first.before);
                const error = storageError(output) ?? (output.trim() || 'unknown storage error');
                throw new Error(`Unable to write ${path}: ${error}`);
            }

            await this.writeBytes(chunk);
            const result = decoder.decode(await this.readUntil(CLI_PROMPT, WRITE_TIMEOUT_MS));
            const error = storageError(result);
            if (error) throw new Error(`Unable to write ${path}: ${error}`);

            written += chunk.length;
            onProgress(written / bytes.length);
        }

        const stat = await this.stat(path);
        const match = stat.match(/File, size:\s*(\d+)b/i);
        if (!match || Number(match[1]) !== bytes.length) {
            throw new Error(`Upload verification failed for ${path}. Expected ${bytes.length} bytes.`);
        }
    }

    async readUntilEither(markers, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const markerBytes = markers.map((marker) => ({marker, bytes: encoder.encode(marker)}));
        const started = performance.now();

        while (true) {
            let best = null;
            for (const candidate of markerBytes) {
                const index = findSequence(this.buffer, candidate.bytes);
                if (index >= 0 && (!best || index < best.index)) {
                    best = {...candidate, index};
                }
            }

            if (best) {
                const before = this.buffer.slice(0, best.index);
                this.buffer = this.buffer.slice(best.index + best.bytes.length);
                return {marker: best.marker, before};
            }

            const elapsed = performance.now() - started;
            const remaining = Math.max(1, timeoutMs - elapsed);
            if (remaining <= 1 && elapsed >= timeoutMs) {
                throw new Error('Timed out waiting for a response from the Flipper Zero CLI.');
            }

            const {value, done} = await this.readWithTimeout(remaining);
            if (done) throw new Error('The Flipper Zero serial connection closed unexpectedly.');
            if (value?.length) this.buffer = concatBytes(this.buffer, value);
        }
    }
}
