"""
Generate a compact land-mask bitmap for the M5Paper world map.

Resolution: 180x90 (2° per cell). Output: firmware/src/landmap.h
Each row is 23 bytes (184 bits, 4 padding bits). Total: 2070 bytes PROGMEM.

Continents are defined as simplified polygons and rasterized via ray-casting.
Run once: python firmware/tools/gen_landmap.py
"""

import math
import os

W, H = 180, 90  # 2° per cell
ROW_BYTES = (W + 7) // 8  # 23 bytes per row

# Simplified continent polygons as (lon, lat) vertex lists.
# Coordinates are approximate — only need to be recognizable at 2° resolution.

AFRICA = [
    (-17, 15), (-17, 21), (-13, 28), (-10, 36), (-5, 36), (0, 37),
    (10, 37), (20, 37), (28, 33), (33, 32), (35, 30), (38, 20),
    (42, 12), (51, 12), (50, 8), (48, 5), (44, 0), (40, -3),
    (40, -10), (40, -15), (36, -20), (33, -27), (28, -33), (26, -34),
    (20, -35), (18, -35), (17, -29), (14, -24), (12, -17), (9, -5),
    (7, 0), (1, 5), (-4, 5), (-8, 4), (-12, 5),
]

SOUTH_AMERICA = [
    (-80, 9), (-77, 8), (-72, 12), (-67, 11), (-62, 10), (-57, 6),
    (-52, 3), (-50, 0), (-44, -2), (-41, -3), (-38, -5), (-35, -8),
    (-35, -12), (-37, -15), (-40, -22), (-43, -23), (-47, -26),
    (-49, -29), (-52, -33), (-57, -38), (-62, -39), (-65, -42),
    (-67, -46), (-68, -50), (-69, -52), (-74, -52), (-75, -48),
    (-74, -42), (-73, -37), (-72, -33), (-71, -18), (-75, -14),
    (-77, -12), (-80, -5), (-80, 0), (-78, 2), (-77, 4),
]

NORTH_AMERICA = [
    (-168, 66), (-162, 64), (-155, 60), (-148, 60), (-140, 60),
    (-137, 57), (-134, 55), (-130, 52), (-125, 49), (-124, 42),
    (-118, 34), (-115, 30), (-110, 24), (-105, 20), (-97, 16),
    (-92, 15), (-88, 15), (-84, 10), (-82, 9), (-78, 9),
    (-77, 8), (-75, 11), (-80, 18), (-80, 25), (-82, 30),
    (-84, 30), (-89, 30), (-90, 29), (-94, 29), (-97, 26),
    (-97, 28), (-94, 30), (-90, 30), (-82, 31), (-76, 35),
    (-70, 41), (-67, 45), (-60, 46), (-52, 47), (-55, 50),
    (-56, 52), (-60, 53), (-64, 58), (-62, 60), (-65, 61),
    (-72, 63), (-80, 64), (-85, 66), (-88, 68), (-95, 72),
    (-110, 72), (-125, 72), (-140, 70), (-155, 71), (-165, 68),
]

GREENLAND = [
    (-55, 60), (-46, 60), (-42, 62), (-38, 65), (-20, 70), (-18, 76),
    (-20, 82), (-30, 83), (-42, 83), (-52, 80), (-58, 76), (-55, 72),
    (-52, 68), (-50, 65),
]

EUROPE = [
    (-10, 36), (-10, 40), (-9, 43), (-3, 43), (-5, 48), (-2, 51),
    (0, 51), (2, 51), (5, 54), (8, 56), (5, 58), (5, 62), (10, 64),
    (15, 66), (18, 69), (25, 71), (32, 71), (40, 69), (42, 67),
    (44, 64), (42, 56), (40, 52), (40, 48), (40, 42), (35, 36),
    (30, 36), (28, 41), (26, 42), (24, 40), (26, 38), (24, 36),
    (22, 40), (20, 40), (15, 38), (14, 45), (7, 44), (3, 43),
    (0, 44), (-3, 44), (-5, 43), (-8, 44), (-10, 42),
]

