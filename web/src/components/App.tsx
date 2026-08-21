import {useEffect, useMemo, useRef, useState} from 'react';
import {FlipperSerial} from '../lib/flipperSerial';
import {
    DEVICE_PATHS,
    formatBytes,
    installPayload,
    loadReleaseManifest,
    manualFapUrl,
    prepareInstallPayload,
    type InstallStepId,
} from '../lib/installer';
import {Header} from './Header';
import {Hero} from './Hero';
import {Notice} from './Notice';
import {KeyCard} from './KeyCard';
import {InstallSet, type InstallSizes} from './InstallSet';
import {TransactionStatus} from './TransactionStatus';
import {InstallBar} from './InstallBar';
import {FeatureGrid} from './FeatureGrid';
import {SourceLinks} from './SourceLinks';
import {Footer} from './Footer';
import type {UiStep} from './ResourceRow';
import styles from './App.module.css';

const stepIds: InstallStepId[] = ['cleanup', 'fap', 'key', 'amiibo', 'games', 'index', 'launch'];

function makeInitialSteps(): Record<InstallStepId, UiStep> {
    return Object.fromEntries(stepIds.map((id) => [id, {state: 'idle', progress: 0}])) as Record<InstallStepId, UiStep>;
}

function initialTheme(): 'light' | 'dark' {
    const stored = localStorage.getItem('az-theme');
    if (stored === 'light' || stored === 'dark') return stored;
    return matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

export function App() {
    const serialRef = useRef(new FlipperSerial());
    const [theme, setTheme] = useState<'light' | 'dark'>(initialTheme);
    const [connected, setConnected] = useState(false);
    const [busy, setBusy] = useState(false);
    const [keyFile, setKeyFile] = useState<File | null>(null);
    const [deviceKeySize, setDeviceKeySize] = useState<number | null>(null);
    const [message, setMessage] = useState('Connect your Flipper Zero to begin.');
    const [error, setError] = useState('');
    const [release, setRelease] = useState<{version?: string; fap: string} | null>(null);
    const [sizes, setSizes] = useState<InstallSizes>({});
    const [steps, setSteps] = useState<Record<InstallStepId, UiStep>>(makeInitialSteps);

    const serialSupported = FlipperSerial.isSupported();
    const hasValidSelectedKey = keyFile?.size === 160;
    const hasValidDeviceKey = deviceKeySize === 160;
    const keyReady = keyFile ? hasValidSelectedKey : hasValidDeviceKey;
    const ready = connected && keyReady && !busy && serialSupported && Boolean(release);

    useEffect(() => {
        document.documentElement.dataset.theme = theme;
        localStorage.setItem('az-theme', theme);
    }, [theme]);

    useEffect(() => {
        const timestamp = Date.now();
        loadReleaseManifest(timestamp)
            .then((manifest) => setRelease(manifest))
            .catch(() => setRelease(null));
    }, []);

    useEffect(() => {
        const serial = serialRef.current;
        serial.onDisconnect((reason) => {
            setConnected(false);
            setDeviceKeySize(null);
            setBusy(false);
            setError(reason);
            setMessage('Reconnect the Flipper Zero to continue.');
        });
        return () => {
            serial.onDisconnect(null);
            void serial.disconnect();
        };
    }, []);

    const releaseLabel = useMemo(() => {
        if (!release) return 'Release metadata unavailable';
        if (!release.version) return 'Release build';
        return release.version.startsWith('development-') ? release.version : `v${release.version}`;
    }, [release]);

    const fapManualUrl = useMemo(() => manualFapUrl(release), [release]);

    function updateStep(id: InstallStepId, state: 'working' | 'done', statusMessage: string): void {
        setSteps((current) => ({
            ...current,
            [id]: {...current[id], state, progress: state === 'done' ? 1 : current[id].progress},
        }));
        if (statusMessage) setMessage(statusMessage);
    }

    function updateProgress(id: InstallStepId, progress: number): void {
        setSteps((current) => ({...current, [id]: {...current[id], progress}}));
    }

    async function connect(): Promise<void> {
        setError('');
        setBusy(true);
        setMessage('Opening the Flipper USB serial interface…');
        try {
            const info = await serialRef.current.connect();
            const model = info.match(/hardware_model\s*:\s*([^\r\n]+)/i)?.[1]?.trim() || 'Flipper Zero';
            const existingKeySize = await serialRef.current.fileSize(DEVICE_PATHS.key);
            setDeviceKeySize(existingKeySize);
            setConnected(true);
            setMessage(existingKeySize === 160 ? `${model} connected. Existing 160-byte retail key will be reused.` : `${model} connected. Select a 160-byte key_retail.bin.`);
        } catch (caught) {
            const detail = caught instanceof Error ? caught.message : String(caught);
            setConnected(false);
            setDeviceKeySize(null);
            setError(detail);
            setMessage('Connection failed.');
        } finally {
            setBusy(false);
        }
    }

    async function disconnect(): Promise<void> {
        setBusy(true);
        try {
            await serialRef.current.disconnect();
            setConnected(false);
            setDeviceKeySize(null);
            setMessage('Flipper disconnected.');
            setError('');
        } finally {
            setBusy(false);
        }
    }

    async function install(): Promise<void> {
        setError('');
        setBusy(true);
        setSteps(makeInitialSteps());
        try {
            if (!serialRef.current.connected) throw new Error('Flipper Zero is no longer connected.');
            if (!keyReady) throw new Error('A valid 160-byte key_retail.bin is required on the Flipper or selected locally.');

            // All fetching, minification, source fingerprinting, and binary index generation
            // completes before installPayload performs the first device mutation or upload.
            const payload = await prepareInstallPayload(keyFile, setMessage);
            setRelease(payload.release.manifest);
            setSizes({
                fap: payload.release.fap.length,
                key: payload.key?.length ?? deviceKeySize ?? 0,
                amiibo: payload.amiibo.minifiedBytes,
                games: payload.games.minifiedBytes,
                index: payload.index.bytes.length,
                amiiboOriginal: payload.amiibo.sourceBytes,
                gamesOriginal: payload.games.sourceBytes,
                figures: payload.index.figureCount,
                categories: payload.index.categoryCount,
                gameRefs: payload.index.gameRefCount,
            });

            setMessage(`Prepared ${payload.index.figureCount} figures in native index v11. Starting Flipper transaction…`);
            await installPayload(serialRef.current, payload, updateStep, updateProgress);
            if (payload.key) setDeviceKeySize(payload.key.length);
            setMessage(`Installation complete. Amiibo is now running on the Flipper Zero.`);
        } catch (caught) {
            const detail = caught instanceof Error ? caught.message : String(caught);
            setError(detail);
            setMessage('Installation stopped. Completed file transfers remain on the device.');
        } finally {
            setBusy(false);
        }
    }

    return <div className={styles.shell}>
        <Header connected={connected} theme={theme} onThemeToggle={() => setTheme(theme === 'dark' ? 'light' : 'dark')} />
        <main className={styles.main}>
            <Hero releaseLabel={releaseLabel} connected={connected} serialSupported={serialSupported} busy={busy} manualFapUrl={fapManualUrl} onConnect={() => void connect()} onDisconnect={() => void disconnect()} />

            {!serialSupported && <Notice title="Web Serial is not available in this browser." tone="error">Use desktop Chrome or Edge for direct USB installation, or download the release FAP above and install it manually with qFlipper.</Notice>}

            <div className={styles.grid}>
                <KeyCard connected={connected} deviceKeySize={deviceKeySize} keyFile={keyFile} onFile={(file) => { setKeyFile(file); setError(''); }} />
                <InstallSet steps={steps} sizes={sizes} releaseLabel={releaseLabel} hasValidDeviceKey={hasValidDeviceKey} keyFile={keyFile} />
            </div>

            <TransactionStatus cleanup={steps.cleanup} launch={steps.launch} />
            <InstallBar connected={connected} busy={busy} ready={ready} error={error} message={message} onInstall={() => void install()} />
            {connected && !busy && error && <div className={styles.noticeSpace}><Notice title="Installer stopped" tone="error">{error}</Notice></div>}
            <FeatureGrid />
            <SourceLinks manualFapUrl={fapManualUrl} />
        </main>
        <Footer />
    </div>;
}
