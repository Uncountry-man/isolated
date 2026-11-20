#!/usr/bin/env python3
"""google-services.json (console do Firebase) -> res/values/firebase.xml, o que o plugin do Gradle faria (este projeto não usa Gradle).
Uso: python3 tools/google_services.py android/google-services.json PACOTE SAIDA.xml
Gera os recursos que o FirebaseApp lê sozinho (google_app_id, google_api_key, gcm_defaultSenderId, project_id, google_storage_bucket,
firebase_database_url) e o default_web_client_id (ID do cliente OAuth "web", usado no login com Google).
Sai com código 2 se o JSON não tem um app Android com esse pacote (o build.sh então compila sem Firebase)."""
import json, sys
from xml.sax.saxutils import escape

src, pkg, out = sys.argv[1], sys.argv[2], sys.argv[3]
j = json.load(open(src, encoding="utf-8"))
pi = j.get("project_info", {})
client = next((c for c in j.get("client", []) if c.get("client_info", {}).get("android_client_info", {}).get("package_name") == pkg), None)
if client is None:
    print(f"google-services.json: nenhum app Android com o pacote {pkg} (Firebase desligado neste build)", file=sys.stderr)
    sys.exit(2)
web = ""
for oc in client.get("oauth_client", []) + client.get("services", {}).get("appinvite_service", {}).get("other_platform_oauth_client", []):
    if oc.get("client_type") == 3:
        web = oc.get("client_id", ""); break
vals = {
    "google_app_id": client["client_info"]["mobilesdk_app_id"],
    "gcm_defaultSenderId": pi.get("project_number", ""),
    "project_id": pi.get("project_id", ""),
    "google_storage_bucket": pi.get("storage_bucket", ""),
    "firebase_database_url": pi.get("firebase_url", ""),
    "google_api_key": (client.get("api_key") or [{}])[0].get("current_key", ""),
    "google_crash_reporting_api_key": (client.get("api_key") or [{}])[0].get("current_key", ""),
}
if web: vals["default_web_client_id"] = web
else: print("AVISO: google-services.json sem cliente OAuth web (client_type 3): ative o login com Google no console e baixe o JSON de novo", file=sys.stderr)
import os
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w", encoding="utf-8") as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n')
    for k, v in vals.items():
        if v: f.write(f'    <string name="{k}" translatable="false">{escape(str(v))}</string>\n')
    f.write("</resources>\n")
print(f"Firebase: {len([v for v in vals.values() if v])} recursos gerados para {pkg}" + ("" if web else " (SEM login Google)"))
