#!/usr/bin/env python3
"""Mescla os AndroidManifest.xml das bibliotecas (AARs) no manifest do app — o "manifest merger" do Gradle, simplificado.

Uso: python3 tools/merge_manifest.py BASE.xml PACOTE SAIDA.xml lib1.xml lib2.xml ...
- Copia <uses-permission>, <permission>, <queries> e os filhos de <application> (activity, service, provider,
  receiver, meta-data, property, uses-library) de cada biblioteca, sem repetir (chave = tag + android:name).
- Componentes repetidos (ex.: androidx.startup.InitializationProvider) têm os filhos (meta-data) juntados.
- tools:node="remove" remove o elemento de todas as fontes; demais atributos tools:* são descartados.
- ${applicationId} vira PACOTE. <uses-sdk> das bibliotecas é ignorado (o build define min/target).
"""
import sys, xml.etree.ElementTree as ET

A = "{http://schemas.android.com/apk/res/android}"
T = "{http://schemas.android.com/tools}"
ET.register_namespace("android", A[1:-1])
ET.register_namespace("tools", T[1:-1])
TOP = ("uses-permission", "uses-permission-sdk-23", "permission")
APP = ("activity", "activity-alias", "service", "provider", "receiver", "meta-data", "property", "uses-library")


def key(e):
    return (e.tag, e.get(A + "name"))


def strip_tools(e, pkg):
    for el in e.iter():
        for k in list(el.attrib):
            if k.startswith(T):
                del el.attrib[k]
            elif "${applicationId}" in el.attrib[k]:
                el.attrib[k] = el.attrib[k].replace("${applicationId}", pkg)


def main():
    base, pkg, out, libs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    tree = ET.parse(base)
    man = tree.getroot()
    app = man.find("application")
    removed = set()
    top = {key(e): e for e in man if e.tag in TOP}
    comp = {key(e): e for e in app if e.tag in APP}
    queries = man.find("queries")
    for lib in libs:
        lm = ET.parse(lib).getroot()
        for e in list(lm):
            if e.get(T + "node") == "remove":
                removed.add(key(e)); continue
            if e.tag in TOP and key(e) not in top:
                strip_tools(e, pkg); top[key(e)] = e; man.insert(list(man).index(app), e)
            elif e.tag == "queries":
                if queries is None:
                    queries = ET.Element("queries"); man.insert(list(man).index(app), queries)
                for q in e:
                    strip_tools(q, pkg); queries.append(q)
        la = lm.find("application")
        if la is None:
            continue
        for e in list(la):
            if e.tag not in APP:
                continue
            if e.get(T + "node") == "remove":
                removed.add(key(e)); continue
            strip_tools(e, pkg)
            k = key(e)
            if k in comp:  # já existe: junta os filhos (meta-data do InitializationProvider etc.)
                have = {key(c) for c in comp[k]}
                for c in e:
                    if key(c) not in have:
                        comp[k].append(c)
                continue
            comp[k] = e
            app.append(e)
    for parent in (man, app):
        for e in list(parent):
            if key(e) in removed:
                parent.remove(e)
    strip_tools(man, pkg)
    ET.indent(tree, "    ")
    tree.write(out, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    main()
