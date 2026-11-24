# The vr buttstocks (vr_gun.cpp): the M16's carbine stock (a 6-position collapsible stock on its buffer tube) and
# the M249's fixed stock, lofted from cross-sections measured off the user's photos, real size, in each gun's mesh
# (metres; z to the muzzle, y up, x across). Writes vr_stocks.h (the build makes it: CMakeLists.txt, --out into its
# binary folder; it's too big for git); with a folder argument also side and
# three-quarter preview renders (shaded, no texture) to check the shapes by.
#   python tools/stocks.py [preview_dir]
import math, os, struct, sys, zlib

# ---- meshes: positions, smooth normals, triangles ------------------------------------------------------------
class Mesh:
    def __init__(self):
        self.p, self.n, self.t = [], [], []

    def add(self, p, n):
        self.p.append(p)
        self.n.append(n)
        return len(self.p) - 1

def norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / l for c in v)

def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return sum(x * y for x, y in zip(a, b))

def loft(m, rings, cap_front=True, cap_back=True):
    """rings: list of point lists (same count), each at one z, front first. Smooth normals across the grid."""
    S, N = len(rings), len(rings[0])
    idx = [[0] * N for _ in range(S)]
    for s in range(S):
        ring = rings[s]
        cx = sum(q[0] for q in ring) / N
        cy = sum(q[1] for q in ring) / N
        for k in range(N):
            a, b = ring[(k - 1) % N], ring[(k + 1) % N]
            tk = sub(b, a)
            f, g = rings[max(s - 1, 0)][k], rings[min(s + 1, S - 1)][k]
            tz = sub(g, f)
            nrm = norm(cross(tk, tz))
            out = (ring[k][0] - cx, ring[k][1] - cy, 0)
            if dot(nrm, out) < 0: nrm = tuple(-c for c in nrm)
            idx[s][k] = m.add(ring[k], nrm)
    for s in range(S - 1):
        for k in range(N):
            a, b, c, d = idx[s][k], idx[s][(k + 1) % N], idx[s + 1][(k + 1) % N], idx[s + 1][k]
            m.t += [(a, b, c), (a, c, d)]
    for s, on, sign in ((0, cap_front, 1), (S - 1, cap_back, -1)):
        if not on: continue
        ring = rings[s]
        z = sum(q[2] for q in ring) / N
        c = m.add((sum(q[0] for q in ring) / N, sum(q[1] for q in ring) / N, z), (0, 0, sign))
        e = [m.add(q, (0, 0, sign)) for q in ring]
        for k in range(N): m.t.append((c, e[k], e[(k + 1) % N]))

def box(m, x0, x1, y0, y1, z0, z1):
    """an axis box, flat faces"""
    P = lambda i, j, k: ((x0, x1)[i], (y0, y1)[j], (z0, z1)[k])
    for axis in range(3):
        for side in (0, 1):
            n = [0, 0, 0]
            n[axis] = 1 if side else -1
            quad = []
            for u, v in ((0, 0), (1, 0), (1, 1), (0, 1)):
                ijk = [0, 0, 0]
                ijk[axis] = side
                o = [a for a in range(3) if a != axis]
                ijk[o[0]], ijk[o[1]] = u, v
                quad.append(m.add(P(*ijk), tuple(n)))
            m.t += [(quad[0], quad[1], quad[2]), (quad[0], quad[2], quad[3])]

def tube(m, y, r, z0, z1, sides=16):
    rings = [[(r * math.cos(2 * math.pi * k / sides), y + r * math.sin(2 * math.pi * k / sides), z) for k in range(sides)]
             for z in (z0, z1)]
    loft(m, rings)

def lerp_table(tab, x):
    """piecewise linear through (x, y) points, sorted by x"""
    if x <= tab[0][0]: return tab[0][1]
    for (x0, y0), (x1, y1) in zip(tab, tab[1:]):
        if x <= x1: return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return tab[-1][1]

def smooth_table(tab, x):
    """a Catmull-Rom curve through the points (x by linear search, then the curve's own parameter)"""
    for i in range(len(tab) - 1):
        if x <= tab[i + 1][0] or i == len(tab) - 2:
            x0, x1 = tab[i][0], tab[i + 1][0]
            t = min(max((x - x0) / (x1 - x0), 0.0), 1.0)
            p0 = tab[max(i - 1, 0)][1]; p1 = tab[i][1]; p2 = tab[i + 1][1]; p3 = tab[min(i + 2, len(tab) - 1)][1]
            return 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3)
    return tab[-1][1]

