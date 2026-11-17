#!/usr/bin/env python3
"""make_skin.py - Sunflower's hand-made MPC touch skin.

The SDK generates a plain screen from ui.json; this copies it to skins/ and adds:
  * bg.png         a faint Vogel sunflower (golden angle, 1,300 florets, a ring of petals) behind every page, 1280 x 628.
  * playhead_strip the LIVE PLAYHEAD: 64 pictures (260 x 260 each) of the sunflower with a bright comet sweeping the florets once per cycle. It is
                   shown by a film-strip control bound to the hidden "Playhead" parameter, which the plugin itself moves (see Sunflower.cpp):
                   about 10 updates a second, beat-level, not a smooth sweep. Placed bottom centre, clear of every control (a transparent
                   picture has no touch area, but it is kept off the controls anyway).
  * lattice_dial   the DIVERGENCE knob becomes the lattice itself: a 90 x 90 film strip (127 frames) of a Vogel spiral
                   whose divergence angle runs 130 .. 145 degrees. At 137.508 (the golden angle) the florets pack into
                   the two Fibonacci spiral families; away from it they collapse into gaps and rays. So the knob is also
                   a lesson in why the sunflower uses that angle. Gold ring = value.
Touchscreen skin: nothing animates; the picture changes because YOU turn the dial (the MPC UI format has no animation
channel). Same pipeline as Vulcan Rotary:
    1. delete <P>/skins   2. tools/build.py <P>   3. python <P>/tools/make_skin.py <P>   4. tools/build.py <P>
TUI.json is patched as TEXT (duplicate "version" keys) through skin_fix.py.
"""
import math, os, shutil, sys
from PIL import Image, ImageDraw, ImageFilter

FRAMES, FS, SS = 127, 90, 4
PH_FRAMES, PH_SIZE, PH_X, PH_Y, PH_PARAM = 64, 260, 510, 352, 11   # playhead: frames, picture size, position on the screen, parameter index
GOLD = (245, 184, 46)
LO, HI = 130.0, 145.0


def vogel(draw, cx, cy, n, deg, scale, rmax, col, rad):
    a = math.radians(deg)
    for i in range(1, n + 1):
        r = scale * math.sqrt(i)
        if r > rmax: break
        x, y = cx + r * math.cos(i * a), cy + r * math.sin(i * a)
        t = i / n
        rr = rad * (0.55 + 0.45 * (1 - t))
        draw.ellipse((x - rr, y - rr, x + rr, y + rr), fill=col)


def dial_frame(norm):
    w = FS * SS; c = w / 2
    deg = LO + (HI - LO) * norm
    img = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    bb = (c - 42 * SS, c - 42 * SS, c + 42 * SS, c + 42 * SS)
    d.arc(bb, 135, 405, fill=(70, 56, 30, 255), width=int(5 * SS))
    if norm > 0.003: d.arc(bb, 135, 135 + 270 * norm, fill=GOLD + (255,), width=int(5 * SS))
    d.ellipse((c - 35 * SS, c - 35 * SS, c + 35 * SS, c + 35 * SS), fill=(20, 12, 4, 255), outline=GOLD + (255,), width=int(1.6 * SS))
    vogel(d, c, c, 260, deg, 2.15 * SS, 31 * SS, (255, 214, 120, 255), 1.5 * SS)
    d.ellipse((c - 2.5 * SS, c - 2.5 * SS, c + 2.5 * SS, c + 2.5 * SS), fill=(90, 50, 12, 255))
    return img.resize((FS, FS), Image.LANCZOS)


def playhead_frame(k):
    """Frame k of 64: the sunflower lattice, dim, with a comet of lit florets over the last 70 degrees of the sweep and a ring with a tick."""
    SS2 = 2
    w = PH_SIZE * SS2; c = w / 2
    img = Image.new("RGBA", (w, w), (0, 0, 0, 0)); d = ImageDraw.Draw(img)
    phase = k / PH_FRAMES * 2 * math.pi                      # 0 = straight up, clockwise
    R = 104 * SS2
    d.ellipse((c - 122 * SS2, c - 122 * SS2, c + 122 * SS2, c + 122 * SS2), outline=(120, 90, 30, 110), width=2 * SS2)           # the ring
    d.arc((c - 122 * SS2, c - 122 * SS2, c + 122 * SS2, c + 122 * SS2), -90, -90 + math.degrees(phase), fill=(245, 184, 46, 200), width=4 * SS2)   # progress
    tx, ty = c + 122 * SS2 * math.sin(phase), c - 122 * SS2 * math.cos(phase)
    d.ellipse((tx - 7 * SS2, ty - 7 * SS2, tx + 7 * SS2, ty + 7 * SS2), fill=(255, 236, 170, 255))                              # the tick
    ga = math.radians(137.50776405)
    for n in range(1, 420):
        r = 4.9 * SS2 * math.sqrt(n)
        if r > R: break
        th = n * ga; x, y = c + r * math.cos(th), c + r * math.sin(th)
        ang = (math.atan2(x - c, -(y - c))) % (2 * math.pi)       # clockwise from up, like the sweep
        behind = (phase - ang) % (2 * math.pi)                      # how far the sweep has passed this floret (0 = just now)
        rr = (2.0 + 1.1 * (1 - n / 420)) * SS2
        if behind < math.radians(70):                               # in the comet: bright, fading to the tail
            a = 1 - behind / math.radians(70)
            col = (255, int(190 + 60 * a), int(70 + 130 * a), int(110 + 145 * a)); rr *= (1.0 + 0.55 * a)
        else:
            col = (200, 150, 50, 70)
        d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=col)
    return img.resize((PH_SIZE, PH_SIZE), Image.LANCZOS)


