#ifndef LAST_TRAIN_PROTOCOL_H
#define LAST_TRAIN_PROTOCOL_H

#define TRAIN_OP 0x31
#define TRAIN_PORT 47822
#define TRAIN_VERSION 1
#define TRAIN_WIDTH 20
#define TRAIN_HEIGHT 12
#define TRAIN_TITLE_SIZE 16
#define TRAIN_TEXT_SIZE 20

#define TRAIN_WALL 0
#define TRAIN_STREET 1
#define TRAIN_PLATFORM 2
#define TRAIN_DOOR 3
#define TRAIN_ITEM_FIRST 4
#define TRAIN_ITEM_LAST 9
#define TRAIN_WATER 10
#define TRAIN_TREE 11
#define TRAIN_HOUSE 12
#define TRAIN_MAX_TILE TRAIN_HOUSE
#define TRAIN_EXIT_X 10
#define TRAIN_EXIT_Y 10
#define TRAIN_HARD_FLAG 0x80

/* UDP request: LT, version, sequence (u16 LE), run ID (u32 LE), stop, item.
 * The high bit of the item byte requests the harder map layout.
 * Reply: TL, version, sequence (u16 LE), stop, title, two captions, tile map.
 * Text is ASCII, space padded, with no terminating byte on the wire. */
#define TRAIN_REQUEST_SIZE 11
#define TRAIN_HEADER_SIZE 6
#define TRAIN_MAP_OFFSET (TRAIN_HEADER_SIZE + TRAIN_TITLE_SIZE + 2 * TRAIN_TEXT_SIZE)
#define TRAIN_REPLY_SIZE (TRAIN_MAP_OFFSET + TRAIN_WIDTH * TRAIN_HEIGHT)

#endif