ASIA = [
    (40, 69), (50, 70), (60, 68), (70, 72), (80, 73), (100, 73),
    (120, 73), (140, 72), (160, 67), (170, 65), (175, 62),
    (170, 60), (163, 58), (155, 55), (150, 50), (145, 48),
    (140, 48), (135, 45), (131, 42), (130, 35), (128, 34),
    (127, 30), (122, 30), (120, 24), (115, 22), (110, 20),
    (108, 16), (106, 10), (103, 2), (104, 1), (103, 2),
    (100, 7), (99, 13), (98, 16), (96, 17), (94, 20), (92, 22),
    (89, 22), (87, 25), (84, 28), (80, 28), (75, 25), (72, 25),
    (68, 24), (62, 25), (57, 26), (52, 25), (48, 30), (44, 33),
    (40, 37), (35, 36), (30, 36), (28, 41), (30, 42), (36, 42),
    (40, 42), (40, 48), (40, 52), (42, 56), (44, 64), (42, 67),
]

INDIA = [
    (68, 24), (72, 25), (75, 25), (80, 28), (84, 28), (87, 25),
    (89, 22), (92, 22), (91, 20), (88, 15), (85, 12), (80, 8),
    (77, 8), (76, 12), (74, 16), (72, 20),
]

ARABIA = [
    (35, 30), (38, 20), (42, 12), (44, 15), (48, 18), (52, 19),
    (55, 22), (56, 25), (52, 25), (48, 30), (44, 33), (40, 33),
]

UK = [
    (-6, 50), (-5, 52), (-3, 53), (-3, 55), (-5, 57), (-5, 59),
    (-3, 59), (-2, 57), (0, 53), (2, 52), (2, 51), (0, 51), (-3, 50),
]

ICELAND = [
    (-24, 64), (-22, 66), (-18, 66), (-14, 65), (-14, 64), (-18, 63),
    (-22, 63),
]

JAPAN = [
    (130, 31), (132, 33), (134, 34), (136, 35), (138, 36), (140, 38),
    (140, 40), (141, 42), (142, 44), (145, 44), (145, 42), (143, 40),
    (142, 38), (140, 36), (137, 33), (133, 30),
]

AUSTRALIA = [
    (114, -14), (117, -14), (121, -14), (129, -12), (133, -12),
    (136, -12), (139, -11), (142, -11), (145, -15), (149, -18),
    (150, -22), (153, -24), (153, -27), (151, -33), (150, -38),
    (148, -38), (144, -38), (140, -36), (137, -35), (134, -34),
    (132, -33), (130, -32), (126, -34), (122, -34), (118, -34),
    (115, -33), (114, -30), (113, -26), (114, -22), (114, -18),
]

INDONESIA = [
    (95, 5), (98, 4), (104, -1), (106, -6), (108, -7), (110, -7),
    (114, -8), (116, -8), (118, -6), (116, -4), (112, -4), (110, -2),
    (108, 0), (106, 2), (104, 4), (100, 5),
]

BORNEO = [
    (108, 4), (110, 3), (115, 4), (117, 4), (118, 2), (118, 0),
    (117, -2), (115, -4), (112, -4), (110, -2), (109, 0), (108, 2),
]

PAPUA = [
    (131, -2), (133, -3), (137, -4), (141, -5), (143, -5),
    (148, -6), (150, -7), (150, -9), (147, -10), (143, -8),
    (140, -6), (137, -5), (134, -4), (131, -3),
]

MADAGASCAR = [
    (44, -12), (46, -13), (48, -16), (50, -18), (50, -23), (47, -25),
    (44, -25), (44, -20), (43, -16),
]

