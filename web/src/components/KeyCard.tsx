import type {ChangeEvent} from 'react';
import {Icon} from './Icon';
import {DEVICE_PATHS, formatBytes} from '../lib/installer';
import styles from './KeyCard.module.css';

export function KeyCard({connected, deviceKeySize, keyFile, onFile}: {
    connected: boolean;
    deviceKeySize: number | null;
    keyFile: File | null;
    onFile: (file: File | null) => void;
}) {
    const validDeviceKey = deviceKeySize === 160;
    return <section className={styles.card}>
        <div className={styles.heading}><div><span>1 · Private key</span><h2>Amiibo retail key</h2></div><Icon name="shield" size={24} /></div>
        <p>A valid 160-byte key already on the Flipper is reused. Select a local key only when one is missing, invalid, or you want to replace it.</p>
        {connected && validDeviceKey && !keyFile && <div className={styles.present}><Icon name="check" /><div><strong>key_retail.bin already installed</strong><small>{DEVICE_PATHS.key} · 160 bytes</small></div></div>}
        {connected && deviceKeySize != null && !validDeviceKey && !keyFile && <div className={styles.warning}>Existing key is {deviceKeySize} bytes, so a 160-byte replacement is required.</div>}
        <label className={`${styles.picker} ${keyFile ? styles.selected : ''}`}>
            <input type="file" accept=".bin,application/octet-stream" onChange={(event: ChangeEvent<HTMLInputElement>) => onFile(event.target.files?.[0] ?? null)} />
            <span className={styles.pickerIcon}><Icon name={keyFile ? 'check' : 'key'} /></span>
            <span className={styles.pickerCopy}><strong>{keyFile ? keyFile.name : validDeviceKey ? 'Replace key_retail.bin (optional)' : 'Choose key_retail.bin'}</strong><small>{keyFile ? `${formatBytes(keyFile.size)} selected` : validDeviceKey ? 'Existing device key will be reused' : 'Expected size: exactly 160 bytes'}</small></span>
            <span className={styles.browse}>Browse</span>
        </label>
        {keyFile && keyFile.size !== 160 && <div className={styles.warning}>Selected file is {keyFile.size} bytes; key_retail.bin must be exactly 160 bytes.</div>}
    </section>;
}
