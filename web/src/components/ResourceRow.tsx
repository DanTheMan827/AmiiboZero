import {Icon, type IconName} from './Icon';
import type {InstallStepId} from '../lib/installer';
import styles from './ResourceRow.module.css';

export type UiStep = {state: 'idle' | 'working' | 'done' | 'error'; progress: number};

export function ResourceRow({icon, title, subtitle, detail, step}: {icon: IconName; title: string; subtitle: string; detail?: string; step: UiStep; id?: InstallStepId}) {
    const active = step.state === 'working';
    const done = step.state === 'done';
    return <div className={`${styles.row} ${active ? styles.active : ''} ${done ? styles.done : ''}`}>
        <div className={styles.icon}><Icon name={done ? 'check' : icon} /></div>
        <div className={styles.copy}>
            <div className={styles.title}><strong>{title}</strong>{detail && <span>{detail}</span>}</div>
            <small>{subtitle}</small>
            {active && <div className={styles.track}><div className={styles.fill} style={{width: `${Math.max(2, step.progress * 100)}%`}} /></div>}
        </div>
        <span className={`${styles.status} ${styles[step.state]}`} />
    </div>;
}
