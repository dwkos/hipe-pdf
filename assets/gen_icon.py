import os
import math
from PIL import Image, ImageDraw, ImageFilter

# Regenerates assets/icon.png (the app icon sent via HIPE_OP_SET_ICON -- see
# src/icon_data.hpp and its generation note). Run from anywhere; output always
# lands next to this script.
OUT = os.path.dirname(os.path.abspath(__file__))
SS = 1024      # supersample canvas (content coordinate space, unchanged from before)
PAD = 140      # extra margin added on all sides of the working canvas, so the
               # page's own -8 degree rotation and the blurred drop shadow have
               # room to extend without being clipped by the canvas edge
CS = SS + 2*PAD  # actual working canvas size
FINAL = 128

PAGE = (250, 250, 252, 255)
PAGE_EDGE = (55, 55, 65, 255)
FOLD = (206, 212, 224, 255)
SPARK = (255, 196, 64, 255)
RED = (207, 38, 44, 255)
RED_DK = (160, 24, 30, 255)
RED_HI = (232, 92, 90, 255)

def draw_page_shape(d, x0, y0, x1, y1, fold, fill=PAGE, fold_fill=FOLD, edge=PAGE_EDGE):
    d.polygon([(x0,y0),(x1-fold,y0),(x1,y0+fold),(x1,y1),(x0,y1)],
              fill=fill, outline=edge, width=12)
    d.polygon([(x1-fold,y0),(x1,y0+fold),(x1-fold,y0+fold)],
              fill=fold_fill, outline=edge, width=12)

m = 80
x0, y0, x1, y1 = m+PAD, m*0.6+PAD, SS-m+PAD, SS-m*0.6+PAD
fold = 180

# --- shadow (page silhouette, rotated, blurred, offset) ---
shadow = Image.new("RGBA", (CS, CS), (0,0,0,0))
ds = ImageDraw.Draw(shadow)
draw_page_shape(ds, x0, y0, x1, y1, fold)
shadow = shadow.rotate(-8, resample=Image.BICUBIC, expand=False, fillcolor=(0,0,0,0))
alpha = shadow.split()[3]
solid = Image.new("RGBA", shadow.size, (20,24,34,255))
solid.putalpha(alpha)
solid = solid.filter(ImageFilter.GaussianBlur(26))

# --- page layer (page shape + red swoosh drawn in the SAME pre-rotation space,
#     so the swoosh tilts together with the page rather than looking pasted on) ---
page = Image.new("RGBA", (CS, CS), (0,0,0,0))
dp = ImageDraw.Draw(page)
draw_page_shape(dp, x0, y0, x1, y1, fold)

# swoosh: a tapered lens/ribbon band, built by rotating a thin ellipse, then
# clipped to the page's own silhouette so it reads as printed ON the page.
def bezier_point_tangent(p0, p1, p2, p3, t):
    mt = 1 - t
    x = mt**3*p0[0] + 3*mt**2*t*p1[0] + 3*mt*t**2*p2[0] + t**3*p3[0]
    y = mt**3*p0[1] + 3*mt**2*t*p1[1] + 3*mt*t**2*p2[1] + t**3*p3[1]
    dx = 3*mt**2*(p1[0]-p0[0]) + 6*mt*t*(p2[0]-p1[0]) + 3*t**2*(p3[0]-p2[0])
    dy = 3*mt**2*(p1[1]-p0[1]) + 6*mt*t*(p2[1]-p1[1]) + 3*t**2*(p3[1]-p2[1])
    return (x, y), (dx, dy)

def ribbon_polygon(p0, p1, p2, p3, half_width, n=160):
    """Builds a single closed polygon tracing a constant-width band along a cubic
    bezier, via each sample's normal offset -- avoids the seam/fringe artifacts of
    stroking many short overlapping line segments."""
    left, right = [], []
    for i in range(n + 1):
        t = i / n
        (x, y), (dx, dy) = bezier_point_tangent(p0, p1, p2, p3, t)
        length = max((dx*dx + dy*dy) ** 0.5, 1e-6)
        nx, ny = -dy / length, dx / length
        left.append((x + nx*half_width, y + ny*half_width))
        right.append((x - nx*half_width, y - ny*half_width))
    return left + right[::-1]

swoosh_layer = Image.new("RGBA", (CS, CS), (0,0,0,0))
ds2 = ImageDraw.Draw(swoosh_layer)
# a cropped-looking fragment of a bezier swash -- deliberately extends past the
# page's own edges so what remains after clipping reads as a "slice" of a larger
# curve, evoking Adobe's ribbon-like corporate mark without reproducing it.
def one_sided_arc_controls(p0, p3, bulge):
    """Control points for a simple single-direction arc (no S-inflection) between
    p0 and p3: both offset to the same side of the p0->p3 chord."""
    dx, dy = p3[0]-p0[0], p3[1]-p0[1]
    length = max((dx*dx+dy*dy)**0.5, 1e-6)
    nx, ny = -dy/length, dx/length
    def chord(t): return (p0[0]+dx*t, p0[1]+dy*t)
    c1 = chord(1/3); c2 = chord(2/3)
    p1 = (c1[0]+nx*bulge, c1[1]+ny*bulge)
    p2 = (c2[0]+nx*bulge, c2[1]+ny*bulge)
    return p1, p2

