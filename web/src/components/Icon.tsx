import styles from './Icon.module.css';

export type IconName = 'usb' | 'shield' | 'download' | 'check' | 'key' | 'database' | 'package' | 'sun' | 'moon' | 'alert' | 'file' | 'refresh' | 'play' | 'trash' | 'external';

export function Icon({name, size = 20}: {name: IconName; size?: number}) {
    const paths: Record<IconName, React.ReactNode> = {
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
        file: <><path d="M6 2h8l4 4v16H6z" /><path d="M14 2v5h5" /></>,
        refresh: <><path d="M20 7v5h-5" /><path d="M4 17v-5h5" /><path d="M6.1 8.3A7 7 0 0 1 18.5 7L20 9M4 15l1.5 2A7 7 0 0 0 17.9 15.7" /></>,
        play: <path d="m8 5 11 7-11 7V5Z" />,
        trash: <><path d="M4 7h16M9 7V4h6v3m-8 0 1 14h8l1-14M10 11v6m4-6v6" /></>,
        external: <><path d="M14 4h6v6M20 4l-9 9" /><path d="M18 13v6H5V6h6" /></>,
    };
    return <svg className={styles.icon} width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{paths[name]}</svg>;
}