# ---- the M16: a carbine's 6-position stock ------------------------------------------------------------------
# its receiver ends at z -0.12, the bore at y 0.0715. The buffer tube (r 1.55 cm) on the bore's line from the
# receiver's end plate to inside the stock. The stock (the user's photos: an M4 stock): the tube's housing on top, a
# collar at its front, four grooves along each side of it; under it the release latch on its hinge at the front,
# then a thin web deepening back to the toe with the sling slot's rim on it under the housing; a butt pad with
# ridges across its face, a sling loop under the toe. 18 cm long, 4.4 cm wide at the butt, the butt 15 cm tall.
def sweep(m, path, section, closed=False, side=(1.0, 0.0, 0.0)):
    """a section (2D points: along side, along the path's normal) carried along a path of 3D points"""
    rings = []
    n = len(path)
    for i, p in enumerate(path):
        a = path[(i - 1) % n] if closed or i > 0 else p
        b = path[(i + 1) % n] if closed or i < n - 1 else p
        t = norm(sub(b, a))
        nn = norm(cross(t, side))
        sec = section(i / (n - 1)) if callable(section) else section
        rings.append([tuple(p[k] + side[k] * u + nn[k] * v for k in range(3)) for u, v in sec])
    if closed: rings.append(rings[0])
    loft(m, rings, cap_front=not closed, cap_back=not closed)

def rrect(hw, ht, r, n=4):
    """a rounded rectangle's outline: half width, half thickness, corner radius"""
    pts = []
    for cx, cy, a0 in ((hw - r, ht - r, 0), (-hw + r, ht - r, 90), (-hw + r, -ht + r, 180), (hw - r, -ht + r, 270)):
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

# traced off the user's side-on photo (204 x 192 px, the stock 188 px long = 17.9 cm): photo px to the gun's
# (z, y), the tube's middle (py 52.5) on the bore's y. Scaled to the gun (user, a photo of a whole carbine: against
# its receiver, the receiver back to the handguard 0.24 m in the mesh, the stock 21 cm long, 15 cm tall, its front
# 6 cm behind the receiver's end), K16 times the first size: 1.14 mm a px, the front (px 193) at z -0.178
K16 = 1.2
S16 = 0.00095 * K16
Z16 = -0.178
def p16(px, py): return (Z16 - (193 - px) * S16, 0.066 - (py - 52.5) * S16)

def clip(poly, axis, v, keep_less):
    """a polygon (2D points) cut by the line axis = v, the part below (keep_less) or above it kept"""
    out = []
    for i, a in enumerate(poly):
        b = poly[(i + 1) % len(poly)]
        ia, ib = (a[axis] <= v) == keep_less, (b[axis] <= v) == keep_less
        if ia: out.append(a)
        if ia != ib:
            f = (v - a[axis]) / (b[axis] - a[axis])
            out.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))
    return out

def area2(poly):
    return sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
               for i in range(len(poly)))

def triangulate(poly):
    """ear clipping (a simple polygon, either winding): index triples"""
    sgn = 1 if area2(poly) > 0 else -1
    idx, tris = list(range(len(poly))), []
    def cr(a, b, c): return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    while len(idx) > 3:
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if cr(a, b, c) * sgn <= 1e-14: continue
            if any(cr(a, b, poly[j]) * sgn > 0 and cr(b, c, poly[j]) * sgn > 0 and cr(c, a, poly[j]) * sgn > 0
                   for j in idx if j not in (i0, i1, i2)): continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            break
        else:
            break  # only slivers left
    if len(idx) == 3: tris.append(tuple(idx))
    return tris

