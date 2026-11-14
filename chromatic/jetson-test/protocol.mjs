export const CORE_URL = 'https://shinyrelay.com/play/sglink.0107e9a3.wasm';
export const CORE_SHA256 = '0107e9a36723e0b0aa86b39f12a0c060192fc20541fa8487ef1cbfbfe2e4ca26';
export const BUTTONS = {right: 1, left: 2, up: 4, down: 8, a: 16, b: 32, select: 64, start: 128};

export function crc8(bytes) {
  let crc = 0;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = ((crc << 1) ^ (crc & 128 ? 7 : 0)) & 255;
  }
  return crc;
}
const bcd = value => (value >> 4) * 10 + (value & 15);
export function decodeState(p) {
  if (p.length !== 64 || p[0] !== 67 || p[1] !== 82 || p[2] !== 1 || crc8(p.subarray(0, 63)) !== p[63])
    throw new Error('invalid Canyon state packet');
  if (p[21] > 5) throw new Error('invalid obstacle count');
  const u16 = i => p[i] | p[i + 1] << 8;
  return {
    sequence: u16(4), frame: u16(6), initialized: !!(p[3] & 1), playing: !!(p[3] & 2), paused: !!(p[3] & 4),
    ship: {x: p[8], y: p[9], z: p[10], state: p[11], counter: p[12]},
    level: p[13], lives: bcd(p[14]), localKeys: p[15], remoteKeys: p[16],
    score: bcd(p[17]) + bcd(p[18]) * 100 + bcd(p[19]) * 10000 + bcd(p[20]) * 1000000,
    obstacles: Array.from({length: p[21]}, (_, i) => ({y: p[22+i*3], type: p[23+i*3], left: p[24+i*3]})),
    canyon: Array.from({length: 18}, (_, i) => ({y: i*8, left: p[37+i], right: p[37+i]+32})),
    obstacleSpeed: u16(55) / 256,
  };
}

// Sliding parser recovers after a lost byte; maximum buffer is 64 bytes.
export class StateParser {
  constructor(onState) { this.buffer = []; this.onState = onState; }
  push(byte) {
    this.buffer.push(byte);
    while (this.buffer.length >= 64) {
      const p = Uint8Array.from(this.buffer.slice(0, 64));
      let state;
      try { state = decodeState(p); } catch { this.buffer.shift(); continue; }
      this.buffer.splice(0, 64);
      this.onState(state, p);
    }
  }
}

export function inputMask(command) {
  if (!command || !Array.isArray(command.buttons)) throw new Error('expected {buttons: [...]}');
  return command.buttons.reduce((mask, button) => {
    if (!Object.hasOwn(BUTTONS, button)) throw new Error('unknown button');
    return mask | BUTTONS[button];
  }, 0);
}

// Use Shiny's published CWL2 implementation instead of guessing its wire format.
export class CablePeer {
  constructor(core, send, onState, now = () => performance.now() >>> 0) {
    this.core = core; this.send = send; this.now = now;
    this.ptr = core.malloc(4096); this.parser = new StateParser(onState);
    this.mask = 0; this.control = [0xA5, 0, 0x5A]; this.controlIndex = 0;
    core.sgl_cwl_reset(now(), 1, 0);
    core.sgl_cwl_event(1, 0, now()); // port connected
    this.arm();
  }
  arm() {
    if (this.controlIndex === 0) this.control = [0xA5, this.mask, this.mask ^ 0x5A];
    const byte = this.control[this.controlIndex];
    this.controlIndex = (this.controlIndex + 1) % 3;
    this.core.sgl_cwl_event(4, byte, this.now()); // external-clock slave armed
  }
  packet(bytes) {
    if (bytes.length !== 6) return;
    new Uint8Array(this.core.memory.buffer).set(bytes, this.ptr);
    this.core.sgl_cwl_packet(this.ptr, bytes.length, this.now());
    this.pump();
  }
  pump() {
    const c = this.core;
    c.sgl_cwl_poll(this.now());
    for (let pass = 0; pass < 64; pass++) {
      const n = c.sgl_cwl_take_commands(this.ptr, 32);
      const commands = new Uint8Array(c.memory.buffer).slice(this.ptr, this.ptr + 2*n);
      for (let i = 0; i < n; i++) {
        if (commands[i*2] === 2) { this.parser.push(commands[i*2+1]); this.arm(); }
      }
      const count = c.sgl_cwl_take_packets(this.ptr, 32);
      const packets = new Uint8Array(c.memory.buffer).slice(this.ptr, this.ptr + 6*count);
      for (let i = 0; i < count; i++) this.send(packets.slice(i*6, i*6+6));
      if (!n && !count) break;
    }
  }
  close() { this.core.free(this.ptr); }
}
