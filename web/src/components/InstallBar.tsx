import {Icon} from './Icon';
import styles from './InstallBar.module.css';

export function InstallBar({connected, busy, ready, error, message, onInstall}: {connected: boolean; busy: boolean; ready: boolean; error: string; message: string; onInstall: () => void}) {
    const title = error ? 'Needs attention' : busy ? 'Working' : connected ? 'Ready to install' : 'Waiting for Flipper';
    return <section className={styles.bar}>
        <div className={styles.status}><span className={`${styles.light} ${error ? styles.error : busy ? styles.working : connected ? styles.ready : ''}`} /><div><strong>{title}</strong><span>{error || message}</span></div></div>
        <button className={styles.button} disabled={!ready} onClick={onInstall}>{busy ? <span className={styles.spinner} /> : <Icon name="download" />} Install everything</button>
    </section>;
}
