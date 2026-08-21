import {Icon} from './Icon';
import styles from './FeatureGrid.module.css';

const features = [
    {icon: 'shield' as const, title: 'Private by design', text: 'Your retail key is never sent to a web service. It is read locally and transferred only over the selected USB serial port.'},
    {icon: 'database' as const, title: 'Native-compatible index', text: 'The browser reproduces the ARM binary layout, UTF-8 byte truncation, sort order, source fingerprints, and JSON byte offsets used by the native app.'},
    {icon: 'refresh' as const, title: 'Always-fresh requests', text: 'release.json, amiibo.json, and games_info.json are requested with a current timestamp query parameter and no-store caching.'},
];

export function FeatureGrid() {
    return <section className={styles.grid}>{features.map((feature) => <div className={styles.card} key={feature.title}><Icon name={feature.icon} /><div><strong>{feature.title}</strong><p>{feature.text}</p></div></div>)}</section>;
}
