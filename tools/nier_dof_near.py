"""NieR (2026-11-16): the depth of field's near blur, switchable (cvar nier_near_blur, render/draw.cpp).

The game's two depth-of-field composite pixel shaders (ps_c482401cc5d71351, ps_31d7631cf15790b1) blur a pixel by
the level log2|r| of its mip chain, r = (d*x + y) / (d*z) for the pixel's distance d and dofConstant (x, y, z):
r = x/z + (y/z)/d. Where |r| grows as d shrinks (r*y*z > 0) the blur is the near side: a character close to the
camera, smeared (the user, 4K). With c2.x = 1 (written by the renderer for these two shaders only) that side keeps
level 0, the scene as drawn; the far side is as the game made it.

Run on a folder of the translator's HLSL dumps (BANJO_HLSL_DUMP_ONLY); rewrites the two in place and deletes every
other file there, so the folder holds only these overrides (BANJO_HLSL_OVERRIDE). A dump whose text is not as
expected is left out (the shader then compiles as the game's, the option doing nothing): never a broken shader.
"""
import pathlib
import re
import sys

SHADERS = ("ps_c482401cc5d71351", "ps_31d7631cf15790b1")


def patch(text):
    # the ratio r, signed: the line right after "r0.w = r0.w + dofConstant.y;" ... "r0.z = r0.w * r0.z;"
    ratio = re.search(r"r0\.w = r0\.w \+ dofConstant\.y;\n(?:.*\n){2}\s*r0\.z = r0\.w \* r0\.z;\n", text)
    lod = re.search(r"\n(\s*)lod = (r\d+\.[xyzw]);\n", text)
    if not ratio or not lod or lod.start() < ratio.end():
        return None
    level = lod.group(2)
    text = (text[:ratio.end()] + "\tfloat nierRatio = r0.z;\n" + text[ratio.end():lod.start()] +
            "\n" + lod.group(1) + "if (g_C(2).x > 0.5 && nierRatio * dofConstant.y * dofConstant.z > 0.0) " + level +
            " = 0.0;  // NieR: nier_near_blur off" + text[lod.start():])
    return text


def main(folder):
    folder = pathlib.Path(folder)
    done = 0
    for f in folder.iterdir():
        if f.suffix != ".hlsl" or f.stem.lower() not in SHADERS:
            if f.is_file():
                f.unlink()
            continue
        new = patch(f.read_text())
        if new is None:
            print(f"nier_dof_near: {f.name} not as expected, left as the game's")
            f.unlink()
            continue
        f.write_text(new)
        done += 1
    print(f"nier_dof_near: {done} depth of field shaders patched")


if __name__ == "__main__":
    main(sys.argv[1])
