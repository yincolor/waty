// whiz TypeScript 全局声明（对应文档第 10 章）。
declare global {
    interface Window {
        whiz: {
            ipc?: {
                invoke(event: string, ...args: any[]): Promise<any>;

                createInvoker(options?: {
                    timeout?: number;
                    signal?: AbortSignal;
                }): (event: string, ...args: any[]) => Promise<any>;

                on(event: string, callback: (data: any) => void): () => void;
            };
            os?: {
                platform(): Promise<'win32' | 'darwin' | 'linux'>;
                arch(): Promise<'x64' | 'arm64'>;
                homedir(): Promise<string>;
                getPath(path: 'userData' | 'temp' | 'downloads' | 'documents' | 'appDir' | 'home' | 'cache' | 'logs'): Promise<string>;
            };
            fs?: {
                readFile(path: string): Promise<Uint8Array>;
                writeFile(path: string, data: Uint8Array): Promise<void>;
                createReadStream(path: string, options?: any): Promise<ReadableStream<Uint8Array>>;
                createWriteStream(path: string, options?: any): Promise<WritableStream<Uint8Array>>;
                stat(path: string): Promise<Stats>;
                exists(path: string): Promise<boolean>;
            };
            window?: {
                setTitle(title: string): Promise<void>;
                getSize(): Promise<{ width: number; height: number }>;
                setPosition(x: number, y: number): Promise<void>;
                center(): Promise<void>;
                maximize(): Promise<void>;
                minimize(): Promise<void>;
                restore(): Promise<void>;
                close(): Promise<void>;
                hide(): Promise<void>;
                show(): Promise<void>;
                focus(): Promise<void>;
                setAlwaysOnTop(enabled: boolean): Promise<void>;
                setDecorated(enabled: boolean): Promise<void>;
                openDevTools(): Promise<void>;
                isMaximized(): Promise<boolean>;
                isMinimized(): Promise<boolean>;
                isVisible(): Promise<boolean>;
            };
            dialog?: {
                showOpenDialog(options?: {
                    title?: string;
                    defaultPath?: string;
                    multiSelect?: boolean;
                    showHidden?: boolean;
                    directory?: boolean;
                    filters?: Array<{ name: string; extensions: string[] }>;
                }): Promise<{ canceled: boolean; filePaths: string[] }>;

                showSaveDialog(options?: {
                    title?: string;
                    defaultPath?: string;
                    defaultName?: string;
                    filters?: Array<{ name: string; extensions: string[] }>;
                }): Promise<{ canceled: boolean; filePath: string }>;

                showMessageBox(options: {
                    type?: 'none' | 'info' | 'warning' | 'error' | 'question';
                    title?: string;
                    message?: string;
                    detail?: string;
                    buttons: string[];
                    defaultButtonIndex?: number;
                    cancelButtonIndex?: number;
                    checkboxLabel?: string;
                    checkboxChecked?: boolean;
                    modal?: boolean;
                }): Promise<{ response: number; checkboxChecked: boolean }>;
            };
        };
    }
}

interface Stats {
    size: number;
    mtimeMs: number;
    mtimeISO: string;
    exists: boolean;
    isFile: boolean;
    isDirectory: boolean;
    type: 'file' | 'directory' | 'symlink' | 'other';
}

export {};
