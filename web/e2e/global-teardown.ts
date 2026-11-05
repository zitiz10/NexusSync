import { killServer, readState } from './server';

export default async function globalTeardown() {
  killServer(readState());
}
