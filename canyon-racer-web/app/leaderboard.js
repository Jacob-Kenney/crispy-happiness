'use client';

import { useCallback, useEffect, useRef, useState } from 'react';

const ENDPOINT = '/api/leaderboard';
const PROFILE_KEY = 'canyon-racer.leaderboard-profile';
const formatScore = (score) => String(score).padStart(5, '0');

function readProfile() {
  try {
    return JSON.parse(localStorage.getItem(PROFILE_KEY)) ?? {};
  } catch {
    return {};
  }
}

export default function Leaderboard({ highScore }) {
  const profile = useRef(null);
  const attemptedScore = useRef(0);
  const inFlight = useRef(false);
  const [entries, setEntries] = useState(null);
  const [name, setName] = useState('');
  const [submittedScore, setSubmittedScore] = useState(0);
  const [loading, setLoading] = useState(true);
  const [sending, setSending] = useState(false);
  const [message, setMessage] = useState('');
  const [error, setError] = useState('');

  async function refresh(signal, fresh = false) {
    setLoading(true);
    setError('');
    try {
      const response = await fetch(fresh ? `${ENDPOINT}?fresh=1` : ENDPOINT, { signal });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error);
      setEntries(data.entries);
    } catch (error) {
      if (!signal?.aborted) setError(error.message || 'Could not load leaderboard.');
    } finally {
      if (!signal?.aborted) setLoading(false);
    }
  }

  useEffect(() => {
    const saved = readProfile();
    profile.current = saved;
    setName(saved.name ?? '');
    setSubmittedScore(saved.score ?? 0);
    const controller = new AbortController();
    refresh(controller.signal);
    return () => controller.abort();
  }, []);

  const submitScore = useCallback(async (name, score) => {
    if (inFlight.current) return;
    inFlight.current = true;
    attemptedScore.current = score;
    const playerId = profile.current?.playerId ?? crypto.randomUUID();
    const submission = { playerId, name: name.trim(), score };
    // Reuse the same player ID if the request succeeds but the response is lost.
    profile.current = { ...profile.current, playerId };
    try { localStorage.setItem(PROFILE_KEY, JSON.stringify(profile.current)); } catch {}
    setSending(true);
    setError('');
    setMessage('');
    try {
      const response = await fetch(ENDPOINT, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(submission),
      });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error);
      setEntries(data.entries);
      setSubmittedScore(submission.score);
      setMessage(data.improved ? `Score posted at #${data.rank}.` : 'Your score did not change the leaderboard.');
      profile.current = submission;
      try { localStorage.setItem(PROFILE_KEY, JSON.stringify(submission)); } catch {}
    } catch (error) {
      setError(error.message || 'Could not submit score.');
    } finally {
      inFlight.current = false;
      setSending(false);
    }
  }, []);

  const canSubmit = highScore > submittedScore && highScore % 10 === 0;

  useEffect(() => {
    const saved = profile.current;
    if (!canSubmit || loading || sending || !submittedScore || !saved?.name || highScore === attemptedScore.current) return;
    submitScore(saved.name, highScore);
  }, [canSubmit, highScore, loading, sending, submittedScore, submitScore]);

  return (
    <section className="leaderboard" aria-labelledby="leaderboard-title">
      <div className="leaderboard-heading">
        <h2 id="leaderboard-title">Leaderboard</h2>
        <button type="button" onClick={() => refresh(undefined, true)} disabled={loading || sending}>Refresh</button>
      </div>
      {entries?.length > 0 && (
        <ol className="leaderboard-scores">
          {entries.map(({ name, score }, index) => (
            <li key={index}><span>{name}</span><strong>{formatScore(score)}</strong></li>
          ))}
        </ol>
      )}
      {entries?.length === 0 && <p>No scores yet.</p>}
      {loading && entries === null && <p role="status">Loading scores…</p>}
      <p>{highScore > 0 ? `Your best: ${formatScore(highScore)}` : 'Finish a race to submit your best score.'}</p>
      {canSubmit && (
        <form onSubmit={(event) => { event.preventDefault(); submitScore(name, highScore); }}>
          <label htmlFor="leaderboard-name">Display name</label>
          <div>
            <input id="leaderboard-name" maxLength={16} required value={name} onChange={(event) => setName(event.target.value)} />
            <button type="submit" disabled={loading || sending}>{sending ? 'Submitting…' : 'Submit score'}</button>
          </div>
          {!submittedScore && <p>Future personal bests will post under this name.</p>}
        </form>
      )}
      {highScore % 10 !== 0 && <p>This cartridge score is ineligible for the leaderboard.</p>}
      {message && <p role="status">{message}</p>}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
