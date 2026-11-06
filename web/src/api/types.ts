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

/** GET /api/files/{id}: a file with its version chain and block counts. */
export interface FileDetail extends FileItem {
  versions: { id: number; version: number; size: number; createdAt: string }[];
  blocks: { total: number; uniqueToFile: number; shared: number };
}

/** GET /api/stats (SPEC §12). Engine internals come for admins only. */
export interface Stats {
  logicalBytes: number;
  physicalBytes: number;
  ratio: number;
  writesSkippedBytes: number;
  networkSavedBytes: number;
  files: number;
  users: number;
  uploadsThisWeek: number;
  mine: { files: number; bytes: number; reusedPercent: number };
  uptimeS: number;
  wal?: {
    lsn: number;
    recordsSinceCheckpoint: number;
    sizeBytes: number;
    lastCheckpointAt: string | null;
    lastCheckpointMs: number;
  };
  index?: {
    entries: number;
    globalDepth: number;
    buckets: number;
    avgFillPercent: number;
    bufferHitRate: number | null;
    dirtyPages: number;
  };
  containers?: { count: number; bytes: number; reclaimableBytes: number; liveHistogram: number[] };
  integrity?: { lastScanAt: string | null; verified: number; failures: number };
  cleanup?: { pendingBytes: number; nextRunAt: string | null };
}

/** stats.updated: the global numbers only. */
export type StatsUpdate = Pick<
  Stats,
  | 'logicalBytes'
  | 'physicalBytes'
  | 'ratio'
  | 'writesSkippedBytes'
  | 'networkSavedBytes'
  | 'files'
  | 'users'
  | 'uploadsThisWeek'
>;

/** GET /api/blockmap: share level 0–4 per tile, in storage order. */
export interface BlockMap {
  tileBytes: number;
  physicalBytes: number;
  blocks: number;
  tiles: number[];
}

/** GET /api/blockmap?file=: the tiles holding that file's blocks. */
export interface FileTiles {
  fileId: number;
  tileBytes: number;
  tiles: number[];
}

/** upload.progress. Other students' uploads arrive anonymised (uploadId null). */
export interface UploadProgressEvent {
  uploadId: string | null;
  fileName?: string;
  phase: 'manifest' | 'sending' | 'committing';
  done?: number;
  total?: number;
  newTiles: number[];
}

export interface UploadCreated {
  uploadId: string;
  manifestPageSize: number;
  maxChunkBody: number;
}

export interface ChunksReply {
  accepted: number[];
  rejected: { index: number; reason: 'hash_mismatch' | 'length_mismatch' | 'unknown_index' }[];
}

export interface CommitReply extends FileItem {
  stats: { chunks: number; newChunks: number; bytesSent: number; reusedPercent: number };
}
