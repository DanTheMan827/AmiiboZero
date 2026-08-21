import {Icon} from './Icon';
import styles from './Hero.module.css';

export function Hero({releaseLabel, connected, serialSupported, busy, manualFapUrl, onConnect, onDisconnect}: {
    releaseLabel: string;
    connected: boolean;
    serialSupported: boolean;
    busy: boolean;
    manualFapUrl: string;
    onConnect: () => void;
    onDisconnect: () => void;
}) {
    return <section className={styles.hero}>
        <div className={styles.eyebrow}><span /> Release installer · {releaseLabel}</div>
        <h1>Install Amiibo Zero<br /><em>without leaving your browser.</em></h1>
        <p>Fresh databases, a native-compatible prebuilt index, your release-matched FAP, and an optional retail key are transferred directly to the Flipper over USB.</p>
        <div className={styles.actions}>
            {!connected ?
                <button className={styles.primary} onClick={onConnect} disabled={!serialSupported || busy}><Icon name="usb" /> Connect Flipper Zero</button> :
                <button className={styles.secondary} onClick={onDisconnect} disabled={busy}><Icon name="usb" /> Disconnect</button>}
            <a className={styles.manual} href={manualFapUrl} download><Icon name="download" /> Download .fap manually</a>
        </div>
        <span className={styles.browserNote}>Web Serial: desktop Chrome / Edge on HTTPS · Manual FAP download works everywhere</span>
    </section>;
}
