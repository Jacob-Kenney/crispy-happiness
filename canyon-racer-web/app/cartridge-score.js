// The MBC5 ROM saves a packed 18-byte settings record at the start of SRAM.
export const SAVE_KEY = 'canyon-racer.cartridge-ram';

export function savedHighScore() {
  try {
    return cartridgeHighScore(Uint8Array.from(atob(localStorage.getItem(SAVE_KEY) ?? ''), (byte) => byte.charCodeAt(0)));
  } catch {
    return 0;
  }
}

export function cartridgeHighScore(ram) {
  if (ram.length < 18 || ram[1] !== 0x0f || ram[2] !== 0xa5 || ram[3] !== 0x78 || ram[4] !== 0x1e) return 0;
  const checksum = ram.subarray(1, 18).reduce((sum, byte) => (sum + byte) & 0xff, 0);
  if (checksum !== ram[0]) return 0;

  let score = 0;
  for (let index = 8; index >= 5; index--) {
    const high = ram[index] >> 4;
    const low = ram[index] & 15;
    if (high > 9 || low > 9) return 0;
    score = score * 100 + high * 10 + low;
  }
  return score;
}
