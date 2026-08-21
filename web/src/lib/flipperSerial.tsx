const encoder = new TextEncoder();
const decoder = new TextDecoder();

const CLI_PROMPT = '>: ';
const DEFAULT_TIMEOUT_MS = 15000;
const WRITE_TIMEOUT_MS = 60000;
const DEFAULT_CHUNK_SIZE = 8192;
const FLIPPER_USB_VENDOR_ID = 0x0483;
const FLIPPER_USB_PRODUCT_ID = 0x5740;
const FLIPPER_USB_FILTERS = [{usbVendorId: FLIPPER_USB_VENDOR_ID, usbProductId: FLIPPER_USB_PRODUCT_ID}];

type SerialPortLike = {
    readable: ReadableStream<Uint8Array> | null;
    writable: WritableStream<Uint8Array> | null;
    open(options: {baudRate: number; bufferSize?: number}): Promise<void>;
    close(): Promise<void>;
    getInfo?(): {usbVendorId?: number; usbProductId?: number};
    setSignals?(signals: {dataTerminalReady?: boolean; requestToSend?: boolean}): Promise<void>;
};

type SerialApiLike = {
    requestPort(options?: {filters?: Array<{usbVendorId?: number; usbProductId?: number}>}): Promise<SerialPortLike>;
    addEventListener?(type: 'disconnect', listener: (event: Event) => void): void;
    removeEventListener?(type: 'disconnect', listener: (event: Event) => void): void;
};

type DataWaiter = {
    resolve: () => void;
    reject: (error: Error) => void;
    timer: ReturnType<typeof setTimeout>;
};

type DisconnectHandler = (reason: string) => void;

function serialApi(): SerialApiLike | undefined {
    return (navigator as Navigator & {serial?: SerialApiLike}).serial;
}

function concatBytes(a: Uint8Array, b: Uint8Array): Uint8Array {
    const merged = new Uint8Array(a.length + b.length);
    merged.set(a, 0);
    merged.set(b, a.length);
    return merged;
}

