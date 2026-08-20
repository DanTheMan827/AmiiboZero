import {useEffect, useMemo, useRef, useState} from 'react';
import {FlipperSerial} from './lib/flipperSerial.js';
import {
    DEVICE_PATHS,
    SOURCE_URLS,
    formatBytes,
    installPayload,
    loadRelease,
    prepareInstallPayload,
} from './lib/installer.js';

const initialSteps = {
    fap: {state: 'idle', progress: 0},
    key: {state: 'idle', progress: 0},
    amiibo: {state: 'idle', progress: 0},
    games: {state: 'idle', progress: 0},
};

function Icon({name, size = 20}) {
    const paths = {
        usb: <path d="M12 2v14m0-14 3 3m-3-3-3 3m3 11 4-4m-4 4-4-4M7 7H4v4m13-4h3v4" />,
        shield: <path d="M12 3 5 6v5c0 4.6 2.7 8 7 10 4.3-2 7-5.4 7-10V6l-7-3Zm-3 9 2 2 4-4" />,
        download: <path d="M12 3v12m0 0 4-4m-4 4-4-4M5 21h14" />,
        check: <path d="m5 12 4 4L19 6" />,
        key: <path d="M15 7a4 4 0 1 1-1.2 2.8L21 17v3h-3v-2h-2v-2h-2l-2.2-2.2" />,
        database: <><ellipse cx="12" cy="5" rx="8" ry="3" /><path d="M4 5v6c0 1.7 3.6 3 8 3s8-1.3 8-3V5M4 11v6c0 1.7 3.6 3 8 3s8-1.3 8-3v-6" /></>,
        package: <><path d="m4 7 8-4 8 4-8 4-8-4Z" /><path d="M4 7v10l8 4 8-4V7M12 11v10" /></>,
        sun: <><circle cx="12" cy="12" r="4" /><path d="M12 2v2m0 16v2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4M2 12h2m16 0h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4" /></>,
        moon: <path d="M20 15.2A8.5 8.5 0 0 1 8.8 4 8.5 8.5 0 1 0 20 15.2Z" />,
        alert: <><path d="M12 3 2.8 20h18.4L12 3Z" /><path d="M12 9v5m0 3h.01" /></>,
    };
    return <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{paths[name]}</svg>;
}

function ResourceRow({icon, title, subtitle, detail, step}) {
    const active = step.state === 'uploading';
    const done = step.state === 'done';
    return (
        <div className={`resource-row ${active ? 'is-active' : ''} ${done ? 'is-done' : ''}`}>
            <div className="resource-icon"><Icon name={done ? 'check' : icon} /></div>
            <div className="resource-copy">
                <div className="resource-title-line"><strong>{title}</strong>{detail && <span>{detail}</span>}</div>
                <div className="resource-subtitle">{subtitle}</div>
                {active && <div className="progress-track"><div className="progress-fill" style={{width: `${Math.max(2, step.progress * 100)}%`}} /></div>}
            </div>
            <div className={`status-dot ${step.state}`} title={step.state} />
        </div>
    );
}

