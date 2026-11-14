'use client';

import { useEffect, useRef, useState } from 'react';
import { createEmulator, loadCartridge } from './emulator';
import Leaderboard from './leaderboard';
import { savedHighScore } from './cartridge-score';

const keyboard = new Map(Object.entries({
  ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
  KeyW: 'up', KeyS: 'down', KeyA: 'left', KeyD: 'right',
  KeyZ: 'A', KeyX: 'B', Enter: 'start', ShiftLeft: 'select', ShiftRight: 'select',
}));

export default function Player() {
  const canvas = useRef(null);
  const cabinet = useRef(null);
  const session = useRef(null);
  const [status, setStatus] = useState('ready');
  const [highScore, setHighScore] = useState(0);
  const [muted, setMuted] = useState(false);
  const [fullscreen, setFullscreen] = useState(false);
  const [fullscreenAvailable, setFullscreenAvailable] = useState(false);

  useEffect(() => {
    setHighScore(savedHighScore());
    function keyDown(event) {
      const button = keyboard.get(event.code);
      if (!session.current?.emulator || !button || event.altKey || event.ctrlKey || event.metaKey) return;
      if (event.target !== canvas.current && event.target.closest?.('button, a')) return;
      event.preventDefault();
      session.current.emulator.setButton(button, event.code, true);
    }

    function keyUp(event) {
      const button = keyboard.get(event.code);
      if (button) session.current?.emulator?.setButton(button, event.code, false);
    }

    function release() {
      session.current?.emulator?.releaseButtons();
    }

    function syncFullscreen() {
      setFullscreen(document.fullscreenElement === cabinet.current);
    }

    setFullscreenAvailable(Boolean(document.fullscreenEnabled && cabinet.current.requestFullscreen));
    window.addEventListener('keydown', keyDown);
    window.addEventListener('keyup', keyUp);
    window.addEventListener('blur', release);
    document.addEventListener('visibilitychange', release);
    document.addEventListener('fullscreenchange', syncFullscreen);
    return () => {
      window.removeEventListener('keydown', keyDown);
      window.removeEventListener('keyup', keyUp);
      window.removeEventListener('blur', release);
      document.removeEventListener('visibilitychange', release);
      document.removeEventListener('fullscreenchange', syncFullscreen);
      const active = session.current;
      session.current = null;
      active?.emulator?.dispose();
      active?.audio?.close().catch(() => {});
    };
  }, []);

  async function start() {
    if (session.current) return;
    const active = {};
    session.current = active;
    setStatus('loading');

    try {
      const AudioContext = window.AudioContext ?? window.webkitAudioContext;
      active.audio = AudioContext ? new AudioContext() : null;
      active.audio?.resume().catch(() => {});
    } catch {
      // Web Audio is optional for gameplay.
    }

    try {
      const [core, rom] = await loadCartridge();
      if (session.current !== active) return;
      active.emulator = createEmulator(core, rom, canvas.current, active.audio, setHighScore);
      canvas.current.focus({ preventScroll: true });
      setStatus('playing');
    } catch (error) {
      if (session.current !== active) return;
      console.error(error);
      active.audio?.close().catch(() => {});
      session.current = null;
      setStatus('error');
    }
  }

  function toggleMute() {
    const next = !muted;
    session.current?.emulator?.mute(next);
    if (!next) session.current?.audio?.resume().catch(() => {});
    setMuted(next);
    canvas.current.focus({ preventScroll: true });
  }

  async function toggleFullscreen() {
    try {
      if (document.fullscreenElement === cabinet.current) await document.exitFullscreen();
      else await cabinet.current.requestFullscreen();
      canvas.current.focus({ preventScroll: true });
    } catch (error) {
      console.warn('Fullscreen is unavailable.', error);
    }
  }

  function control(button, label, key, className = '') {
    function release(event) {
      session.current?.emulator?.setButton(button, `pointer-${event.pointerId}`, false);
    }
    return (
      <button
        type="button"
        className={className}
        disabled={status !== 'playing'}
        onPointerDown={(event) => {
          if (event.button !== 0) return;
          event.preventDefault();
          event.currentTarget.setPointerCapture(event.pointerId);
          session.current?.emulator?.setButton(button, `pointer-${event.pointerId}`, true);
          canvas.current.focus({ preventScroll: true });
        }}
        onLostPointerCapture={release}
        onKeyDown={(event) => {
          if (event.code !== 'Space' && event.code !== 'Enter') return;
          event.preventDefault();
          session.current?.emulator?.setButton(button, `control-${event.code}`, true);
        }}
        onKeyUp={(event) => {
          if (event.code !== 'Space' && event.code !== 'Enter') return;
          event.preventDefault();
          session.current?.emulator?.setButton(button, `control-${event.code}`, false);
        }}
        onBlur={() => {
          session.current?.emulator?.setButton(button, 'control-Space', false);
          session.current?.emulator?.setButton(button, 'control-Enter', false);
        }}
        aria-label={label}
      >
        <kbd>{key}</kbd>
        {className !== 'direction' && <span>{label}</span>}
      </button>
    );
  }

  return <>
    <section className="cabinet" ref={cabinet} aria-label="Canyon Racer game player">
      <div className="game-panel">
        <div className="screen">
          <canvas ref={canvas} width="160" height="144" tabIndex={0} aria-label="Game Boy screen" />
          {status !== 'playing' && (
            <div className="launch">
              {status === 'error' && <p role="alert">The game couldn’t load.</p>}
              <button type="button" onClick={start} disabled={status === 'loading'}>
                <span aria-hidden="true">▶</span>
                {status === 'loading' ? 'Loading…' : status === 'error' ? 'Try again' : 'Play game'}
              </button>
            </div>
          )}
        </div>
        <div className="game-toolbar">
          <button type="button" onClick={toggleMute} disabled={status !== 'playing'} aria-pressed={muted}>
            {muted ? 'Muted' : 'Sound'}
          </button>
          {fullscreenAvailable && <button type="button" className="fullscreen-button" onClick={toggleFullscreen}>{fullscreen ? 'Exit fullscreen' : 'Fullscreen'}</button>}
        </div>
      </div>
      <aside className="controls">
        <h2>Controls</h2>
        <div className="steering">
          <div className="dpad" role="group" aria-label="Steering">
            {control('up', 'Move up', '↑', 'direction')}
            {control('left', 'Move left', '←', 'direction')}
            {control('down', 'Move down', '↓', 'direction')}
            {control('right', 'Move right', '→', 'direction')}
          </div>
          <span>Steer</span>
        </div>
        <div className="actions">
          {control('A', 'Jump', 'Z')}
          {control('B', 'Set checkpoint', 'X')}
          {control('start', 'Start / Pause', 'Enter')}
          {control('select', 'Select', 'Shift')}
        </div>
        <p className="tip">Hold <kbd>↑</kbd> + <kbd>Z</kbd> for a longer jump. Setting a checkpoint costs one life.</p>
      </aside>
    </section>
    <Leaderboard highScore={highScore} />
  </>;
}
