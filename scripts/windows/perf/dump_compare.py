import hashlib
import os
import sys

a, b = sys.argv[1], sys.argv[2]
fa, fb = set(os.listdir(a)), set(os.listdir(b))


def h(p):
    return hashlib.sha1(open(p, "rb").read()).hexdigest()


common = sorted(fa & fb)
differ = [f for f in common if h(os.path.join(a, f)) != h(os.path.join(b, f))]
print(f"identical {len(common) - len(differ)} of {len(fa)} (reference) / {len(fb)} (run); "
      f"only reference {sorted(fa - fb)[:4]} only run {sorted(fb - fa)[:4]}")
if differ:
    print("differ:", differ[:10])
