import { cartridgeHighScore } from './cartridge-score';

const CORE_URL = '/emulator/binjgb.js';
const ROM_URL = '/rom/canyon-racer.gb';
const SAVE_KEY = 'canyon-racer.cartridge-ram';
const AUDIO_FRAMES = 2048;
const TICKS_PER_SECOND = 4_194_304;
const FRAME_READY = 1;
const AUDIO_READY = 2;
const TARGET_REACHED = 4;
// The ROM's title checksum ($35) selects the Mario's Picross palette on Game Boy Color.
const CANYON_RACER_PALETTE = 16;
const BUTTONS = ['up', 'down', 'left', 'right', 'A', 'B', 'start', 'select'];

let resources;

function loadCore() {
  if (window.Binjgb) return window.Binjgb();

  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = CORE_URL;
    script.onload = () => {
      script.remove();
      if (window.Binjgb) resolve(window.Binjgb());
      else reject(new Error('The emulator did not initialize.'));
    };
    script.onerror = () => {
      script.remove();
      reject(new Error('The emulator could not be loaded.'));
    };
    document.head.append(script);
  });
}

export function loadCartridge() {
  resources ??= Promise.all([
    loadCore(),
    fetch(ROM_URL).then((response) => {
      if (!response.ok) throw new Error('The game ROM could not be loaded.');
      return response.arrayBuffer();
    }).then((buffer) => new Uint8Array(buffer)),
  ]).catch((error) => {
    resources = undefined;
    throw error;
  });
  return resources;
}

export function createEmulator(core, rom, canvas, audio, onHighScore) {
  const context = canvas.getContext('2d', { alpha: false });
  if (!context) throw new Error('This browser could not initialize the game screen.');

  const frame = context.createImageData(canvas.width, canvas.height);
  const romPointer = core._malloc(rom.length);
  core.HEAPU8.set(rom, romPointer);
  const sampleRate = audio?.sampleRate ?? 48_000;
  const emulator = core._emulator_new_simple(romPointer, rom.length, sampleRate, AUDIO_FRAMES, 2);
  if (!emulator) {
    core._free(romPointer);
    throw new Error('The emulator could not open the cartridge.');
  }

  const joypad = core._joypad_new();
  core._emulator_set_default_joypad_callback(emulator, joypad);
  core._emulator_set_builtin_palette(emulator, CANYON_RACER_PALETTE);
  const framePointer = core._get_frame_buffer_ptr(emulator);
  const audioPointer = core._get_audio_buffer_ptr(emulator);
  const output = audio?.createGain();
  if (output) {
    output.gain.value = 0.45;
    output.connect(audio.destination);
  }

  let animation;
  let previousTime;
  let targetTicks = core._emulator_get_ticks_f64(emulator);
  let nextAudioTime = 0;
  const held = new Map(BUTTONS.map((button) => [button, new Set()]));

  function withCartridgeRam(readOrWrite) {
    const file = core._ext_ram_file_data_new(emulator);
    try {
      const pointer = core._get_file_data_ptr(file);
      const size = core._get_file_data_size(file);
      readOrWrite(file, core.HEAPU8.subarray(pointer, pointer + size));
    } finally {
      core._file_data_delete(file);
      core._free(file);
    }
  }

  let highScore = 0;
  try {
    const saved = localStorage.getItem(SAVE_KEY);
    if (saved) {
      withCartridgeRam((file, ram) => {
        const bytes = atob(saved);
        if (bytes.length !== ram.length) return;
        for (let index = 0; index < ram.length; index++) ram[index] = bytes.charCodeAt(index);
        core._emulator_read_ext_ram(emulator, file);
        highScore = cartridgeHighScore(ram);
      });
    }
  } catch {
    // Storage can be unavailable or contain a corrupt save.
  }
  onHighScore(highScore);

  function saveCartridge() {
    try {
      withCartridgeRam((file, ram) => {
        core._emulator_write_ext_ram(emulator, file);
        const score = cartridgeHighScore(ram);
        if (score !== highScore) {
          highScore = score;
          onHighScore(score);
        }
        localStorage.setItem(SAVE_KEY, btoa(String.fromCharCode(...ram)));
      });
    } catch {
      // The game remains playable in browsers that block storage.
    }
  }

  function playAudio() {
    if (audio?.state !== 'running') return;

    const buffer = audio.createBuffer(2, AUDIO_FRAMES, sampleRate);
    for (let channel = 0; channel < 2; channel++) {
      const samples = buffer.getChannelData(channel);
      for (let index = 0; index < AUDIO_FRAMES; index++) {
        samples[index] = core.HEAPU8[audioPointer + index * 2 + channel] / 255;
      }
    }
    const source = audio.createBufferSource();
    source.buffer = buffer;
    source.connect(output);
    if (nextAudioTime < audio.currentTime) nextAudioTime = audio.currentTime + 0.06;
    source.start(nextAudioTime);
    nextAudioTime += buffer.duration;
  }

  function update(time) {
    const elapsed = Math.min(time - (previousTime ?? time), 80);
    previousTime = time;
    targetTicks += elapsed * TICKS_PER_SECOND / 1000;
    let drawFrame = false;
    let event;
    do {
      event = core._emulator_run_until_f64(emulator, targetTicks);
      if (event & FRAME_READY) drawFrame = true;
      if (event & AUDIO_READY) playAudio();
    } while (!(event & TARGET_REACHED));

    if (drawFrame) {
      frame.data.set(core.HEAPU8.subarray(framePointer, framePointer + frame.data.length));
      context.putImageData(frame, 0, 0);
    }
    if (core._emulator_was_ext_ram_updated(emulator)) saveCartridge();
    animation = requestAnimationFrame(update);
  }

  function setButton(button, source, pressed) {
    const sources = held.get(button);
    if (pressed) sources.add(source);
    else sources.delete(source);
    core[`_set_joyp_${button}`](emulator, sources.size > 0);
  }

  function releaseButtons() {
    for (const [button, sources] of held) {
      sources.clear();
      core[`_set_joyp_${button}`](emulator, false);
    }
  }

  animation = requestAnimationFrame(update);
  return {
    setButton,
    releaseButtons,
    mute(muted) {
      if (output) output.gain.value = muted ? 0 : 0.45;
    },
    dispose() {
      cancelAnimationFrame(animation);
      releaseButtons();
      output?.disconnect();
      core._emulator_delete(emulator);
      core._joypad_delete(joypad);
      core._free(romPointer);
    },
  };
}