def mirror_y(points, axis_y):
    """Flips a polygon vertically about a horizontal axis, keeping x (and thus
    horizontal placement/footprint) unchanged -- used to invert the curve's bend
    direction without shifting where it crosses the page."""
    return [(x, 2*axis_y - y) for (x, y) in points]

def translate_y(points, dy):
    return [(x, y + dy) for (x, y) in points]

def rotate_points(points, angle_deg, center):
    a = math.radians(angle_deg)
    ca, sa = math.cos(a), math.sin(a)
    cx, cy = center
    out = []
    for x, y in points:
        dx, dy = x - cx, y - cy
        # PIL's rotate(angle) turns counter-clockwise (as viewed normally) for
        # positive angle despite image y pointing down -- negate sin to match.
        out.append((cx + dx*ca + dy*sa, cy - dx*sa + dy*ca))
    return out

DOWN_SHIFT = 430  # pushes the (now hill-shaped) band back down to ~2/3 down the
                   # page -- mirroring alone moved it up near the top, which reads
                   # as if nothing continues below (giving away the "cropped
                   # fragment" illusion); low placement implies the rest of the
                   # (unreproduced) mark continues off the bottom edge instead.

EXTRA_ROTATE = 24  # extra CCW nudge to the swoosh's own angle, independent of
                     # the page's -8 degree tilt applied later
EXTRA_REACH = 260   # extends both endpoints further off-canvas first, so the
                     # extra rotation doesn't pull the visible ends back from
                     # the page edges -- they should still read as "cropped" by
                     # the page boundary, not floating with a gap.
ROT_CENTER = (SS/2+PAD, SS/2+PAD)

def build_swoosh_poly(p0, p3, bulge, half_width):
    p0 = (p0[0]-EXTRA_REACH+PAD, p0[1]+PAD)
    p3 = (p3[0]+EXTRA_REACH+PAD, p3[1]+PAD)
    p1, p2 = one_sided_arc_controls(p0, p3, bulge)
    poly = ribbon_polygon(p0, p1, p2, p3, half_width)
    poly = translate_y(mirror_y(poly, SS/2+PAD), DOWN_SHIFT)
    poly = rotate_points(poly, EXTRA_ROTATE, ROT_CENTER)
    return poly

ds2.polygon(build_swoosh_poly((-150, 620), (1180, 300), 550, 85), fill=RED)
ds2.polygon(build_swoosh_poly((-150, 560), (1180, 210), 550, 23), fill=RED_HI)

# clip swoosh to the page silhouette so it never spills past the page edges
page_mask = Image.new("L", (CS, CS), 0)
dm = ImageDraw.Draw(page_mask)
draw_page_shape(dm, x0, y0, x1, y1, fold, fill=255, fold_fill=255, edge=255)
swoosh_clipped = Image.new("RGBA", (CS, CS), (0,0,0,0))
swoosh_clipped.paste(swoosh_layer, (0,0), Image.composite(swoosh_layer.split()[3], Image.new("L",(CS,CS),0), page_mask))

page.alpha_composite(swoosh_clipped)
# re-stroke the page outline on top so the swoosh never visually overlaps the border
dp2 = ImageDraw.Draw(page)
draw_page_shape_outline_only = None
dp2.polygon([(x0,y0),(x1-fold,y0),(x1,y0+fold),(x1,y1),(x0,y1)], outline=PAGE_EDGE, width=12)
dp2.polygon([(x1-fold,y0),(x1,y0+fold),(x1-fold,y0+fold)], outline=PAGE_EDGE, width=12)

page = page.rotate(-8, resample=Image.BICUBIC, expand=False, fillcolor=(0,0,0,0))

# --- compose ---
canvas = Image.new("RGBA", (CS, CS), (0,0,0,0))
canvas.alpha_composite(solid, (18, 26))
canvas.alpha_composite(page, (0, 0))

# --- sparkle near the folded corner, drawn crisp after rotation ---
dcanvas = ImageDraw.Draw(canvas)
sx, sy, sl = SS-238+PAD, 190+PAD, 92
w = 26
dcanvas.line([(sx-sl,sy),(sx+sl,sy)], fill=SPARK, width=w)
dcanvas.line([(sx,sy-sl),(sx,sy+sl)], fill=SPARK, width=w)
d2 = int(w*0.9)
dcanvas.ellipse([sx-d2,sy-d2,sx+d2,sy+d2], fill=SPARK)

final = canvas.resize((FINAL, FINAL), Image.LANCZOS)
final.save(f"{OUT}/icon.png")
print("done", final.size)
