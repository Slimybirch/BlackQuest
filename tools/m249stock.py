# The M249's para stock, out (user: on the game's own M249, its photo of an M249 Para): two round bars one above
# the other running back from the receiver, the upper from its back plate, the lower from the buffer's housing under
# it; the butt an upright oval, its back dished to sit snug in the shoulder's crease under the arm. In the game's
# M249 mesh (metres; z to the muzzle, y up, +x the gun's left): its receiver's back plate at z -0.144, y 0.039..0.105,
# 4.4 cm wide. Each triangle has a material (vr_gun.cpp's kParaLook: 0 metal, 1 polymer, 3 dark steel).
import math

from stocks import Mesh, norm, sub, cross, loft, box

METAL, POLY, STEEL = 0, 1, 3


class Parts(Mesh):
    """a mesh whose triangles carry a material (set before each part)"""

    def __init__(self):
        super().__init__()
        self.mat = []
        self.cur = METAL

    def use(self, mat):
        self.close()
        self.cur = mat

    def close(self):
        self.mat += [self.cur] * (len(self.t) - len(self.mat))


def cyl(m, p0, p1, r, sides=16):
    """a cylinder between two points along z, capped"""
    rings = [[(p[0] + r * math.cos(2 * math.pi * i / sides), p[1] + r * math.sin(2 * math.pi * i / sides), p[2])
              for i in range(sides)] for p in (p0, p1)]
    loft(m, rings)


def oval(cy, hw, hh, z, k=1.0, n=28):
    """an upright oval (half width, half height) about y cy at z, scaled by k"""
    return [(k * hw * math.cos(2 * math.pi * i / n), cy + k * hh * math.sin(2 * math.pi * i / n), z) for i in range(n)]


UPPER_Y, LOWER_Y = 0.090, 0.030  # the bars, one over the other (user: upright, as the photo has them)
BAR_R = 0.0075
BACK = -0.144                    # the receiver's back plate
BUTT_Z = -0.385                  # the butt's front face (the stock out: 24 cm behind the receiver)


def stock():
    m = Parts()
    # the buffer's housing under the receiver's back, its cap behind it
    m.use(METAL)
    cyl(m, (0.0, 0.036, BACK - 0.058), (0.0, 0.036, BACK + 0.004), 0.019, sides=20)
    cyl(m, (0.0, 0.036, BACK - 0.070), (0.0, 0.036, BACK - 0.058), 0.012, sides=16)
    # the bars back to the butt (the upper from the back plate, the lower from the housing)
    m.use(STEEL)
    cyl(m, (0.0, UPPER_Y, BUTT_Z - 0.004), (0.0, UPPER_Y, BACK + 0.002), BAR_R)
    cyl(m, (0.0, LOWER_Y, BUTT_Z - 0.004), (0.0, LOWER_Y, BACK - 0.070), BAR_R)
    # the butt's sockets the bars go into
    m.use(METAL)
    for y in (UPPER_Y, LOWER_Y):
        box(m, -0.011, 0.011, y - 0.011, y + 0.011, BUTT_Z, BUTT_Z + 0.016)
    # the butt: an upright oval, bevelled at its front, its back dished (deepest in the middle) to sit in the
    # shoulder's crease
    m.use(POLY)
    cy, hw, hh = 0.058, 0.022, 0.068
    z0, z1 = BUTT_Z, BUTT_Z - 0.026  # front, back rim
    rings = [oval(cy, hw, hh, z0, 0.86), oval(cy, hw, hh, z0 - 0.004, 1.0), oval(cy, hw, hh, z1 + 0.004, 1.0),
             oval(cy, hw, hh, z1, 0.94)]
    dish = 0.010
    for k in (0.75, 0.5, 0.25):
        rings.append(oval(cy, hw, hh, z1 + dish * (1 - k * k), 0.94 * k))
    loft(m, rings, cap_front=True, cap_back=True)
    right_side(m)
    m.close()
    return m


# ---- the M249's right side (user: the game modelled 42% of it, the flat game never showing it; from their photos
# of the real gun's right). Measured off the game's mesh (dev_dumpgun): its right half stops under y 0.09 on the
# receiver and the handguard (the left's walls at x 15-22 mm and 20-29 mm); the box's right face and the grip's are
# there. Panels on that open side, and what the photos have on it: the side plates and their rivets, the charging
# handle's slot with its rails, the ejection port's cover. Their texture (vr_gun.cpp): HANDGUARD always the left's
# own texels at the mirrored spot (its grooves); the rest the left's (option A) or our own section (option B: a look
# of kParaLook, the material less 10)
HANDGUARD = 7
WALL, SLOT = 10 + METAL, 10 + 6  # 6: kParaLook's dark


