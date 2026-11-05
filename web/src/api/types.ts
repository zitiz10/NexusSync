// Types of the server's JSON (SPEC §12). Field names match the API exactly.

export type Role = 'student' | 'admin';

export interface User {
  id: number;
  username: string;
  displayName: string;
  role: Role;
}

export type HealthStatus = 'ok' | 'recovering' | 'stopping' | 'failed';

export interface Health {
  status: HealthStatus;
  version: string;
  progress?: { phase: string; elapsedMs: number };
}

export interface FileItem {
  id: number;
  name: string;
  version: number;
  prevVersionId: number | null;
  ownerId: number;
  ownerName: string;
  size: number;
  chunkCount: number;
  chunker: 'cdc' | 'fixed';
  sha256: string;
  bytesSent: number;
  reusedPercent: number;
  createdAt: string;
  shared: boolean;
}

export interface ErrorBody {
  error: { code: string; message: string; indices?: number[] };
}

/** A server-sent event (SPEC §12 "WebSocket events"). */
export interface ServerEvent {
  type: string;
  at: string;
  data: unknown;
}
