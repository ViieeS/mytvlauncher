"""Instance lumen's variable Geist (woff2) at the weights the UI uses, as static TTFs."""
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
src, outdir = sys.argv[1], sys.argv[2]
for w in (300, 500, 600, 650, 700, 800):
    f = TTFont(src)
    f.flavor = None
    instancer.instantiateVariableFont(f, {"wght": w}, inplace=True)
    f.save(f"{outdir}/geist-{w}.ttf")
    print("wrote", w)