NZ_NORTH = [
    (173, -35), (175, -37), (178, -38), (178, -41), (175, -41),
    (173, -39), (172, -37),
]

NZ_SOUTH = [
    (168, -44), (170, -44), (172, -42), (174, -42), (172, -44),
    (171, -46), (168, -46), (167, -45),
]

SRI_LANKA = [
    (80, 10), (81, 8), (82, 7), (81, 6), (80, 7), (79, 8),
]

PHILIPPINES = [
    (118, 7), (120, 10), (122, 14), (124, 18), (122, 18),
    (120, 14), (118, 10),
]

ALL_POLYGONS = [
    AFRICA, SOUTH_AMERICA, NORTH_AMERICA, GREENLAND,
    EUROPE, ASIA, INDIA, ARABIA, UK, ICELAND, JAPAN,
    AUSTRALIA, INDONESIA, BORNEO, PAPUA, MADAGASCAR,
    NZ_NORTH, NZ_SOUTH, SRI_LANKA, PHILIPPINES,
]


def point_in_polygon(px, py, poly):
    """Ray-casting point-in-polygon test."""
    n = len(poly)
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if ((yi > py) != (yj > py)) and (px < (xj - xi) * (py - yi) / (yj - yi) + xi):
            inside = not inside
        j = i
    return inside


def generate_bitmap():
    bitmap = bytearray(ROW_BYTES * H)
    for by in range(H):
        lat = 90 - (by * 2 + 1)  # center of cell, north to south
        for bx in range(W):
            lon = -180 + (bx * 2 + 1)  # center of cell, west to east
            land = any(point_in_polygon(lon, lat, p) for p in ALL_POLYGONS)
            if land:
                byte_idx = by * ROW_BYTES + bx // 8
                bit_idx = 7 - (bx % 8)
                bitmap[byte_idx] |= (1 << bit_idx)
    return bitmap


def bitmap_to_ascii(bitmap):
    lines = []
    for by in range(H):
        row = []
        for bx in range(W):
            byte_idx = by * ROW_BYTES + bx // 8
            bit_idx = 7 - (bx % 8)
            if bitmap[byte_idx] & (1 << bit_idx):
                row.append("#")
            else:
                row.append(".")
        lines.append("".join(row))
    return "\n".join(lines)


def bitmap_to_header(bitmap):
    lines = [
        "#pragma once",
        "// Auto-generated by gen_landmap.py — do not edit",
        "// 180x90 land mask (2° resolution), 23 bytes per row",
        "#include <pgmspace.h>",
        "",
        "#define LANDMAP_W 180",
        "#define LANDMAP_H 90",
        "#define LANDMAP_ROW_BYTES 23",
        "",
        "static const uint8_t LANDMAP[LANDMAP_H * LANDMAP_ROW_BYTES] PROGMEM = {",
    ]
    for by in range(H):
        row_bytes = []
        for i in range(ROW_BYTES):
            row_bytes.append(f"0x{bitmap[by * ROW_BYTES + i]:02x}")
        lines.append("    " + ", ".join(row_bytes) + ",")
    lines.append("};")
    return "\n".join(lines)


if __name__ == "__main__":
    print("Generating land bitmap...")
    bitmap = generate_bitmap()

    # Preview
    ascii_map = bitmap_to_ascii(bitmap)
    print(ascii_map)

    # Count land cells
    land_count = sum(
        1 for by in range(H) for bx in range(W)
        if bitmap[by * ROW_BYTES + bx // 8] & (1 << (7 - bx % 8))
    )
    print(f"\nLand cells: {land_count} / {W * H} ({100 * land_count / (W * H):.1f}%)")

    # Write header
    out_path = os.path.join(os.path.dirname(__file__), "..", "src", "landmap.h")
    header = bitmap_to_header(bitmap)
    with open(out_path, "w") as f:
        f.write(header + "\n")
    print(f"Wrote {out_path} ({len(bitmap)} bytes of bitmap data)")
