// Generate tests/fixtures/sleeve-indexed.png -- a colour-type-3 (PAL8) PNG with
// the SAME two halves as sleeve.png, so the two fixtures differ in exactly one
// property: how the pixels are stored.
//
// Written rather than exported from an image editor because the fixture has to
// be reproducible from something a reader can check, and because nothing in this
// toolchain writes indexed PNG on demand.

const fs = require('fs');
const zlib = require('zlib');

const W = 16, H = 16;
const LEFT = [220, 30, 30];
const RIGHT = [30, 60, 200];

// CRC32, table-free -- this runs once and clarity beats speed.
function crc32(buf) {
    let c = ~0;
    for (const b of buf) {
        c ^= b;
        for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return (~c) >>> 0;
}

function chunk(type, data) {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
}

// IHDR: 8-bit, colour type 3 = indexed
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0);
ihdr.writeUInt32BE(H, 4);
ihdr[8] = 8;   // bit depth
ihdr[9] = 3;   // colour type: INDEXED, which is the whole point of this fixture
ihdr[10] = 0;  // compression
ihdr[11] = 0;  // filter
ihdr[12] = 0;  // interlace

// PLTE: entry 0 is the left colour, entry 1 the right.
//
// DELIBERATELY NOT IN INDEX ORDER OF BRIGHTNESS or anything else a decoder might
// coincidentally get right -- the two entries are simply different enough that a
// channel swap or an off-by-one index is unmissable.
const plte = Buffer.from([...LEFT, ...RIGHT]);

// Scanlines: filter byte 0, then one index per pixel.
const raw = Buffer.alloc(H * (1 + W));
for (let y = 0; y < H; y++) {
    const row = y * (1 + W);
    raw[row] = 0;  // filter: none
    for (let x = 0; x < W; x++) raw[row + 1 + x] = x < W / 2 ? 0 : 1;
}

const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr),
    chunk('PLTE', plte),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
]);

const out = process.argv[2];
fs.writeFileSync(out, png);
console.log(`${out}: ${png.length} bytes, ${W}x${H}, colour type 3 (indexed)`);
