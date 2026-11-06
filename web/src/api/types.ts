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
  progress?: { phase: string; elapsedMs: number; recordsReplayed?: number };
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
    bufferHits: number;
    bufferMisses: number;
  };
  containers?: { count: number; bytes: number; reclaimableBytes: number; liveHistogram: number[] };
  integrity?: { lastScanAt: string | null; verified: number; failures: number; running: boolean; percent: number };
  cleanup?: {
    pendingBytes: number;
    nextRunAt: string | null;
    running: boolean;
    lastRunAt: string | null;
    lastReclaimedBytes: number;
  };
  server?: {
    version: string;
    dataDir: string;
    chunker: 'cdc' | 'fixed';
    cdcMin: number;
    cdcAvg: number;
    cdcMax: number;
    fixedSize: number;
    containerSize: number;
    checkpointIntervalS: number;
    gcGraceS: number;
  };
}

/** engine.gc (M12, SPEC §12): progress, then the result. */
export type GcEvent =
  | { phase: 'freeing'; trigger: 'admin' | 'nightly' }
  | { phase: 'compacting'; done: number; total: number }
  | { phase: 'failed'; trigger: 'admin' | 'nightly' }
  | ({ phase: 'done' } & GcResult);

export interface GcResult {
  trigger: 'admin' | 'nightly';
  blocksFreed: number;
  bytesFreed: number;
  containersCompacted: number;
  containersSkipped: number;
  blocksMoved: number;
  bytesMoved: number;
  bytesReclaimed: number;
  durationMs: number;
}

/** One damaged block a scan found (also the engine.corruption payload). */
export interface ScanFailure {
  sha256: string;
  containerId: number;
  offset: number;
  reason: 'crc' | 'hash' | 'unreadable';
  files: { id: number; name: string }[];
}

export interface ScanResult {
  trigger: 'admin' | 'nightly';
  verified: number;
  failures: number;
  durationMs: number;
  items: ScanFailure[];
}

/** engine.scan: progress every 1%, then the result. */
export type ScanEvent =
  | { phase: 'scanning'; done: number; total: number; percent: number }
  | { phase: 'failed'; trigger: 'admin' | 'nightly' }
  | ({ phase: 'done' } & ScanResult);

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

/** One block in GET /api/compare (SPEC §12, D-043). */
export interface CompareBlock {
  index: number;
  size: number;
  sha256: string;
  /** The other version has a block with this hash. */
  reused: boolean;
  matchIndex: number | null;
  stored: { containerId: number; offset: number; sharedBy: number } | null;
}

export interface CompareResult {
  mode: 'fixed' | 'cdc';
  files: {
    a: { id: number; name: string; version: number; size: number };
    b: { id: number; name: string; version: number; size: number };
  };
  totals: {
    a: { chunks: number; bytes: number };
    b: { chunks: number; bytes: number; reused: number; sentBytes: number };
  };
  firstDifference: number | null;
  window: { from: number; count: number; a: CompareBlock[]; b: CompareBlock[] };
  /** Version 2's blocks as [value, run] pairs; 1 = also in version 1. */
  minimap: [number, number][];
}

// ---- admin (SPEC §12 "Admin", D-046) ----------------------------------------

export interface AdminUser {
  id: number;
  username: string;
  displayName: string;
  role: Role;
  status: 'active' | 'disabled';
  files: number;
  bytes: number;
  createdAt: string;
  lastSeenAt: string | null;
}

export interface Recovery {
  at: string;
  recordsReplayed: number;
  transactionsRedone: number;
  transactionsDiscarded: number;
  pagesRepaired: number;
  walBytesCut: number;
  durationMs: number;
  result: 'clean' | 'recovered';
}

export interface Addresses {
  port: number;
  listen: string;
  items: { address: string; url: string }[];
}
