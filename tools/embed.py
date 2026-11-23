# embed.py <in> <out> <name>: a text file as a c++ string (TOTEST.md for the in-game test checklist, vr_totest.cpp)
import sys

src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src, encoding="utf-8").read()
assert ")embed\"" not in text
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("// generated from %s by tools/embed.py\n#pragma once\n" % src.replace("\\", "/").split("/")[-1])
    f.write("static const char %s[] = R\"embed(%s)embed\";\n" % (name, text))
