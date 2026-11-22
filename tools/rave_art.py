"""Draw Mystik's two native-size printing plates. Run with Pillow and aggdraw to regenerate the embedded art."""
from math import exp, sin, sqrt
from pathlib import Path

import aggdraw
from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
S = 4
BG = (7, 7, 14)
ACID = (201, 255, 20)


def new_image(size):
    return Image.new("RGB", (size[0] * S, size[1] * S), BG)


def mask(size, contours):
    image = Image.new("L", size)
    draw = aggdraw.Draw(image)
    for contour in contours:
        path = aggdraw.Path()
        path.moveto(*(v * S for v in contour[0]))
        for point in contour[1:]:
            (path.lineto if len(point) == 2 else path.curveto)(*(v * S for v in point))
        path.close()
        draw.path(path, aggdraw.Brush(255))
    draw.flush()
    return image


def gradient(stops, t):
    for (lo, a), (hi, b) in zip(stops, stops[1:]):
        if t <= hi:
            u = max(0, (t - lo) / (hi - lo))
            return tuple(round(x + u * (y - x)) for x, y in zip(a, b))
    return stops[-1][1]


CHROME = [(0, (32, 25, 68)), (.14, (140, 133, 195)), (.31, (248, 252, 255)),
          (.44, (155, 173, 214)), (.485, (231, 249, 255)), (.505, (26, 24, 46)),
          (.64, (35, 24, 64)), (.81, (155, 117, 210)), (.93, (220, 242, 255)), (1, (70, 76, 122))]


def plate(image, shape, top, bottom):
    gradient_fill = Image.new("RGB", image.size)
    draw = ImageDraw.Draw(gradient_fill)
    for y in range(image.height):
        draw.line((0, y, image.width, y), fill=gradient(CHROME, (y / S - top) / (bottom - top)))
    image.paste(gradient_fill, mask=shape)
    upper = ImageChops.subtract(shape, ImageChops.offset(shape, S, S))
    image.paste((233, 246, 255), mask=upper.point(lambda v: v * .77))


def wordmark():
    image = new_image((132, 25))
    outlines = [
        [(1,22),(8,8),(3,6),(16,3),(20,12),(29,4),(36,4),(26,21),(19,21),(25,11),(18,17),(14,11),(8,21)],
        [(33,4),(40,4),(42,11),(51,4),(59,4),(45,16),(41,22),(33,22),(38,14)],
        [(75,4),(71,9),(58,9),(56,11),(70,11),(68,15),(60,21),(43,21),(48,16),(60,16),(62,14),(49,14),(50,10),(58,4)],
        [(75,4),(99,4),(94,9),(87,9),(80,21),(72,21),(79,9),(71,9)],
        [(98,8),(106,4),(96,21),(88,21)],
        [(108,4),(115,4),(110,12),(122,4),(132,2),(116,14),(127,22),(110,20),(105,16),(102,21),(97,21)]]
    lettering = mask(image.size, outlines)
    image.paste((238, 35, 139), mask=ImageChops.offset(lettering, -S, S))
    plate(image, lettering, 2, 22)
    return image


def halo(image):
    # Tapered orbital blade. Split the two edges at the ridge for a hard chrome reflection.
    wing = [[(14,91),(38,76,46,47,53,39),(51,54,46,68,39,77),
             (55,73,61,61,66,55),(62,73,45,86,14,91)]]
    lower = [[(14,91),(38,82,48,73,59,65),(49,79,39,87,14,91)]]
    for flip in (False, True):
        def reflect(paths):
            return [[tuple((160 - n if j % 2 == 0 else 132 - n) if flip else n
                           for j, n in enumerate(point)) for point in path] for path in paths]
        plate(image, mask(image.size, reflect(wing)), 37, 92)
        image.paste((128, 119, 200), mask=mask(image.size, reflect(lower)))


