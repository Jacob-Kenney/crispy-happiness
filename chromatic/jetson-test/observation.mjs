const hitboxes = [[0, 8], [24, 32], [8, 24], [0, 32], [8, 24]];
const obstacleNames = ['left', 'right', 'middle', 'full', 'item'];

export function observation(state, packet, previousAction) {
  const {x, y, z, state: shipState, counter} = state.ship;
  const wall = row => state.canyon[Math.min(17, Math.max(0, Math.floor(row / 8)))].left;
  const exact = packet[57] === 1;
  const left = exact ? packet[58] : wall(y + 16);
  const front = exact ? [...packet.slice(59, 62)] : [wall(y - 8), wall(y - 24), wall(y - 40)];
  const obstacles = state.obstacles
    .filter(item => !(item.type & 128) && item.y <= y + 22 && (item.type & 7) < hitboxes.length)
    .sort((a, b) => b.y - a.y)
    .slice(0, 3)
    .map(item => {
      const type = item.type & 7;
      const [offsetLeft, offsetRight] = hitboxes[type];
      const edgeLeft = item.left + offsetLeft;
      const edgeRight = item.left + offsetRight;
      return 'dy' + (y - item.y) + ' ' + obstacleNames[type] + ' ' + edgeLeft + '..' + edgeRight +
        ' dl' + (edgeLeft - x) + ' dr' + (edgeRight - x);
    });
  const pose = (shipState & 32) ? 'crash' : ((shipState & 1) ? 'jump' : 'ground');
  const invincible = Number(counter > 0);
  return [
    'x=' + x + ' y=' + y + ' z=' + z + ' ' + pose + ' inv=' + invincible +
      ' lv=' + state.level + ' sc=' + state.score + ' spd=' + Math.round(state.obstacleSpeed * 256),
    'canyon ' + left + '..' + (left + 32) + ' dx=' + (x - left - 16) +
      ' gl=' + (x + 8 - left) + ' gr=' + (left + 32 - x - 16),
    'front ' + front.map(edge => edge + '..' + (edge + 32)).join(' '),
    'obs ' + (obstacles.join('; ') || 'none'),
    'last=' + previousAction,
  ].join('\n');
}
