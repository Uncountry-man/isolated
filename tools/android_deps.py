#!/usr/bin/env python3
"""Resolve dependências Maven (AAR/JAR) SEM Gradle e extrai o que o build.sh precisa.

Uso: python3 tools/android_deps.py OUT_DIR grupo:artefato:versão [...]
Gera em OUT_DIR:
  jars/*.jar           classes de cada biblioteca (classes.jar do AAR + libs/*.jar)
  res/<nome>/          res/ de cada AAR que tem recursos
  manifests/<nome>.xml AndroidManifest.xml de cada AAR
  packages.txt         pacote Java de cada AAR com recursos (para o R.java)
  resolved.txt         lista final (versão mais alta vence, como o Gradle)
Cache dos downloads em OUT_DIR/cache (rodar de novo não baixa nada).
"""
import os, re, sys, urllib.request, zipfile, xml.etree.ElementTree as ET

REPOS = ["https://dl.google.com/dl/android/maven2", "https://repo1.maven.org/maven2"]
NS = "{http://maven.apache.org/POM/4.0.0}"
SKIP_SCOPES = {"test", "provided", "system", "import"}


def vkey(v):
    parts = re.split(r"[.\-]", v)
    out = []
    for p in parts:
        if p.isdigit():
            out.append((1, int(p), ""))
        else:  # alpha/beta/rc < final
            out.append((0, 0, p))
    return out


def fetch(cache, path):
    dst = os.path.join(cache, path.replace("/", "_"))
    if os.path.exists(dst):
        return open(dst, "rb").read() if os.path.getsize(dst) else None
    for r in REPOS:
        try:
            data = urllib.request.urlopen(f"{r}/{path}", timeout=60).read()
            open(dst, "wb").write(data)
            return data
        except Exception:
            continue
    open(dst, "wb").close()  # marca "não existe"
    return None


def gpath(g, a, v, ext):
    return f"{g.replace('.', '/')}/{a}/{v}/{a}-{v}.{ext}"


class Pom:
    def __init__(self, cache, g, a, v):
        data = fetch(cache, gpath(g, a, v, "pom"))
        if data is None:
            raise RuntimeError(f"POM não encontrado: {g}:{a}:{v}")
        root = ET.fromstring(data)
        self.props = {"project.version": v, "project.groupId": g, "version": v}
        self.managed = {}
        par = root.find(NS + "parent")
        if par is not None:
            pg, pa, pv = (par.findtext(NS + t) for t in ("groupId", "artifactId", "version"))
            try:
                pp = Pom(cache, pg, pa, pv)
                self.props.update({k: x for k, x in pp.props.items() if not k.startswith("project.")})
                self.managed.update(pp.managed)
            except RuntimeError:
                pass
        props = root.find(NS + "properties")
        if props is not None:
            for p in props:
                self.props[p.tag.replace(NS, "")] = (p.text or "").strip()
        self.packaging = root.findtext(NS + "packaging") or "jar"
        dm = root.find(NS + "dependencyManagement/" + NS + "dependencies")
        if dm is not None:
            for d in dm.findall(NS + "dependency"):
                k = (self.sub(d.findtext(NS + "groupId")), self.sub(d.findtext(NS + "artifactId")))
                ver = d.findtext(NS + "version")
                if ver:
                    self.managed[k] = self.clean(self.sub(ver))
        self.deps = []
        ds = root.find(NS + "dependencies")
        if ds is not None:
            for d in ds.findall(NS + "dependency"):
                scope = (d.findtext(NS + "scope") or "compile").strip()
                if scope in SKIP_SCOPES or (d.findtext(NS + "optional") or "").strip() == "true":
                    continue
                dg, da = self.sub(d.findtext(NS + "groupId")), self.sub(d.findtext(NS + "artifactId"))
                dv = d.findtext(NS + "version")
                dv = self.clean(self.sub(dv)) if dv else self.managed.get((dg, da))
                if dv:
                    self.deps.append((dg, da, dv))

    def sub(self, s):
        s = (s or "").strip()
        for _ in range(5):
            s2 = re.sub(r"\$\{([^}]+)\}", lambda m: self.props.get(m.group(1), m.group(0)), s)
            if s2 == s:
                break
            s = s2
        return s

    @staticmethod
    def clean(v):  # "[1.2.0]" / "[1.0,2.0)" -> menor limite explícito
        v = v.strip()
        if v[:1] in "[(":
            v = v.strip("[]()").split(",")[0].strip()
        return v


def main():
    out = sys.argv[1]
    roots = [c.split(":") for c in sys.argv[2:]]
    cache = os.path.join(out, "cache")
    os.makedirs(cache, exist_ok=True)
    best = {}  # (g,a) -> versão
    poms = {}
    queue = [tuple(r) for r in roots]
    while queue:  # iteração até fixar (versão mais alta de cada (g,a))
        g, a, v = queue.pop()
        cur = best.get((g, a))
        if cur and vkey(cur) >= vkey(v):
            continue
        best[(g, a)] = v
        p = poms.get((g, a, v)) or Pom(cache, g, a, v)
        poms[(g, a, v)] = p
        queue.extend(p.deps)
    # só o que é alcançável pela versão vencedora
    keep, stack = set(), [(r[0], r[1]) for r in roots]
    while stack:
        k = stack.pop()
        if k in keep:
            continue
        keep.add(k)
        p = poms[(k[0], k[1], best[k])]
        stack.extend((d[0], d[1]) for d in p.deps)
    for sub in ("jars", "res", "manifests"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    pk, lines = [], []
    for (g, a) in sorted(keep):
        v = best[(g, a)]
        p = poms[(g, a, v)]
        name = f"{g}.{a}"
        if p.packaging in ("pom", "bom"):
            continue
        aar = fetch(cache, gpath(g, a, v, "aar"))
        if aar:
            lines.append(f"{g}:{a}:{v} aar")
            zpath = os.path.join(cache, gpath(g, a, v, "aar").replace("/", "_"))
            with zipfile.ZipFile(zpath) as z:
                names = z.namelist()
                if "classes.jar" in names:
                    open(os.path.join(out, "jars", name + ".jar"), "wb").write(z.read("classes.jar"))
                for n in names:
                    if n.startswith("libs/") and n.endswith(".jar"):
                        open(os.path.join(out, "jars", name + "-" + os.path.basename(n)), "wb").write(z.read(n))
                man = z.read("AndroidManifest.xml")
                open(os.path.join(out, "manifests", name + ".xml"), "wb").write(man)
                res = [n for n in names if n.startswith("res/") and not n.endswith("/")]
                if res:
                    for n in res:
                        dst = os.path.join(out, "res", name, n[4:])
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        open(dst, "wb").write(z.read(n))
                    pkg = ET.fromstring(man).get("package")
                    if pkg:
                        pk.append(f"{name} {pkg}")
            continue
        jar = fetch(cache, gpath(g, a, v, "jar"))
        if jar:
            lines.append(f"{g}:{a}:{v} jar")
            open(os.path.join(out, "jars", name + ".jar"), "wb").write(jar)
        else:
            lines.append(f"{g}:{a}:{v} (sem binário)")
    open(os.path.join(out, "packages.txt"), "w").write("\n".join(pk) + "\n")
    open(os.path.join(out, "resolved.txt"), "w").write("\n".join(lines) + "\n")
    print(f"{len(lines)} bibliotecas, {len(pk)} com recursos")


if __name__ == "__main__":
    main()