def faceplate():
    image = new_image((160, 126))
    pix = image.load()
    cx, cy, radius = 79.5, 65, 34
    for j in range(image.height):
        y = j / S
        for i in range(image.width):
            x = i / S
            # A warped checker floor disappears below the beat ring.
            if y > 89:
                z = (y - 89) / 37
                u = (x - 80) / (6 + 18 * z) + .60 * sin(z * 5)
                v = 25 / (z + .38)
                if (int(u + 100) + int(v)) % 2:
                    lit = z ** 1.3 * (.4 + .6 * abs(x - 80) / 80)
                    pix[i, j] = (int(11 + 70 * lit), int(10 + 38 * lit), int(25 + 95 * lit))
    halo(image)
    # Environment reflected by a polished sphere. A dark horizon separates the two softboxes.
    for j in range(int((cy-radius)*S), int((cy+radius)*S)+1):
        for i in range(int((cx-radius)*S), int((cx+radius)*S)+1):
            nx, ny = (i / S - cx) / radius, (j / S - cy) / radius
            rr = nx * nx + ny * ny
            if rr >= 1:
                continue
            nz = sqrt(1 - rr)
            rx, ry, rz = 2*nz*nx, 2*nz*ny, 2*nz*nz-1
            horizon = .5 + .49 * (ry + .20 * rx + .12 * rz)
            color = gradient([(0,(42,36,64)),(.10,(236,246,255)),(.19,(137,158,191)),
                              (.35,(48,54,79)),(.48,(20,20,35)),(.51,(85,74,119)),
                              (.53,(15,13,27)),(.68,(11,9,21)),(.79,(54,30,66)),
                              (.87,(180,83,161)),(.94,(78,70,133)),(1,(210,223,248))], horizon)
            rim = max(0,1 - nz * 2.9) ** 2
            strip = .95 * exp(-((rx+.70)/.18)**2 - ((ry+.52)/.3)**2)
            pix[i, j] = tuple(min(255, round(c + (242-c)*strip + rim*(178-c))) for c in color)
    # Enamel inlays. The player scales their luminance from the actual vactrol light.
    inlay = [
        [(66,45),(69,43,72,49,72,53),(74,60,72,65,70,64),(67,63,65,51,66,45)],
        [(83,43),(86,40,90,48,90,54),(91,60,89,62,87,61),(84,59,82,48,83,43)],
        [(53,65),(58,68,63,67,65,63),(65,66,63,69,61,70),(70,86,91,93,102,62),
         (99,62,96,60,96,56),(99,60,104,59,106,55),(106,59,105,62,104,63),
         (101,86,89,98,74,89),(66,85,61,78,58,71),(55,71,53,69,53,65)]]
    shape = mask(image.size, inlay)
    image.paste((88, 130, 12), mask=ImageChops.offset(shape, 0, S))
    image.paste(ACID, mask=shape)
    # Pinpoint cross reflections on the hard edges.
    for x, y, radius in [(57,42,5.5),(107,82,3.5),(143,36,2.5),(27,88,1.8)]:
        image.paste((232,243,255), mask=mask(image.size, [[(x-radius,y),(x-.7,y-.7),(x,y-radius),
                    (x+.7,y-.7),(x+radius,y),(x+.7,y+.7),(x,y+radius),(x-.7,y+.7)]]))
    return image


def emit(out, name, source):
    image = source.resize((source.width//S, source.height//S), Image.Resampling.LANCZOS)
    indexed = image.quantize(colors=160, dither=Image.Dither.NONE)
    palette = indexed.getpalette()
    colors = [0xff000000 | r << 16 | g << 8 | b for r,g,b in zip(palette[::3], palette[1::3], palette[2::3])]
    out.append(f"inline constexpr uint32_t {name}Palette[] = {{")
    for i in range(0, len(colors), 8):
        out.append("    " + ", ".join(f"0x{c:08x}" for c in colors[i:i+8]) + ",")
    out.append("};")
    out.append(f"inline constexpr uint8_t {name}[{image.height}][{image.width}] = {{")
    pixels = indexed.tobytes()
    for y in range(image.height):
        row = pixels[y*image.width:(y+1)*image.width]
        out.append("    {" + ",".join(str(v) for v in row) + "},")
    out.append("};")
    if name == "medallion":
        # Precompute the spans touched by the face lighting. The rest stays full chrome.
        green_inks = {i for i, color in enumerate(colors) if ((color >> 8) & 255) - (color & 255) > 12}
        out.append("inline constexpr struct { uint8_t y, left, end; } enamel[] = {")
        for y in range(image.height):
            row = pixels[y * image.width:(y + 1) * image.width]
            start = None
            for x in range(image.width + 1):
                painted = x < image.width and row[x] in green_inks
                if painted and start is None:
                    start = x
                if not painted and start is not None:
                    out.append(f"    {{{y}, {start}, {x}}},")
                    start = None
        out.append("};")


if __name__ == "__main__":
    out = ["// Generated by tools/rave_art.py. Original artwork at the screen's native scale.",
           "#pragma once", "#include <cstdint>", "namespace hdui::art {"]
    emit(out, "logo", wordmark())
    emit(out, "medallion", faceplate())
    out.append("}")
    (ROOT / "src/ui/rave_art.h").write_text("\n".join(out) + "\n")