def slab(m, pts, w0, w1=None):
    """a side profile in photo px extruded across x, half width w0 at the front (Z16) to w1 at the butt, flat
    faces: its two sides and the rim round it"""
    w1 = w0 if w1 is None else w1
    zy = [p16(*q) for q in pts]
    def W(z): return w0 + (w1 - w0) * min(max((Z16 - z) / (188 * S16), 0.0), 1.0)
    for side in (-1, 1):
        P = [(side * W(z), y, z) for z, y in zy]
        for a, b, c in triangulate(zy):
            n = norm(cross(sub(P[b], P[a]), sub(P[c], P[a])))
            if n[0] * side < 0: n = tuple(-v for v in n)
            m.t.append(tuple(m.add(P[i], n) for i in (a, b, c)))
    ccw = area2(zy) > 0
    for i in range(len(zy)):
        (za, ya), (zb, yb) = zy[i], zy[(i + 1) % len(zy)]
        if abs(za - zb) + abs(ya - yb) < 1e-6: continue
        q = [(W(za), ya, za), (-W(za), ya, za), (-W(zb), yb, zb), (W(zb), yb, zb)]
        n = norm(cross(sub(q[1], q[0]), sub(q[3], q[0])))
        # outward: the edge's normal in (z, y) on the side away from the profile's inside
        ez, ey = zb - za, yb - ya
        out = (0, -ez, ey) if ccw else (0, ez, -ey)
        if dot(n, out) < 0: n = tuple(-v for v in n)
        e = [m.add(v, n) for v in q]
        m.t += [(e[0], e[1], e[2]), (e[0], e[2], e[3])]

def pbox(m, px0, px1, py0, py1, x0, x1):
    """a box over photo px, x0..x1 across"""
    za, ya = p16(px0, py1)
    zb, yb = p16(px1, py0)
    box(m, x0, x1, ya, yb, za, zb)

