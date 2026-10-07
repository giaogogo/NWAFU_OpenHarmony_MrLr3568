export type OnDataCallback = (port: string, data: string) => void;

export const open:      (port: string, baudRate: number) => boolean;
export const send:      (port: string, data: string) => number;
export const read:      (port: string) => string;
export const onData:    (port: string, cb: OnDataCallback) => boolean;
export const offData:   (port: string) => boolean;
export const close:     (port: string) => boolean;
export const closeAll:  () => boolean;
export const isOpen:    (port: string) => boolean;
export const listPorts: () => string[];