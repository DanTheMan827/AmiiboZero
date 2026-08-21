import {Icon} from './Icon';
import styles from './Notice.module.css';

export function Notice({title, children, tone = 'info'}: {title: string; children: React.ReactNode; tone?: 'info' | 'error' | 'success'}) {
    return <div className={`${styles.notice} ${styles[tone]}`}><Icon name={tone === 'success' ? 'check' : 'alert'} /><div><strong>{title}</strong><span>{children}</span></div></div>;
}