def m16():
    m = Mesh()
    yc = 0.066
    # the end plate and castle nut (notched), then the tube
    tube(m, yc, 0.0205, -0.113, -0.124, sides=24)
    for k in range(4):
        a = math.radians(45 + 90 * k)
        x, y = 0.0205 * math.cos(a), yc + 0.0205 * math.sin(a)
        box(m, x - 0.002, x + 0.002, y - 0.002, y + 0.002, -0.125, -0.112)
    tube(m, yc, 0.0155, -0.122, -0.215, sides=24)
    # the tube's housing (py 33..72: 4.4 cm across), a round tube (user), its core 2 mm in under the frame of rails
    # and posts round the photo's 3 rows of 5 windows, each piece a curved strip of the tube's wall; a lip round its
    # front
    R, Rc = 0.0185 * K16, 0.0165 * K16
    def circle(z, r, n=40):
        return [(r * math.cos(2 * math.pi * k / n), yc + r * math.sin(2 * math.pi * k / n), z) for k in range(n)]
    zf, zb = p16(188, 0)[0], p16(21, 0)[0]
    loft(m, [circle(zf + (zb - zf) * i / 4, Rc) for i in range(5)])
    loft(m, [circle(p16(195, 0)[0], R + 0.0015), circle(zf, R + 0.0015)])
    def ang(py): return math.degrees(math.asin(max(-1.0, min(1.0, (p16(0, py)[1] - yc) / R))))
    def wall(side, a0, a1, px0, px1, n=4):
        """a curved strip of the tube's wall, a0..a1 degrees from level on one side, out to R, over photo px"""
        sec = []
        for i in range(n + 1):
            a = math.radians(a0 + (a1 - a0) * i / n)
            sec.append((side * R * math.cos(a), yc + R * math.sin(a)))
        for i in range(n, -1, -1):
            a = math.radians(a0 + (a1 - a0) * i / n)
            sec.append((side * (Rc - 0.0005) * math.cos(a), yc + (Rc - 0.0005) * math.sin(a)))
        loft(m, [[(x, y, p16(px, 0)[0]) for x, y in sec] for px in (px1, px0)])
    for side in (-1, 1):
        for py0, py1 in ((40, 43), (49, 51), (57, 59), (66, 69)):
            wall(side, ang(py1), ang(py0), 21, 178, 2)
        for px0, px1 in ((21, 23), (48, 53), (81, 85), (110, 115), (144, 148), (176, 188)):
            wall(side, ang(69), ang(40), px0, px1, 8)
        # the wall over and under the windows, full height
        wall(side, ang(40), 90, 21, 188, 8)
        wall(side, -90, ang(69), 21, 188, 6)
    # the web under it: its outline off the photo, the sling slot (px 27..64, py 78..84) through it, so built as
    # the bands round the slot; it stops short of the pad's rounded bottom
    web = [(190, 64), (190, 75), (160, 76), (153, 82), (152, 100), (112, 100), (78, 104), (55, 114), (40, 125),
           (30, 140), (26, 156), (20, 156), (20, 64)]
    band = clip(clip(web, 1, 78, False), 1, 84, True)
    for pc in (clip(web, 1, 78, True), clip(web, 1, 84, False), clip(band, 0, 27, True), clip(band, 0, 64, False)):
        if len(pc) >= 3: slab(m, pc, 0.0110 * K16, 0.0150 * K16)
    # the adjustment lever along the web's underside, pivoted at the front by the lock pin, its back end the tab
    # hanging down (px 76..82, to py 127); the lock pin's knob under the front
    slab(m, [(150, 97), (150, 104), (110, 104), (84, 122), (82, 127), (76, 127), (76, 104), (110, 97)], 0.0055 * K16)
    pbox(m, 124, 135, 96, 108, -0.0042 * K16, 0.0042 * K16)
    pbox(m, 122, 137, 101, 104, -0.0030 * K16, 0.0030 * K16)
    # the butt pad (py 31..164, raked back to the top: its back px 7..19, its front px 20..29), oval seen from behind
    # (user): straight sides, round ends 5 cm across, its edges rounded over; ridges across its face
    PW, pt, pb = 0.021 * K16, 31, 164
    rr = PW / S16  # the ends' radius in px
    def oval(n=48):
        """the outline as (x, py): round top, straight sides, round bottom"""
        pts = []
        for k in range(n):
            a = 2 * math.pi * k / n
            c, sn = math.cos(a), math.sin(a)
            cy = pt + rr if sn > 0 else pb - rr
            pts.append((PW * c, cy - rr * sn))
        return pts
    def back(py): return 7 + 12 * (py - pt) / (pb - pt)
    def front(py): return 20 + 9 * (py - pt) / (pb - pt)
    def pad_ring(t, inset):
        out = []
        for x, py in oval():
            # inset toward the oval's middle line, in metres
            k = max(0.0, 1 - inset / PW)
            yy = p16(0, py)[1]
            ym = p16(0, min(max(py, pt + rr), pb - rr))[1]
            px = back(py) + (front(py) - back(py)) * t
            out.append((x * k, ym + (yy - ym) * k, p16(px, 0)[0]))
        return out
    loft(m, [pad_ring(1.0, 0.0012), pad_ring(0.85, 0.0), pad_ring(0.15, 0.0), pad_ring(0.0, 0.0018)])
    for k in range(10):
        py = 42 + k * 12.2
        dy = max(pt + rr - py, py - (pb - rr), 0.0) * S16
        w = math.sqrt(max(PW * PW - dy * dy, 0.0)) - 0.0035
        x = back(py)
        if w > 0.004:
            slab(m, [(x - 1.0, py - 1.2), (x + 0.5, py - 1.2), (x + 0.5, py + 1.2), (x - 1.0, py + 1.2)], w)
    # the sling loop (user): a rectangle of rod across the stock, hanging down below the pad's bottom from a hinge
    # block at the toe
    pbox(m, 21, 30, 157, 166, -0.007 * K16, 0.007 * K16)
    lz = p16(26, 0)[0]
    top, bot = p16(0, 163)[1], p16(0, 177)[1]
    loop = [(u, (top + bot) / 2 + v, lz) for u, v in rrect(0.013 * K16, (top - bot) / 2, 0.003 * K16)]
    sweep(m, loop, [(0.0015 * K16 * math.cos(math.radians(t)), 0.0015 * K16 * math.sin(math.radians(t)))
                    for t in range(0, 360, 45)], closed=True, side=(0.0, 0.0, 1.0))
    return m

# ---- the M249: its fixed stock ------------------------------------------------------------------------------
# traced off the user's side-on photo (968 x 729 px), scaled so its mounting flange (py 85..350) is the receiver's
# back end in the mesh (z -0.144, y 0.039..0.105, 4.4 cm wide; the user: the old one stood proud of it all round):
# 0.249 mm a px, the stock 20 cm long, as the whole gun's photo has it (3 times the flange's height). A flange, a slim
# wrist, the comb rising to a flat top with the cheek rest bevelled in above a crease, the underside dropping in a
# rounded chin to the butt, a rubber pad ridged across its face, the sling swivel on its top
S249 = 0.066 / 265
def p2m(px, py): return (-0.144 - (px - 110) * S249, 0.105 - (py - 85) * S249)

