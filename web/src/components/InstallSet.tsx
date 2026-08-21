import {Icon} from './Icon';
import {ResourceRow, type UiStep} from './ResourceRow';
import {DEVICE_PATHS, formatBytes} from '../lib/installer';
import styles from './InstallSet.module.css';

export type InstallSizes = {fap?: number; key?: number; amiibo?: number; games?: number; index?: number; amiiboOriginal?: number; gamesOriginal?: number; figures?: number; categories?: number; gameRefs?: number};

export function InstallSet({steps, sizes, releaseLabel, hasValidDeviceKey, keyFile}: {
    steps: Record<string, UiStep>;
    sizes: InstallSizes;
    releaseLabel: string;
    hasValidDeviceKey: boolean;
    keyFile: File | null;
}) {
    const saved = (sizes.amiiboOriginal ?? 0) + (sizes.gamesOriginal ?? 0) - (sizes.amiibo ?? 0) - (sizes.games ?? 0);
    return <section className={styles.card}>
        <div className={styles.heading}><div><span>2 · Install set</span><h2>Release + data + index</h2></div><Icon name="download" size={24} /></div>
        <div className={styles.list}>
            <ResourceRow icon="package" title="Amiibo Zero" subtitle={DEVICE_PATHS.app} detail={sizes.fap ? formatBytes(sizes.fap) : releaseLabel} step={steps.fap} />
            <ResourceRow icon="key" title="key_retail.bin" subtitle={keyFile ? 'Local replacement key' : hasValidDeviceKey ? 'Existing key on Flipper' : 'Local 160-byte key'} detail={keyFile ? formatBytes(keyFile.size) : hasValidDeviceKey ? 'Already installed' : 'Required'} step={steps.key} />
            <ResourceRow icon="database" title="amiibo.json" subtitle="Fetched with cache-buster, parsed, then minified" detail={sizes.amiibo ? formatBytes(sizes.amiibo) : 'Fresh source'} step={steps.amiibo} />
            <ResourceRow icon="database" title="games_info.json" subtitle="Fetched with cache-buster, parsed, then minified" detail={sizes.games ? formatBytes(sizes.games) : 'Fresh source'} step={steps.games} />
            <ResourceRow icon="file" title="amiibo.idx" subtitle="Generated in-browser using native index v11 layout" detail={sizes.index ? formatBytes(sizes.index) : 'Pre-generated'} step={steps.index} />
        </div>
        {(sizes.figures != null || saved > 0) && <div className={styles.meta}>{sizes.figures != null && <span>{sizes.figures} figures · {sizes.categories} categories · {sizes.gameRefs} game refs</span>}{saved > 0 && <span>{formatBytes(saved)} JSON formatting removed before transfer</span>}</div>}
    </section>;
}
