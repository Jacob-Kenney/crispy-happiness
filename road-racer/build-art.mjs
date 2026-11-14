import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const root = process.argv[2];
const rows = (pixels, x = 0, y = 0) => pixels.slice(y, y + 8).flatMap(line => {
  let low = 0, high = 0;
  for (const color of line.slice(x, x + 8)) {
    low = low * 2 + (Number(color) & 1);
    high = high * 2 + (Number(color) >> 1);
  }
  return [low, high];
});
const hex = values => values.map(n => '0x' + n.toString(16).padStart(2, '0')).join(',');
const roadTiles = [
  // Grass, verge, bushes and trees use the same green palette.
  ['11111111','11111111','11011111','11111111','11111111','11111101','11111111','11111111'],
  ['11111222','11111122','11011222','11111122','11111222','11111202','11111222','11111122'],
  ['11111111','11111111','11111111','11131311','11333311','11100311','11111111','11111111'],
  ['11111111','11103111','11030311','10303031','10333331','11333111','11111311','11111111'],
  // White edge lines, aggregate, and the two halves of the yellow center line.
  ['30222222','30222222','30222222','30222222','30222212','30222222','30222222','30222222'],
  ['22222222','22222222','22122222','22222222','22222222','22222222','22222212','22222222'],
  ['22222223','22222223','22222223','22222223','22222223','22222223','22222223','22222223'],
  ['32222222','32222222','32222222','32222222','32222222','32222222','32222222','32222222'],
  ['22222203','22222203','22122203','22222203','22222203','22222203','22222203','22222203'],
  ['22211111','22011111','22211111','22211111','22111111','22211101','22111111','22211111'],
];
const map = Array.from({length:32},(_,i) => {
  const dash = i % 4 < 2;
  return [0,i%7===2?3:0,0,i%5===2?2:0,0,i%6===0?3:0,i%7===1?2:0,1,
    4,dash?6:5,dash?7:5,8,9,i%6===3?3:0,0,i%5===1?2:0,0,i%7===5?3:0,0,0];
});
const attributes = map.slice(0,4).map((line, i) => line.map((_, x) =>
  x === 9 || x === 10 ? (i % 4 < 2 ? 3 : 2) : x === 8 || x === 11 ? 2 : 1));
writeFileSync(join(root,'res/map_canyon.c'), [
  '#include <gbdk/metasprites.h>', '#include "map_canyon.h"', 'BANKREF(map_canyon)',
  'const uint8_t map_canyon_tiles[] = {' + hex(roadTiles.flatMap(tile => rows(tile))) + '};',
  'const unsigned char map_canyon_map[640] = {\n  ' + map.join(',\n  ') + '\n};',
  'const uint8_t road_map_attributes[80] = {\n  ' + attributes.join(',\n  ') + '\n};',
].join('\n'));
const mapHeader = join(root, 'res/map_canyon.h');
writeFileSync(mapHeader, readFileSync(mapHeader, 'utf8')
  .replace('map_canyon_TILE_COUNT 48', 'map_canyon_TILE_COUNT ' + roadTiles.length)
  .replace('map_canyon_tiles[768]', 'map_canyon_tiles[]'));

const car = [
  '0002332000','0032222300','0122222210','1122222211',
  '1121111211','1211111121','1211111121','0222222220',
  '0232222320','0232222320','0232222320','1211111121',
  '1221111221','1332222331','0132222310','0011111100',
].map(line => '0000000' + line + '0000000');
const carTiles = [0, 8, 16].flatMap(x => [...rows(car, x), ...rows(car, x, 8)]);
const shipPath = join(root,'res/sprite_ship.c');
const ship = readFileSync(shipPath,'utf8').replace(/(sprite_ship_tiles\[1024\] = \{)([^}]+)}/, (_, header, body) => {
  const original = body.match(/0x[\da-f]{2}/gi).map(s => Number(s));
  original.splice(0, carTiles.length, ...carTiles);
  return header + '\n  ' + hex(original) + '\n}';
}).replace(/sprite_ship_metasprite0\[\] = \{[^}]+}/,
  'sprite_ship_metasprite0[] = { METASPR_ITEM(21, 8, 0, 1), METASPR_ITEM(0, 8, 2, 1), METASPR_ITEM(0, 8, 4, 1), METASPR_TERM }')
  .replace('sprite_ship_metasprite0, sprite_ship_metasprite1, sprite_ship_metasprite2,', 'sprite_ship_metasprite0, sprite_ship_metasprite0, sprite_ship_metasprite0,');
writeFileSync(shipPath,ship);

const hazards = [
  ['00000300','00003320','10003330','13333000','12333000','03330000','01010000','02020000'],
  ['00333000','00323000','00030000','02333200','00333100','00330100','00110100','00220100'],
  ['00033000','00033000','00233200','00233200','03333330','03222330','13333331','11111111'],
  ['00030000','00333000','00323000','00333000','00333000','01333100','00020000','00010000'],
  ['00030000','03030030','00333000','33333303','00333330','03333100','30030030','00000300'],
  ['00000300','03000000','00033003','00333300','03333100','00033300','30000020','00003000'],
  ['30000003','33000030','03300033','00300000','00010000','00003300','03000330','33000003'],
  ['00030030','30030300','03333000','00311133','33333100','00333000','03030300','30030030'],
];
const obstaclePath = join(root,'res/sprite_obstacles.c');
const hazardTiles = hazards.flatMap(sprite => [...rows(sprite), ...Array(16).fill(0)]);
const obstacle = readFileSync(obstaclePath,'utf8').replace(/sprite_obstacles_tiles\[224\] = \{[^}]+}/,
  'sprite_obstacles_tiles[' + hazardTiles.length + '] = {\n  ' + hex(hazardTiles) + '\n}');
writeFileSync(obstaclePath,obstacle);
const obstacleHeader = join(root,'res/sprite_obstacles.h');
writeFileSync(obstacleHeader, readFileSync(obstacleHeader,'utf8')
  .replace('sprite_obstacles_TILE_COUNT 14', 'sprite_obstacles_TILE_COUNT ' + hazardTiles.length / 16)
  .replace('sprite_obstacles_tiles[224]', 'sprite_obstacles_tiles[' + hazardTiles.length + ']'));

const letters = {
  R:['1110','1001','1001','1110','1010','1001','1001'],
  L:['1000','1000','1000','1000','1000','1000','1111'],
  A:['0110','1001','1001','1111','1001','1001','1001'],
  C:['0111','1000','1000','1000','1000','1000','0111'],
  E:['1111','1000','1000','1110','1000','1000','1111'],
  P:['1110','1001','1001','1110','1000','1000','1000'],
  S:['0111','1000','1000','0110','0001','0001','1110'],
  T:['1111','0110','0110','0110','0110','0110','0110'],
};
const chars = Object.keys(letters);
const font = chars.flatMap(char => rows([...letters[char].map(line => '00' + line.replaceAll('1','3') + '00'), '00000000']));
const textTiles = text => [...text].map(char => chars.indexOf(char) + 1);
const titleMap = textTiles('RL RACER');
const startMap = textTiles('PRESS START');
writeFileSync(join(root,'src/road_title.h'), [
  'static const uint8_t road_font[] = {' + hex([...Array(16).fill(0), ...font]) + '};',
  'static const uint8_t road_title[] = {' + hex(titleMap) + '};',
  'static const uint8_t road_start[] = {' + hex(startMap) + '};',
].join('\n'));