def m249():
    m = Mesh()
    top = [(120, 92), (160, 110), (200, 145), (250, 178), (300, 190), (350, 198), (400, 205), (450, 210), (500, 212),
           (530, 208), (560, 190), (580, 165), (600, 142), (630, 126), (680, 115), (750, 108), (855, 104)]
    bot = [(120, 345), (150, 340), (200, 336), (250, 334), (300, 338), (350, 345), (400, 352), (450, 362),
           (475, 375), (490, 398), (492, 430), (492, 460), (500, 482), (520, 492), (600, 494), (855, 494)]
    wid = [(120, 0.0215), (170, 0.0190), (300, 0.0155), (450, 0.0160), (600, 0.0185), (750, 0.0205), (855, 0.0210)]
    crease = [(560, 192), (650, 228), (745, 262), (855, 268)]  # the cheek rest's lower edge, bevelled in above it
    N = 40

    def ring(yt, yb, w, z, e=0.5, bevel=None):
        yc, h = (yt + yb) / 2, (yt - yb) / 2
        pts = []
        for k in range(N):
            t = 2 * math.pi * k / N
            c, s = math.cos(t), math.sin(t)
            x, y = w * math.copysign(abs(c) ** e, c), yc + h * math.copysign(abs(s) ** e, s)
            if bevel is not None and y > bevel:
                x *= 1 - 0.28 * ((y - bevel) / max(yt - bevel, 1e-6)) ** 1.5
            pts.append((x, y, z))
        return pts

    rings = []
    for px in range(120, 856, 8):
        yt = p2m(0, smooth_table(top, px))[1]
        yb = p2m(0, smooth_table(bot, px))[1]
        bev = None
        if px >= 560:
            fade = min((px - 560) / 40, 1.0)
            bev = yt - (yt - p2m(0, lerp_table(crease, px))[1]) * fade
        rings.append(ring(yt, yb, lerp_table(wid, px), p2m(px, 0)[0], bevel=bev))
    loft(m, rings)
    # the flange on the receiver's back: a plate the receiver's size, squarer, 5 mm thick
    ft, fb = p2m(0, 85)[1], p2m(0, 350)[1]
    loft(m, [ring(ft, fb, 0.0225, p2m(100, 0)[0], 0.3), ring(ft, fb, 0.0225, p2m(122, 0)[0], 0.3)])
    # the rubber butt pad, a little proud of the stock, ridges across its face
    pt, pb = p2m(0, 96)[1], p2m(0, 497)[1]
    zp0, zp1 = p2m(855, 0)[0], p2m(897, 0)[0]
    loft(m, [ring(pt, pb, 0.0225, zp0), ring(pt, pb, 0.0225, zp1)])
    for k in range(16):
        y = p2m(0, 150 + k * 21)[1]
        box(m, -0.018, 0.018, y - 0.0012, y + 0.0012, zp1 - 0.0015, zp1 + 0.0004)
    # the sling swivel on the pad's top: a plate and its ring
    zs, ys = p2m(860, 72)
    box(m, -0.003, 0.003, pt - 0.001, ys + 0.002, zs - 0.004, zs + 0.004)
    loop = [(0.0, ys + 0.0028 * math.sin(math.radians(a)), zs + 0.0028 * math.cos(math.radians(a))) for a in range(0, 360, 20)]
    sweep(m, loop, [(0.0012 * math.cos(math.radians(a)), 0.0012 * math.sin(math.radians(a))) for a in range(0, 360, 45)],
          closed=True)
    return m

# ---- texture layout: every triangle its own cell in a texture of the stock's own (user: shadows and darkened
# edges painted on, as the game's guns have), so each texel can be shaded on its own. The game fills it at run time
# (vr_gun.cpp VrStockBake): the receiver patch tiled over each triangle at its point on the stock (its st, metres
# on the plane square to the triangle's main axis), darkened by how much of the stock is near and in front of it
# and by nearness to a sharp edge (the edges marked here). No texel of the game's is in here
ATLAS_W = 1024
ATLAS_PX_PER_M = 600.0
PAD = 2

