#!/usr/bin/env python3
"""Junta as regras de ProGuard/R8 que as bibliotecas trazem (sem Gradle).

Uso: python3 tools/collect_proguard.py build/deps saida.pro
  - AAR: proguard.txt (regras "consumer" da biblioteca)
  - JAR: META-INF/com.android.tools/r8*/ *.pro (preferidas) ou META-INF/proguard/*.pro
Lê build/deps/resolved.txt (gerado por tools/android_deps.py) e os arquivos de build/deps/cache.
"""
import os
import sys
import zipfile


def gpath(g, a, v, ext):
    return f"{g.replace('.', '/')}/{a}/{v}/{a}-{v}.{ext}".replace("/", "_")


def main():
    deps, out = sys.argv[1], sys.argv[2]
    cache = os.path.join(deps, "cache")
    parts = []
    for line in open(os.path.join(deps, "resolved.txt")):
        f = line.split()
        if len(f) < 2 or f[1] not in ("aar", "jar"):
            continue
        g, a, v = f[0].split(":")
        z = os.path.join(cache, gpath(g, a, v, f[1]))
        if not os.path.exists(z):
            continue
        with zipfile.ZipFile(z) as zf:
            names = zf.namelist()
            if f[1] == "aar":
                if "proguard.txt" in names:
                    parts.append(f"# ---- {f[0]} (proguard.txt)\n" + zf.read("proguard.txt").decode("utf-8", "replace"))
                # libs/*.jar e classes.jar podem ter regras R8 embutidas
                inner = [n for n in names if n.endswith(".jar")]
                for n in inner:
                    try:
                        import io
                        with zipfile.ZipFile(io.BytesIO(zf.read(n))) as jz:
                            parts += rules_from_jar(jz, f"{f[0]}!{n}")
                    except zipfile.BadZipFile:
                        pass
            else:
                parts += rules_from_jar(zf, f[0])
    open(out, "w").write("\n".join(parts) + "\n")
    print(f"regras R8: {len(parts)} blocos -> {out}")


def rules_from_jar(z, tag):
    names = z.namelist()
    r8 = [n for n in names if n.startswith("META-INF/com.android.tools/r8") and n.endswith(".pro")]
    pg = [n for n in names if n.startswith("META-INF/proguard/") and n.endswith(".pro")]
    sel = r8 if r8 else pg
    return [f"# ---- {tag} ({n})\n" + z.read(n).decode("utf-8", "replace") for n in sel]


if __name__ == "__main__":
    main()
