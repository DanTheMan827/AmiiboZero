import {Icon} from './Icon';
import styles from './Header.module.css';

export function Header({connected, theme, onThemeToggle}: {connected: boolean; theme: 'light' | 'dark'; onThemeToggle: () => void}) {
    return <header className={styles.header}>
        <a className={styles.brand} href="./" aria-label="Amiibo Zero installer home">
            <span className={styles.brandMark}>AZ</span>
            <span className={styles.brandCopy}><strong>Amiibo Zero</strong><small>Web Installer</small></span>
        </a>
        <div className={styles.actions}>
            <span className={`${styles.connection} ${connected ? styles.connected : ''}`}><span className={styles.dot} />{connected ? 'Flipper connected' : 'Not connected'}</span>
            <button className={styles.themeButton} onClick={onThemeToggle} title={`Use ${theme === 'dark' ? 'light' : 'dark'} theme`} aria-label="Toggle color scheme">
                <Icon name={theme === 'dark' ? 'sun' : 'moon'} />
            </button>
        </div>
    </header>;
}
