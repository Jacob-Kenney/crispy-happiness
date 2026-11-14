import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile, writeFile} from 'node:fs/promises';
import {CablePeer, StateParser, crc8, decodeState, inputMask} from '../chromatic/protocol.mjs';

test('state CRC, framing, loss recovery and input validation', () => {
  const p = new Uint8Array(64);
  p.set([67,82,1,3,255,255,1,0,80,100,7,2]);
  p[14] = 0x10; p[17] = 0x34; p[18] = 0x12;
  p[63] = crc8(p.subarray(0,63));
  const state = decodeState(p);
  assert.equal(state.sequence, 65535); assert.equal(state.score, 1234); assert.equal(state.lives, 10);
  const received = []; const parser = new StateParser(s => received.push(s));
  for (const byte of [0,67,82,...p.slice(0,40),...p,...p]) parser.push(byte);
  assert.equal(received.length, 2);
  p[8] ^= 1; assert.throws(() => decodeState(p));
  assert.equal(inputMask({buttons:['left','a']}), 18);
  assert.throws(() => inputMask({buttons:['invalid']}));
});

test('real ROM exchanges CRC-valid state and receives controls through two CWL2 cores', {
  skip: !process.env.CANYON_TEST_GBA || !process.env.CANYON_TEST_SGLINK,
}, async () => {
  async function instantiate(path) {
    const module = await WebAssembly.compile(await readFile(path));
    const imports = {};
    for (const i of WebAssembly.Module.imports(module)) {
      imports[i.module] ??= {}; imports[i.module][i.name] = () => 0;
    }
    const instance = await WebAssembly.instantiate(module, imports);
    instance.exports._initialize?.();
    return instance.exports;
  }
  const [g, master, slave] = await Promise.all([
    instantiate(process.env.CANYON_TEST_GBA), instantiate(process.env.CANYON_TEST_SGLINK), instantiate(process.env.CANYON_TEST_SGLINK),
  ]);
  const rom = await readFile(new URL('../canyon-racer/build/gb/canyon_racer_0.7.7_agent_mbc5.gb', import.meta.url));
  g.smgba_side_init();
  const machine = g.smgba_create(1, 256*224, 160, 48000);
  const r = g.malloc(rom.length); new Uint8Array(g.memory.buffer).set(rom, r);
  assert.equal(g.smgba_load_rom(machine, r, rom.length), 0); g.free(r);
  g.smgba_link_enable(machine, 1);
  const gp = g.malloc(4096), mp = master.malloc(4096);
  let now = 1; const states = [], outbound = [];
  const peer = new CablePeer(slave, p => outbound.push(p), s => states.push(s), () => now);
  master.sgl_cwl_reset(now, 1, 0);
  let remoteSeen = false;
  for (; now < 120000; now++) {
    // Exercise menus using the actual remote controls, then steer in gameplay.
    peer.mask = states.at(-1)?.playing && !states.at(-1)?.paused ? (states.some(s => s.playing && s.remoteKeys === 2) ? 0 : 2) : (now % 6000 < 3000 ? 128 : 0);
    for (let i = 0; i < 4; i++) {
      g.smgba_run_slice(machine, 1048);
      if (g.smgba_link_stalled(machine)) break;
    }
    for (let pass = 0; pass < 16; pass++) {
      const n = g.smgba_link_events(machine, gp, 32);
      const ev = new Uint8Array(g.memory.buffer).slice(gp, gp+n*2);
      for (let i = 0; i < n; i++) master.sgl_cwl_event(ev[i*2],ev[i*2+1],now);
      while (outbound.length) {
        const p = outbound.shift(); new Uint8Array(master.memory.buffer).set(p,mp); master.sgl_cwl_packet(mp,6,now);
      }
      master.sgl_cwl_poll(now);
      const commands = master.sgl_cwl_take_commands(mp,32);
      const cmd = new Uint8Array(master.memory.buffer).slice(mp,mp+commands*2);
      for (let i = 0; i < commands; i++) g.smgba_link_command(machine,cmd[i*2],cmd[i*2+1]);
      const count = master.sgl_cwl_take_packets(mp,32);
      const packets = new Uint8Array(master.memory.buffer).slice(mp,mp+count*6);
      for (let i = 0; i < count; i++) peer.packet(packets.subarray(i*6,i*6+6));
      peer.pump();
      if (!n && !commands && !count && !outbound.length) break;
    }
    remoteSeen ||= states.some(s => s.remoteKeys !== 0);
  }
  assert.ok(states.length > 10, `only ${states.length} states received`);
  assert.ok(remoteSeen, 'ROM did not accept remote controls');
  assert.ok(states.some(s => s.playing), 'remote controls did not enter gameplay');
  assert.ok(states.some(s => s.playing && s.remoteKeys === 2), "gameplay did not receive steering");
  assert.ok(states.at(-1).remoteKeys === 0, "buttons did not release");
  assert.ok(states.some(s => s.obstacles.length), 'no live obstacles observed');
  if (process.env.CANYON_TEST_FRAME) {
    const rgba = new Uint8Array(g.memory.buffer, g.smgba_video(machine), 160*144*4);
    const rgb = Buffer.alloc(160*144*3);
    for (let i = 0, j = 0; i < rgba.length; i += 4) {
      rgb[j++] = rgba[i]; rgb[j++] = rgba[i+1]; rgb[j++] = rgba[i+2];
    }
    await writeFile(process.env.CANYON_TEST_FRAME, Buffer.concat([Buffer.from('P6\n160 144\n255\n'),rgb]));
  }
  console.log(`CWL2/ROM: ${states.length} valid snapshots; real gameplay and obstacles observed`);
  peer.close(); master.free(mp); g.free(gp); g.smgba_destroy(machine);
});
