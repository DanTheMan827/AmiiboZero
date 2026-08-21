import {Icon} from './Icon';
import type {UiStep} from './ResourceRow';
import styles from './TransactionStatus.module.css';

function Phase({icon, title, text, step}: {icon: 'trash' | 'play'; title: string; text: string; step: UiStep}) {
    return <div className={`${styles.phase} ${step.state === 'done' ? styles.done : ''} ${step.state === 'working' ? styles.working : ''}`}><span className={styles.phaseIcon}><Icon name={step.state === 'done' ? 'check' : icon} /></span><div><strong>{title}</strong><small>{text}</small></div></div>;
}

export function TransactionStatus({cleanup, launch}: {cleanup: UiStep; launch: UiStep}) {
    return <div className={styles.wrap}>
        <Phase icon="trash" title="Preflight cleanup" text="Stop Amiibo if running and delete every active/temp index file before the first upload." step={cleanup} />
        <span className={styles.arrow}>→</span>
        <Phase icon="play" title="Automatic launch" text="After all files verify, open the newly installed FAP through the Flipper loader." step={launch} />
    </div>;
}
