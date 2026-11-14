import { BlobPreconditionFailedError, get, put } from '@vercel/blob';

const PATH = 'leaderboard/v1.json';
const LIMIT = 20;
const MAX_BODY_BYTES = 512;
const NO_CACHE = { 'Cache-Control': 'no-store' };
const READ_CACHE = {
  'Cache-Control': 'public, max-age=15',
  'Vercel-CDN-Cache-Control': 'public, s-maxage=30, stale-while-revalidate=60',
};
const PLAYER_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const DISPLAY_NAME = /^[\p{L}\p{M}\p{N}][\p{L}\p{M}\p{N} _.'-]*$/u;

async function readBoard() {
  const result = await get(PATH, { access: 'private', useCache: false });
  if (!result) return { entries: [], etag: null };
  return { entries: await new Response(result.stream).json(), etag: result.blob.etag };
}

function publicEntries(entries) {
  return entries.map(({ name, score }) => ({ name, score }));
}

function unavailable(error) {
  if (error) console.error('Leaderboard storage unavailable', error.name);
  return Response.json({ error: 'Leaderboard is temporarily unavailable.' }, { status: 503, headers: NO_CACHE });
}

export async function GET() {
  try {
    const { entries } = await readBoard();
    return Response.json({ entries: publicEntries(entries) }, { headers: READ_CACHE });
  } catch (error) {
    return unavailable(error);
  }
}

async function readSubmission(request) {
  const reader = request.body?.getReader();
  if (!reader) return null;
  let size = 0;
  const chunks = [];
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > MAX_BODY_BYTES) {
        await reader.cancel();
        return null;
      }
      chunks.push(value);
    }
    return JSON.parse(Buffer.concat(chunks).toString('utf8'));
  } catch {
    return null;
  } finally {
    reader.releaseLock();
  }
}

export async function POST(request) {
  const input = await readSubmission(request);
  const playerId = input?.playerId;
  const name = typeof input?.name === 'string' ? input.name.trim().normalize('NFC') : '';
  const score = input?.score;
  if (typeof playerId !== 'string' || !PLAYER_ID.test(playerId) || name.length > 16 || !DISPLAY_NAME.test(name) ||
      !Number.isSafeInteger(score) || score <= 0 || score > 99_999_990 || score % 10 !== 0) {
    return Response.json({ error: 'Enter a name of up to 16 letters, numbers or common punctuation and submit a valid score.' }, {
      status: 400, headers: NO_CACHE,
    });
  }

  try {
    let board = await readBoard();
    for (let attempt = 0; attempt < 4; attempt++) {
      const { entries, etag } = board;
      const personalBest = entries.find((entry) => entry.playerId === playerId);
      if (personalBest?.score >= score || (entries.length === LIMIT && entries.at(-1).score >= score)) {
        const rank = personalBest ? entries.indexOf(personalBest) + 1 : null;
        return Response.json({ entries: publicEntries(entries), rank, improved: false }, { headers: NO_CACHE });
      }

      const next = entries.filter((entry) => entry.playerId !== playerId);
      const index = next.findIndex((entry) => entry.score < score);
      const rank = index === -1 ? next.length + 1 : index + 1;
      next.splice(rank - 1, 0, { playerId, name, score });
      next.length = Math.min(next.length, LIMIT);

      try {
        await put(PATH, JSON.stringify(next), {
          access: 'private',
          contentType: 'application/json',
          addRandomSuffix: false,
          allowOverwrite: Boolean(etag),
          ...(etag ? { ifMatch: etag } : {}),
        });
        return Response.json({ entries: publicEntries(next), rank, improved: true }, { headers: NO_CACHE });
      } catch (error) {
        if (attempt === 3) {
          if (error instanceof BlobPreconditionFailedError) return unavailable();
          throw error;
        }
        if (etag && !(error instanceof BlobPreconditionFailedError)) throw error;
        // A create-only put can also lose a race to the first writer.
        board = await readBoard();
        if (!etag && !board.etag) throw error;
      }
    }
  } catch (error) {
    return unavailable(error);
  }
}