function findSequence(haystack: Uint8Array, needle: Uint8Array): number {
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

function storageError(output: string): string | null {
    const match = output.match(/Storage error:\s*([^\r\n]+)/i);
    return match ? match[1].trim() : null;
}

function quotePath(path: string): string {
    if (path.includes('"')) throw new Error('Flipper paths may not contain double quotes.');
    return `"${path}"`;
}

function delay(milliseconds: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

export class FlipperSerial {
    private port: SerialPortLike | null = null;
    private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
    private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
    private buffer = new Uint8Array(0);
    private dataWaiters = new Set<DataWaiter>();
    private dataSequence = 0;
    private readPump: Promise<void> | null = null;
    private connectedState = false;
    private manualDisconnect = false;
    private disconnectNotified = false;
    private disconnectHandler: DisconnectHandler | null = null;

    private readonly serialDisconnectListener = (event: Event): void => {
        if (!this.port || this.manualDisconnect) return;
        const eventPort = (event as Event & {port?: SerialPortLike}).port ??
            ((event.target as unknown as SerialPortLike)?.getInfo ? event.target as unknown as SerialPortLike : null);
        if (eventPort && eventPort !== this.port) return;
        void this.handleUnexpectedDisconnect('Flipper Zero was disconnected from USB.');
    };

    static isSupported(): boolean {
        return Boolean(serialApi() && window.isSecureContext);
    }

    static isCanonicalUsbDevice(info: {usbVendorId?: number; usbProductId?: number}): boolean {
        return info?.usbVendorId === FLIPPER_USB_VENDOR_ID && info?.usbProductId === FLIPPER_USB_PRODUCT_ID;
    }

    get connected(): boolean {
        return this.connectedState;
    }

    onDisconnect(handler: DisconnectHandler | null): void {
        this.disconnectHandler = handler;
    }

    async connect(): Promise<string> {
        if (!FlipperSerial.isSupported()) {
            throw new Error('Web Serial requires a Chromium-based desktop browser on HTTPS or localhost.');
        }
        if (this.connectedState) return this.readDeviceInfo();

        const api = serialApi()!;
        this.manualDisconnect = false;
        this.disconnectNotified = false;
        this.port = await api.requestPort({filters: FLIPPER_USB_FILTERS});
        const usbInfo = this.port.getInfo?.() ?? {};
        if (!FlipperSerial.isCanonicalUsbDevice(usbInfo)) {
            const vid = usbInfo.usbVendorId == null ? 'unknown' : `0x${usbInfo.usbVendorId.toString(16).padStart(4, '0')}`;
            const pid = usbInfo.usbProductId == null ? 'unknown' : `0x${usbInfo.usbProductId.toString(16).padStart(4, '0')}`;
            this.port = null;
            throw new Error(`The selected serial device is not the canonical Flipper Zero USB CDC device (${vid}:${pid}).`);
        }

        try {
            await this.port.open({baudRate: 115200, bufferSize: 65536});
            await this.port.setSignals?.({dataTerminalReady: true, requestToSend: false});
            if (!this.port.readable || !this.port.writable) throw new Error('Flipper serial streams did not open.');
            this.reader = this.port.readable.getReader();
            this.writer = this.port.writable.getWriter();
            this.buffer = new Uint8Array(0);
            this.dataSequence = 0;
            this.connectedState = true;
            api.addEventListener?.('disconnect', this.serialDisconnectListener);
            this.readPump = this.runReadPump();

            // Canonical Flipper storage.py waits after opening before consuming the shell prompt.
            await delay(500);
            await this.readUntil(CLI_PROMPT, 8000);
            return await this.readDeviceInfo();
        } catch (error) {
            await this.disconnect();
            throw error;
        }
    }

    async readDeviceInfo(timeoutMs = 10000): Promise<string> {
        await this.writeText('device_info\r');
        const beforeModel = await this.readUntil('hardware_model', timeoutMs);
        const afterModel = await this.readUntil(CLI_PROMPT, timeoutMs);
        const info = `${decoder.decode(beforeModel)}hardware_model${decoder.decode(afterModel)}`;
        if (!/hardware_model\s*:\s*[^\r\n]+/i.test(info)) {
            throw new Error('Connected to the Flipper USB serial interface, but device_info did not return hardware_model.');
        }
        return info;
    }

    async disconnect(): Promise<void> {
        this.manualDisconnect = true;
        serialApi()?.removeEventListener?.('disconnect', this.serialDisconnectListener);
        this.connectedState = false;
        this.rejectWaiters(new Error('Serial connection closed.'));

        try {
            await this.port?.setSignals?.({dataTerminalReady: false, requestToSend: false});
        } catch {
            // Device may already be gone.
        }
        try {
            await this.reader?.cancel();
        } catch {
            // Stream may already be closed.
        }
        try {
            await this.readPump;
        } catch {
            // Pump reports disconnections through the handler.
        }
        try {
            this.reader?.releaseLock();
        } catch {
            // Ignore stale reader lock state.
        }
        try {
            this.writer?.releaseLock();
        } catch {
            // Ignore stale writer lock state.
        }
        try {
            await this.port?.close();
        } catch {
            // Ignore a port that disappeared before close.
        }

        this.reader = null;
        this.writer = null;
        this.port = null;
        this.readPump = null;
        this.buffer = new Uint8Array(0);
        this.manualDisconnect = false;
        this.disconnectNotified = false;
    }

    private async runReadPump(): Promise<void> {
        try {
            while (this.reader && this.connectedState) {
                const {value, done} = await this.reader.read();
                if (done) break;
                if (value?.length) {
                    this.buffer = concatBytes(this.buffer, value) as Uint8Array<ArrayBuffer>;
                    this.dataSequence += 1;
                    this.resolveWaiters();
                }
            }
            if (!this.manualDisconnect && this.connectedState) {
                await this.handleUnexpectedDisconnect('Flipper Zero serial stream closed unexpectedly.');
            }
        } catch (error) {
            if (!this.manualDisconnect && this.connectedState) {
                const message = error instanceof Error ? error.message : String(error);
                await this.handleUnexpectedDisconnect(`Flipper Zero serial connection failed: ${message}`);
            }
        }
    }

    private async handleUnexpectedDisconnect(reason: string): Promise<void> {
        if (this.disconnectNotified || this.manualDisconnect) return;
        this.disconnectNotified = true;
        this.connectedState = false;
        serialApi()?.removeEventListener?.('disconnect', this.serialDisconnectListener);
        this.rejectWaiters(new Error(reason));
        try { this.reader?.releaseLock(); } catch { /* disconnected */ }
        try { this.writer?.releaseLock(); } catch { /* disconnected */ }
        this.reader = null;
        this.writer = null;
        this.port = null;
        this.buffer = new Uint8Array(0);
        this.disconnectHandler?.(reason);
    }

    private resolveWaiters(): void {
        for (const waiter of this.dataWaiters) {
            clearTimeout(waiter.timer);
            waiter.resolve();
        }
        this.dataWaiters.clear();
    }

    private rejectWaiters(error: Error): void {
        for (const waiter of this.dataWaiters) {
            clearTimeout(waiter.timer);
            waiter.reject(error);
        }
        this.dataWaiters.clear();
    }

    private waitForData(timeoutMs: number, observedSequence: number): Promise<void> {
        if (!this.connectedState) return Promise.reject(new Error('Flipper is not connected.'));
        return new Promise((resolve, reject) => {
            const waiter: DataWaiter = {
                resolve,
                reject,
                timer: setTimeout(() => {
                    this.dataWaiters.delete(waiter);
                    reject(new Error('Timed out waiting for the Flipper Zero CLI.'));
                }, timeoutMs),
            };
            this.dataWaiters.add(waiter);
            // Close the tiny race between checking the receive buffer and registering
            // this waiter: if the read pump appended data in between, resolve now.
            if (this.dataSequence !== observedSequence) {
                this.dataWaiters.delete(waiter);
                clearTimeout(waiter.timer);
                resolve();
            }
        });
    }

    async writeText(text: string): Promise<void> {
        if (!this.writer || !this.connectedState) throw new Error('Flipper is not connected.');
        try {
            await this.writer.write(encoder.encode(text));
        } catch (error) {
            await this.handleUnexpectedDisconnect('Flipper Zero disconnected during a serial write.');
            throw error;
        }
    }

    async writeBytes(bytes: Uint8Array): Promise<void> {
        if (!this.writer || !this.connectedState) throw new Error('Flipper is not connected.');
        try {
            await this.writer.write(bytes);
        } catch (error) {
            await this.handleUnexpectedDisconnect('Flipper Zero disconnected during a file transfer.');
            throw error;
        }
    }

    async readUntil(marker: string, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<Uint8Array> {
        const markerBytes = encoder.encode(marker);
        const deadline = performance.now() + timeoutMs;
        while (true) {
            const index = findSequence(this.buffer, markerBytes);
            if (index >= 0) {
                const before = this.buffer.slice(0, index);
                this.buffer = this.buffer.slice(index + markerBytes.length);
                return before;
            }
            const remaining = deadline - performance.now();
            if (remaining <= 0) throw new Error(`Timed out waiting for ${JSON.stringify(marker)} from the Flipper Zero CLI.`);
            const observedSequence = this.dataSequence;
            // Re-check once after capturing the sequence; a marker may already have arrived.
            if (findSequence(this.buffer, markerBytes) >= 0) continue;
            await this.waitForData(remaining, observedSequence);
        }
    }

    async readUntilEither(markers: string[], timeoutMs = DEFAULT_TIMEOUT_MS): Promise<{marker: string; before: Uint8Array}> {
        const candidates = markers.map((marker) => ({marker, bytes: encoder.encode(marker)}));
        const deadline = performance.now() + timeoutMs;
        while (true) {
            let best: {marker: string; bytes: Uint8Array; index: number} | null = null;
            for (const candidate of candidates) {
                const index = findSequence(this.buffer, candidate.bytes);
                if (index >= 0 && (!best || index < best.index)) best = {...candidate, index};
            }
            if (best) {
                const before = this.buffer.slice(0, best.index);
                this.buffer = this.buffer.slice(best.index + best.bytes.length);
                return {marker: best.marker, before};
            }
            const remaining = deadline - performance.now();
            if (remaining <= 0) throw new Error('Timed out waiting for a response from the Flipper Zero CLI.');
            const observedSequence = this.dataSequence;
            if (candidates.some((candidate) => findSequence(this.buffer, candidate.bytes) >= 0)) continue;
            await this.waitForData(remaining, observedSequence);
        }
    }

    async command(command: string, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<string> {
        await this.writeText(`${command}\r`);
        return decoder.decode(await this.readUntil(CLI_PROMPT, timeoutMs));
    }

    async stat(path: string): Promise<string> {
        return this.command(`storage stat ${quotePath(path)}`);
    }

    async fileSize(path: string): Promise<number | null> {
        const stat = await this.stat(path);
        const match = stat.match(/File, size:\s*(\d+)b/i);
        if (match) return Number(match[1]);
        const error = storageError(stat);
        if (error && /not exist|invalid name\/path/i.test(error)) return null;
        if (error) throw new Error(`Unable to inspect ${path}: ${error}`);
        return null;
    }

    async ensureDirectory(path: string): Promise<void> {
        const stat = await this.stat(path);
        if (/\bDirectory\b/.test(stat) || /\bStorage\b/.test(stat)) return;
        const error = storageError(stat);
        if (error && !/not exist|invalid name\/path/i.test(error)) throw new Error(`Unable to inspect ${path}: ${error}`);
        const output = await this.command(`storage mkdir ${quotePath(path)}`);
        const mkdirError = storageError(output);
        if (mkdirError && !/already exist/i.test(mkdirError)) throw new Error(`Unable to create ${path}: ${mkdirError}`);
    }

    async removeIfExists(path: string): Promise<boolean> {
        const stat = await this.stat(path);
        if (/File, size:/i.test(stat)) {
            const output = await this.command(`storage remove ${quotePath(path)}`);
            const error = storageError(output);
            if (error) throw new Error(`Unable to remove ${path}: ${error}`);
            return true;
        }
        const error = storageError(stat);
        if (error && !/not exist|invalid name\/path/i.test(error)) throw new Error(`Unable to inspect ${path}: ${error}`);
        return false;
    }

    async writeFile(path: string, bytes: Uint8Array, onProgress: (fraction: number) => void = () => {}, chunkSize = DEFAULT_CHUNK_SIZE): Promise<void> {
        if (!(bytes instanceof Uint8Array)) bytes = new Uint8Array(bytes);
        if (!bytes.length) throw new Error(`Refusing to upload empty file to ${path}.`);
        await this.removeIfExists(path);
        let written = 0;
        while (written < bytes.length) {
            const chunk = bytes.subarray(written, Math.min(bytes.length, written + chunkSize));
            await this.writeText(`storage write_chunk ${quotePath(path)} ${chunk.length}\r`);
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
        const actual = await this.fileSize(path);
        if (actual !== bytes.length) throw new Error(`Upload verification failed for ${path}. Expected ${bytes.length} bytes, got ${actual ?? 'missing'}.`);
    }

    async runningApplication(): Promise<string | null> {
        const output = await this.command('loader info');
        if (/No application is running/i.test(output)) return null;
        return output.match(/Application\s+"([^"]+)"\s+is running/i)?.[1] ?? null;
    }

    async closeAmiiboIfRunning(): Promise<{closed: boolean; running: string | null}> {
        const running = await this.runningApplication();
        if (!running) return {closed: false, running: null};
        if (!/^amiibo(?: zero)?$/i.test(running.trim())) return {closed: false, running};

        const output = await this.command('loader close');
        if (/has to be closed manually/i.test(output)) {
            throw new Error(`Amiibo is running but refused the loader exit signal. Close it on the Flipper and try again.`);
        }
        if (!/was closed/i.test(output) && !/No application is running/i.test(output)) {
            throw new Error(`Unexpected response while closing Amiibo: ${output.trim() || 'no response'}`);
        }
        await delay(250);
        return {closed: true, running};
    }

    async launchFap(path: string): Promise<string> {
        const output = await this.command(`loader open ${quotePath(path)}`, 20000);
        await delay(250);
        const running = await this.runningApplication();
        if (!running || !/^amiibo(?: zero)?$/i.test(running.trim())) {
            const detail = output.trim();
            throw new Error(detail ? `Amiibo Zero did not launch: ${detail}` : `Amiibo Zero did not launch; loader reports ${running ? `"${running}"` : 'no running application'}.`);
        }
        return running;
    }
}