def atlas(m, ATLAS_W=ATLAS_W, ATLAS_PX_PER_M=ATLAS_PX_PER_M):
    """m's triangles unshared, each with its cell: per corner position, normal, atlas uv (0..1), st; per triangle
    its sharp edges (bit k: corners k, k+1), and its material and bone when m has them"""
    def key(p): return tuple(round(c, 5) for c in p)
    fn = []
    for a, b, c in m.t:
        n = cross(sub(m.p[b], m.p[a]), sub(m.p[c], m.p[a]))
        l = math.sqrt(dot(n, n))
        fn.append(tuple(x / l for x in n) if l > 1e-12 else (0.0, 0.0, 0.0))
    edges = {}
    for i, (a, b, c) in enumerate(m.t):
        for u, v in ((a, b), (b, c), (c, a)):
            edges.setdefault(tuple(sorted((key(m.p[u]), key(m.p[v])))), []).append(i)
    cells = []
    for i, (a, b, c) in enumerate(m.t):
        p0, p1, p2 = m.p[a], m.p[b], m.p[c]
        e1 = sub(p1, p0)
        l1 = math.sqrt(dot(e1, e1)) or 1e-9
        x = tuple(v / l1 for v in e1)
        y = cross(fn[i], x)
        q = [(0.0, 0.0), (l1, 0.0), (dot(sub(p2, p0), x), dot(sub(p2, p0), y))]
        if q[2][1] < 0: q = [(u, -v) for u, v in q]
        x0 = min(u for u, v in q)
        q = [(u - x0, v) for u, v in q]
        w = max(1, math.ceil(max(u for u, v in q) * ATLAS_PX_PER_M)) + 2 * PAD
        h = max(1, math.ceil(max(v for u, v in q) * ATLAS_PX_PER_M)) + 2 * PAD
        cells.append([i, w, h, q])
    # shelf packing, tallest first
    order = sorted(range(len(cells)), key=lambda k: -cells[k][2])
    cx = cy = row = 0
    place = {}
    for k in order:
        i, w, h, q = cells[k]
        if cx + w > ATLAS_W: cx, cy, row = 0, cy + row, 0
        place[i] = (cx, cy)
        cx += w
        row = max(row, h)
    H = cy + row
    H = 1 << max(0, (H - 1).bit_length())  # a power of two
    out = Mesh()
    out.uv, out.st, out.edge = [], [], []
    for i, (a, b, c) in enumerate(m.t):
        ox, oy = place[i]
        q = cells[i][3]
        n = fn[i]
        ax = max(range(3), key=lambda k: abs(n[k]))
        bits = 0
        for k, (u, v) in enumerate(((a, b), (b, c), (c, a))):
            others = [j for j in edges[tuple(sorted((key(m.p[u]), key(m.p[v]))))] if j != i]
            if not others or min(dot(fn[i], fn[j]) for j in others) < math.cos(math.radians(35)):
                bits |= 1 << k
        out.edge.append(bits)
        tri = []
        for k, vi in enumerate((a, b, c)):
            p = m.p[vi]
            tri.append(out.add(p, m.n[vi]))
            out.uv.append(((ox + PAD + q[k][0] * ATLAS_PX_PER_M) / ATLAS_W, (oy + PAD + q[k][1] * ATLAS_PX_PER_M) / H))
            out.st.append({0: (p[2], p[1]), 1: (p[2], p[0]), 2: (p[0], p[1])}[ax])
        out.t.append(tuple(tri))
    out.atlas = (ATLAS_W, H)
    out.ppm = ATLAS_PX_PER_M
    if hasattr(m, 'mat'): out.mat = m.mat
    if hasattr(m, 'bone'): out.bone = m.bone
    return out

# ---- output ------------------------------------------------------------------------------------------------
def emit(name, m, out):
    out.append('constexpr uint32_t k%sAtlas[] = {%d, %d};' % (name, m.atlas[0], m.atlas[1]))
    out.append('constexpr float k%sPos[] = {' % name)
    out.append(',\n'.join('    %.5ff, %.5ff, %.5ff' % p for p in m.p) + '};')
    out.append('constexpr float k%sNrm[] = {' % name)
    out.append(',\n'.join('    %.4ff, %.4ff, %.4ff' % n for n in m.n) + '};')
    out.append('constexpr float k%sUv[] = {' % name)
    out.append(',\n'.join('    %.6ff, %.6ff' % uv for uv in m.uv) + '};')
    out.append('constexpr float k%sSt[] = {' % name)
    out.append(',\n'.join('    %.5ff, %.5ff' % st for st in m.st) + '};')
    out.append('constexpr uint8_t k%sEdge[] = {' % name)
    out.append(',\n'.join('    ' + ', '.join(str(e) for e in m.edge[k:k + 32]) for k in range(0, len(m.edge), 32)) + '};')
    if hasattr(m, 'mat'):
        out.append('constexpr float k%sPxPerM = %.1ff;' % (name, m.ppm))
        for what, arr in (('Mat', m.mat), ('Bone', getattr(m, 'bone', None))):
            if arr is None: continue
            out.append('constexpr uint8_t k%s%s[] = {' % (name, what))
            out.append(',\n'.join('    ' + ', '.join(str(e) for e in arr[k:k + 32]) for k in range(0, len(arr), 32)) + '};')
    out.append('constexpr uint16_t k%sIdx[] = {' % name)
    flat = [i for t in m.t for i in t]
    out.append(',\n'.join('    ' + ', '.join(str(i) for i in flat[k:k + 18]) for k in range(0, len(flat), 18)) + '};')

