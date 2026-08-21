import {SOURCE_URLS} from '../lib/installer';
import {Icon} from './Icon';
import styles from './SourceLinks.module.css';

export function SourceLinks({manualFapUrl}: {manualFapUrl: string}) {
    return <section className={styles.links}><strong>Resources</strong><a href={SOURCE_URLS.amiibo} target="_blank" rel="noreferrer">amiibo.json <Icon name="external" size={14} /></a><a href={SOURCE_URLS.games} target="_blank" rel="noreferrer">games_info.json <Icon name="external" size={14} /></a><a href={manualFapUrl} download>amiibo_zero.fap <Icon name="download" size={14} /></a></section>;
}