def flat(m, x, z0, z1, y0, y1, step=0.01):
    """a flat panel on the plane x (facing -x), cut into cells (each its own texels at the left's mirrored spot)"""
    nz, ny = max(1, round((z1 - z0) / step)), max(1, round((y1 - y0) / step))
    n = (-1.0, 0.0, 0.0)
    for i in range(nz):
        for j in range(ny):
            za, zb = z0 + (z1 - z0) * i / nz, z0 + (z1 - z0) * (i + 1) / nz
            ya, yb = y0 + (y1 - y0) * j / ny, y0 + (y1 - y0) * (j + 1) / ny
            q = [m.add((x, ya, za), n), m.add((x, ya, zb), n), m.add((x, yb, zb), n), m.add((x, yb, za), n)]
            m.t += [(q[0], q[1], q[2]), (q[0], q[2], q[3])]


def ledge(m, x0, x1, y, z0, z1, step=0.01):
    """a flat strip on the plane y (facing up) from x0 to x1, in cells along z"""
    nz = max(1, round((z1 - z0) / step))
    n = (0.0, 1.0, 0.0)
    for i in range(nz):
        za, zb = z0 + (z1 - z0) * i / nz, z0 + (z1 - z0) * (i + 1) / nz
        q = [m.add((x0, y, za), n), m.add((x0, y, zb), n), m.add((x1, y, zb), n), m.add((x1, y, za), n)]
        m.t += [(q[0], q[1], q[2]), (q[0], q[2], q[3])]


def rbox(m, x0, x1, y0, y1, z0, z1):
    box(m, x0, x1, y0, y1, z0, z1)


def right_side(m):
    m.use(WALL)
    flat(m, -0.020, -0.140, 0.200, 0.030, 0.092)            # the receiver's wall
    # its upper band under the top cover, set in as the left's is (x 13-17 mm there: its four slots, the long
    # channel, rivets; user: the game's own right band there was plain), and the ledge down to the wall
    flat(m, -0.0165, -0.140, 0.200, 0.092, 0.112)
    ledge(m, -0.020, -0.0165, 0.092, -0.140, 0.200)
    rbox(m, -0.0215, -0.020, 0.035, 0.060, -0.135, -0.062)  # the side plates, front and back
    rbox(m, -0.0215, -0.020, 0.035, 0.060, 0.122, 0.196)
    rbox(m, -0.0225, -0.020, 0.032, 0.046, 0.040, 0.112)    # the ejection port's cover
    rbox(m, -0.0215, -0.020, 0.078, 0.081, 0.020, 0.192)    # the charging handle slot's rails
    rbox(m, -0.0215, -0.020, 0.065, 0.068, 0.020, 0.192)
    for z, y in ((-0.128, 0.042), (-0.128, 0.054), (-0.070, 0.048), (0.130, 0.042), (0.188, 0.054),
                 (0.160, 0.086), (-0.110, 0.086), (0.010, 0.040)):
        cyl_x(m, z, y)
    m.use(SLOT)
    flat(m, -0.0203, 0.022, 0.190, 0.068, 0.078, 0.02)      # the slot (dark in B)
    m.use(HANDGUARD)
    flat(m, -0.024, 0.205, 0.435, 0.035, 0.105)             # the handguard's side


def cyl_x(m, z, y, r=0.0022, sides=8):
    """a rivet's head on the right wall: a short cylinder out along -x, capped"""
    rings = []
    for x in (-0.020, -0.0217):
        rings.append([(x, y + r * math.cos(2 * math.pi * i / sides), z + r * math.sin(2 * math.pi * i / sides))
                      for i in range(sides)])
    for s in range(sides):
        a, b = rings[0][s], rings[0][(s + 1) % sides]
        c, d = rings[1][(s + 1) % sides], rings[1][s]
        nrm = norm((0.0, (a[1] + b[1]) / 2 - y, (a[2] + b[2]) / 2 - z))
        q = [m.add(p, nrm) for p in (a, b, c, d)]
        m.t += [(q[0], q[1], q[2]), (q[0], q[2], q[3])]
    c = m.add((-0.0217, y, z), (-1.0, 0.0, 0.0))
    e = [m.add(p, (-1.0, 0.0, 0.0)) for p in rings[1]]
    for s in range(sides): m.t.append((c, e[s], e[(s + 1) % sides]))