# ---- preview: a z-buffered, shaded render ----------------------------------------------------------------
def png(path, w, h, px):
    raw = b''.join(b'\x00' + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    ch = lambda t, c: struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))

def render(m, path, yaw, pitch, w=640, h=420):
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    def view(p):  # turn about y, then x
        x, y, z = p
        x, z = x * cy + z * sy, -x * sy + z * cy
        y, z = y * cp - z * sp, y * sp + z * cp
        return x, y, z
    vp = [view(p) for p in m.p]
    vn = [view(n) for n in m.n]
    xs = [p[0] for p in vp]; ys = [p[1] for p in vp]
    sc = min((w - 40) / (max(xs) - min(xs)), (h - 40) / (max(ys) - min(ys)))
    ox, oy = (w - (max(xs) - min(xs)) * sc) / 2 - min(xs) * sc, (h + (max(ys) - min(ys)) * sc) / 2 + min(ys) * sc
    scr = [(ox + p[0] * sc, oy - p[1] * sc, p[2]) for p in vp]
    zb = [-1e9] * (w * h)
    px = bytearray([235] * (w * h * 3))
    light = norm((0.4, 0.7, 0.6))
    for a, b, c in m.t:
        A, B, C = scr[a], scr[b], scr[c]
        x0, x1 = int(max(min(A[0], B[0], C[0]), 0)), int(min(max(A[0], B[0], C[0]) + 1, w - 1))
        y0, y1 = int(max(min(A[1], B[1], C[1]), 0)), int(min(max(A[1], B[1], C[1]) + 1, h - 1))
        den = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1])
        if abs(den) < 1e-9: continue
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                l0 = ((B[1] - C[1]) * (x - C[0]) + (C[0] - B[0]) * (y - C[1])) / den
                l1 = ((C[1] - A[1]) * (x - C[0]) + (A[0] - C[0]) * (y - C[1])) / den
                l2 = 1 - l0 - l1
                if l0 < 0 or l1 < 0 or l2 < 0: continue
                z = l0 * A[2] + l1 * B[2] + l2 * C[2]
                i = y * w + x
                if z <= zb[i]: continue
                zb[i] = z
                n = norm(tuple(l0 * vn[a][k] + l1 * vn[b][k] + l2 * vn[c][k] for k in range(3)))
                d = abs(dot(n, light))
                v = int(25 + 200 * d)
                px[i * 3:i * 3 + 3] = bytes((v, v, v))
    png(path, w, h, px)

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    # the M249's para stock (tools/m249stock.py; -I keeps this folder off the path)
    sys.path.insert(0, here)
    import m249stock
    meshes = {'M16Stock': m16(), 'M249Stock': m249stock.stock()}
    previews = dict(meshes)
    meshes = {name: atlas(m) for name, m in meshes.items()}
    out = ['// generated by tools/stocks.py (the vr buttstocks, lofted from the user\'s photos): do not edit by hand',
           '#pragma once', '#include <cstdint>', '', 'namespace game {']
    for name, m in meshes.items():
        emit(name, m, out)
        print(name, len(m.p), 'vertices', len(m.t), 'triangles, atlas', m.atlas)
        assert len(m.p) < 65536
    out += ['}  // namespace game', '']
    args = sys.argv[1:]
    dest = os.path.join(here, '..', 'src', 'game', 'vr_stocks.h')
    if args[:1] == ['--out']: dest, args = args[1], args[2:]
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    open(dest, 'w', newline='\n').write('\n'.join(out))
    if args:
        for name, m in previews.items():
            render(m, os.path.join(args[0], name + '_side.png'), math.radians(90), 0)
            render(m, os.path.join(args[0], name + '_34.png'), math.radians(55), math.radians(-20))