def add_playhead(root, sf, out):
    """The playhead control: a film-strip knob bound to the hidden parameter, with only the picture (no label, focus rectangle or value text)."""
    import copy
    strip = Image.new("RGBA", (PH_SIZE, PH_SIZE * PH_FRAMES), (0, 0, 0, 0))
    for k in range(PH_FRAMES): strip.paste(playhead_frame(k), (0, k * PH_SIZE))
    strip.save(os.path.join(out, "playhead_strip.png"), optimize=True)
    d = copy.deepcopy(sf.find_def(root, "knobLattice"))
    keep = []
    for c in sf.get(d, "componentsData"):
        cd = sf.get(c, "componentData")
        if sf.get(cd, "name") == "knob":
            dat = sf.get(cd, "data"); sf.put(dat, "filmStrip", "playhead_strip.png"); sf.put(dat, "numFrames", PH_FRAMES)
            sf.put(sf.get(c, "bounds"), "bounds", f"0 0 {PH_SIZE} {PH_SIZE}"); keep.append(c)
    sf.put(d, "componentsData", sf.Arr(keep) if hasattr(sf, "Arr") else keep)
    sf.defs(root).append(sf.Obj([("key", "knobPlayhead"), ("value", d)]))
    tab = sf.tabs(root)[0]
    src = [c for c in sf.tab_children(tab) if sf.comp_name(c) == "Divergence"][0]
    comp = copy.deepcopy(src)
    cd = sf.get(comp, "componentData"); sf.put(cd, "name", "Playhead"); sf.put(cd, "type", "knobPlayhead")
    sf.put(sf.get(sf.get(comp, "handle remapping"), "map")[0], "value", f"Parameter {PH_PARAM}")
    b = sf.get(comp, "bounds"); sf.put(b, "bounds", f"{PH_X} {PH_Y} {PH_SIZE} {PH_SIZE}"); sf.put(b, "acceptsHWFocus", "No")
    sf.tab_children(tab).append(comp)


def make_background():
    W, H = 1280, 628
    bg = Image.new("RGBA", (W, H), (14, 10, 5, 255))
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(layer)
    cx, cy = W / 2, H / 2
    for k in range(34):                                   # petals
        ang = k * 2 * math.pi / 34
        px, py = cx + 285 * math.cos(ang), cy + 285 * math.sin(ang)
        d.ellipse((px - 34, py - 16, px + 34, py + 16), fill=(245, 184, 46, 70))
    ov = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    petals = Image.new("RGBA", (W, H), (0, 0, 0, 0)); pd = ImageDraw.Draw(petals)
    for k in range(34):
        ang = k * 2 * math.pi / 34
        pts = []
        for s in range(-10, 11):
            t = s / 10.0
            lx, ly = 60 * (1 + t) , 18 * (1 - t * t)
            pts.append((lx, ly))
        # simple petal: ellipse rotated by drawing on a small canvas
        pc = Image.new("RGBA", (130, 50), (0, 0, 0, 0)); ImageDraw.Draw(pc).ellipse((0, 4, 128, 46), fill=(245, 184, 46, 26))
        pc = pc.rotate(-math.degrees(ang), expand=True, resample=Image.BICUBIC)
        petals.alpha_composite(pc, (int(cx + 235 * math.cos(ang) - pc.width / 2), int(cy + 235 * math.sin(ang) - pc.height / 2)))
    bg.alpha_composite(petals)
    disc = Image.new("RGBA", (W, H), (0, 0, 0, 0)); dd = ImageDraw.Draw(disc)
    vogel(dd, cx, cy, 1300, 137.50776405, 7.3, 262, (255, 200, 80, 38), 3.3)
    bg.alpha_composite(disc)
    return bg.convert("RGB")


def main():
    project = os.path.abspath(sys.argv[1]); build = os.path.join(project, "build"); src = None
    for d in os.listdir(build):
        cand = os.path.join(build, d, "Plugin Skins")
        if os.path.isdir(cand): src = cand
    if not src: sys.exit("no generated Plugin Skins under build/ - run tools/build.py first (with skins/ removed)")
    out = os.path.join(project, "skins")
    if os.path.isdir(out): shutil.rmtree(out)
    shutil.copytree(src, out)
    strip = Image.new("RGBA", (FS, FS * FRAMES), (0, 0, 0, 0))
    for f in range(FRAMES): strip.paste(dial_frame(f / (FRAMES - 1)), (0, f * FS))
    strip.save(os.path.join(out, "lattice_dial.png"), optimize=True)
    make_background().save(os.path.join(out, "bg.png"), optimize=True)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import skin_fix as sf
    p = os.path.join(out, "TUI.json"); root = sf.load(p)
    n_bg = sf.add_background(root, "bg.png")
    n_dial = sf.clone_knob_def(root, "knobLattice", "lattice_dial.png", "Divergence")
    add_playhead(root, sf, out)
    n_menu = sf.fix_menus(root)
    made = sf.make_translucent_segments(out, GOLD)
    n_img, n_move = sf.fix_segments(root)
    sf.save(root, p)
    print("skin written to", out, "| backgrounds:", n_bg, "| Divergence dial:", n_dial, "| menus -> value fields:", n_menu, "| segment images:", len(made), "| captions moved:", n_move)

if __name__ == "__main__": main()