function App() {
    const serialRef = useRef(new FlipperSerial());
    const [theme, setTheme] = useState(() => localStorage.getItem('az-theme') || (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'));
    const [connected, setConnected] = useState(false);
    const [busy, setBusy] = useState(false);
    const [keyFile, setKeyFile] = useState(null);
    const [message, setMessage] = useState('Connect your Flipper Zero to begin.');
    const [error, setError] = useState('');
    const [release, setRelease] = useState(null);
    const [sizes, setSizes] = useState({});
    const [steps, setSteps] = useState(initialSteps);

    const serialSupported = FlipperSerial.isSupported();
    const ready = connected && keyFile && !busy && serialSupported && release;

    useEffect(() => {
        document.documentElement.dataset.theme = theme;
        localStorage.setItem('az-theme', theme);
    }, [theme]);

    useEffect(() => {
        loadRelease()
            .then(({manifest, fap}) => {
                setRelease({...manifest, embeddedSize: fap.length});
                setSizes((current) => ({...current, fap: fap.length}));
            })
            .catch(() => setRelease(null));
    }, []);

    useEffect(() => () => {
        serialRef.current.disconnect();
    }, []);

    const releaseLabel = useMemo(() => {
        if (!release) return 'Release FAP unavailable';
        return release.version ? `v${release.version}` : 'Release build';
    }, [release]);

    const updateStep = (id, state, statusMessage) => {
        setSteps((current) => ({...current, [id]: {...current[id], state, progress: state === 'done' ? 1 : current[id].progress}}));
        if (statusMessage) setMessage(statusMessage);
    };

    const updateProgress = (id, progress) => {
        setSteps((current) => ({...current, [id]: {...current[id], progress}}));
    };

    async function connect() {
        setError('');
        setBusy(true);
        setMessage('Opening Web Serial connection…');
        try {
            const info = await serialRef.current.connect();
            const model = info.match(/hardware_model\s*:\s*([^\r\n]+)/i)?.[1]?.trim() || 'Flipper Zero';
            setConnected(true);
            setMessage(`${model} connected. Choose your key file, then install.`);
        } catch (err) {
            setConnected(false);
            setError(err.message);
            setMessage('Connection failed.');
            await serialRef.current.disconnect();
        } finally {
            setBusy(false);
        }
    }

    async function disconnect() {
        setBusy(true);
        await serialRef.current.disconnect();
        setConnected(false);
        setBusy(false);
        setMessage('Flipper disconnected.');
    }

    async function install() {
        setError('');
        setBusy(true);
        setSteps(initialSteps);
        try {
            const payload = await prepareInstallPayload(keyFile, setMessage);
            setSizes({
                fap: payload.release.fap.length,
                key: payload.key.length,
                amiibo: payload.amiibo.minifiedBytes,
                games: payload.games.minifiedBytes,
                amiiboOriginal: payload.amiibo.sourceBytes,
                gamesOriginal: payload.games.sourceBytes,
            });

            setMessage('Preparing Flipper storage…');
            await installPayload(serialRef.current, payload, updateStep, updateProgress);
            setMessage(`Installation complete. Launch Amiibo from Apps → NFC on your Flipper Zero.`);
        } catch (err) {
            setError(err.message);
            setMessage('Installation stopped. Files that completed before the error remain installed.');
        } finally {
            setBusy(false);
        }
    }

    return (
        <div className="app-shell">
            <header className="topbar">
                <a className="brand" href="./" aria-label="Amiibo Zero Installer home">
                    <span className="brand-mark">AZ</span>
                    <span><strong>Amiibo Zero</strong><small>Web Installer</small></span>
                </a>
                <div className="topbar-actions">
                    <span className={`connection-pill ${connected ? 'connected' : ''}`}><span />{connected ? 'Flipper connected' : 'Not connected'}</span>
                    <button className="icon-button" onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')} title={`Use ${theme === 'dark' ? 'light' : 'dark'} theme`}>
                        <Icon name={theme === 'dark' ? 'sun' : 'moon'} />
                    </button>
                </div>
            </header>

            <main>
                <section className="hero">
                    <div className="eyebrow"><span className="pulse-dot" /> Release installer · {releaseLabel}</div>
                    <h1>Install Amiibo Zero<br /><span>directly from your browser.</span></h1>
                    <p>Connect over USB, provide your own retail key, and the installer will transfer the release FAP plus freshly downloaded, minified Amiibo databases to your Flipper Zero.</p>
                    <div className="hero-actions">
                        {!connected ? (
                            <button className="primary-button" onClick={connect} disabled={!serialSupported || busy}><Icon name="usb" /> Connect Flipper Zero</button>
                        ) : (
                            <button className="secondary-button" onClick={disconnect} disabled={busy}><Icon name="usb" /> Disconnect</button>
                        )}
                        <span className="browser-note">Chrome / Edge desktop · HTTPS</span>
                    </div>
                </section>

                {!serialSupported && (
                    <div className="notice error-notice"><Icon name="alert" /><div><strong>Web Serial is not available here.</strong><span>Open this page in desktop Chrome or Edge. Web Serial also requires HTTPS, which GitHub Pages provides.</span></div></div>
                )}

                <section className="installer-grid">
                    <div className="panel setup-panel">
                        <div className="panel-heading">
                            <div><span className="section-kicker">1 · Private key</span><h2>Select key_retail.bin</h2></div>
                            <Icon name="shield" size={24} />
                        </div>
                        <p>Your 160-byte combined Amiibo retail key is read locally by the browser and sent only to the connected Flipper. It is never uploaded to this site.</p>
                        <label className={`file-picker ${keyFile ? 'has-file' : ''}`}>
                            <input type="file" accept=".bin,application/octet-stream" onChange={(event) => {
                                const file = event.target.files?.[0] || null;
                                setKeyFile(file);
                                setError('');
                            }} />
                            <span className="file-picker-icon"><Icon name={keyFile ? 'check' : 'key'} /></span>
                            <span className="file-picker-copy">
                                <strong>{keyFile ? keyFile.name : 'Choose key_retail.bin'}</strong>
                                <small>{keyFile ? `${formatBytes(keyFile.size)} selected` : 'Expected size: exactly 160 bytes'}</small>
                            </span>
                            <span className="file-picker-action">Browse</span>
                        </label>
                        {keyFile && keyFile.size !== 160 && <div className="inline-warning">This file is {keyFile.size} bytes. A combined key_retail.bin must be exactly 160 bytes.</div>}

                        <div className="path-card">
                            <span>Files are installed to</span>
                            <code>{DEVICE_PATHS.dataDirectory}/</code>
                        </div>
                    </div>

                    <div className="panel resources-panel">
                        <div className="panel-heading">
                            <div><span className="section-kicker">2 · Install set</span><h2>Release + databases</h2></div>
                            <Icon name="download" size={24} />
                        </div>
                        <div className="resource-list">
                            <ResourceRow icon="package" title="Amiibo Zero" subtitle={`${DEVICE_PATHS.app}`} detail={sizes.fap ? formatBytes(sizes.fap) : releaseLabel} step={steps.fap} />
                            <ResourceRow icon="key" title="key_retail.bin" subtitle="Your local 160-byte key" detail={keyFile ? formatBytes(keyFile.size) : 'Required'} step={steps.key} />
                            <ResourceRow icon="database" title="amiibo.json" subtitle="Fetched live, parsed, then JSON.stringify() minified" detail={sizes.amiibo ? formatBytes(sizes.amiibo) : 'Current database'} step={steps.amiibo} />
                            <ResourceRow icon="database" title="games_info.json" subtitle="Fetched live, parsed, then JSON.stringify() minified" detail={sizes.games ? formatBytes(sizes.games) : 'Current database'} step={steps.games} />
                        </div>
                        {(sizes.amiiboOriginal || sizes.gamesOriginal) && (
                            <div className="compression-note">JSON minification saved {formatBytes((sizes.amiiboOriginal || 0) + (sizes.gamesOriginal || 0) - (sizes.amiibo || 0) - (sizes.games || 0))} before transfer.</div>
                        )}
                    </div>
                </section>

                <section className="install-bar">
                    <div className="install-status">
                        <span className={`status-light ${error ? 'error' : busy ? 'working' : connected ? 'ready' : ''}`} />
                        <div><strong>{error ? 'Needs attention' : busy ? 'Working' : connected ? 'Ready to install' : 'Waiting for Flipper'}</strong><span>{error || message}</span></div>
                    </div>
                    <button className="primary-button install-button" disabled={!ready || keyFile?.size !== 160} onClick={install}>
                        {busy ? <span className="spinner" /> : <Icon name="download" />} Install everything
                    </button>
                </section>

                <section className="details-grid">
                    <div className="detail-card"><Icon name="shield" /><div><strong>Private by design</strong><p>Your retail key never leaves this browser except over the USB serial connection to your Flipper.</p></div></div>
                    <div className="detail-card"><Icon name="database" /><div><strong>Fresh database files</strong><p>The installer fetches the current AmiiboData JSON files at install time, validates them by parsing, and transfers minified JSON.</p></div></div>
                    <div className="detail-card"><Icon name="package" /><div><strong>Release-matched FAP</strong><p>The website bundles the release-channel FAP from the same GitHub Actions run that produced the GitHub Release.</p></div></div>
                </section>

                <section className="source-note">
                    <strong>Database sources</strong>
                    <a href={SOURCE_URLS.amiibo} target="_blank" rel="noreferrer">amiibo.json</a>
                    <a href={SOURCE_URLS.games} target="_blank" rel="noreferrer">games_info.json</a>
                </section>
            </main>

            <footer><span>Amiibo Zero Web Installer</span><span>Close Amiibo Zero on-device before replacing the FAP.</span></footer>
        </div>
    );
}

export default App;
