#!/usr/bin/env python3
"""Ajudante de PUSH do servidor do Isolated (Firebase Cloud Messaging, API HTTP v1). O servidor (C++, sem HTTPS) roda este script com
--fcm-cmd e escreve no stdin 1 JSON por linha: {"token","title","body","action"}. Só biblioteca padrão + o comando `openssl` (assinatura RS256).

Credencial: a CONTA DE SERVIÇO do projeto Firebase (console do Firebase > Configurações do projeto > Contas de serviço > Gerar nova chave
privada), salva em /opt/isolated/data/fcm_service_account.json (dono isolated, 600). Outro caminho: variável FCM_SA. Sem o arquivo: não envia
(registra no log e sai com código 2). FCM_DRYRUN=1: só imprime o que mandaria (testes)."""
import base64, json, os, subprocess, sys, tempfile, time, urllib.error, urllib.request

SA = os.environ.get("FCM_SA", "/opt/isolated/data/fcm_service_account.json")
CACHE = os.environ.get("FCM_TOKEN_CACHE", os.path.join(os.path.dirname(SA), "fcm_oauth_cache.json"))
DRY = os.environ.get("FCM_DRYRUN") == "1"


def b64u(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def oauth_token(sa):
    try:
        c = json.load(open(CACHE))
        if c.get("exp", 0) > time.time() + 120 and c.get("email") == sa["client_email"]:
            return c["token"]
    except Exception:
        pass
    now = int(time.time())
    head = b64u(json.dumps({"alg": "RS256", "typ": "JWT"}).encode())
    claims = b64u(json.dumps({"iss": sa["client_email"], "scope": "https://www.googleapis.com/auth/firebase.messaging",
                              "aud": "https://oauth2.googleapis.com/token", "iat": now, "exp": now + 3600}).encode())
    msg = (head + "." + claims).encode()
    with tempfile.NamedTemporaryFile("w", delete=False) as kf:
        os.chmod(kf.name, 0o600)
        kf.write(sa["private_key"])
    try:
        sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", kf.name], input=msg, capture_output=True, check=True).stdout
    finally:
        os.unlink(kf.name)
    body = ("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + head + "." + claims + "." + b64u(sig)).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body, headers={"Content-Type": "application/x-www-form-urlencoded"})
    r = json.load(urllib.request.urlopen(req, timeout=15))
    tok = r["access_token"]
    try:
        with open(CACHE + ".tmp", "w") as f:
            json.dump({"token": tok, "exp": now + int(r.get("expires_in", 3600)), "email": sa["client_email"]}, f)
        os.chmod(CACHE + ".tmp", 0o600)
        os.replace(CACHE + ".tmp", CACHE)
    except Exception:
        pass
    return tok


def message(j):
    title, body, action = j.get("title", "Isolated"), j.get("body", ""), j.get("action", "")
    return {"message": {
        "token": j["token"],
        "notification": {"title": title, "body": body},
        "data": {"title": title, "body": body, "action": action},
        "android": {"priority": "high", "notification": {"channel_id": "tripulacao", "tag": "tripulacao"}},
    }}


def main():
    jobs = []
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            j = json.loads(line)
            if j.get("token"):
                jobs.append(j)
        except Exception as e:
            print("[fcm] linha invalida:", e, file=sys.stderr)
    if not jobs:
        return 0
    if DRY:
        for j in jobs:
            print("[fcm] (teste) enviaria:", json.dumps(message(j), ensure_ascii=False))
        return 0
    try:
        sa = json.load(open(SA))
    except Exception as e:
        print("[fcm] sem conta de servico em %s (%s): push desligado" % (SA, e), file=sys.stderr)
        return 2
    try:
        tok = oauth_token(sa)
    except Exception as e:
        print("[fcm] ERRO ao pegar o token OAuth:", e, file=sys.stderr)
        return 3
    url = "https://fcm.googleapis.com/v1/projects/%s/messages:send" % sa["project_id"]
    ok = bad = 0
    for j in jobs:
        req = urllib.request.Request(url, data=json.dumps(message(j)).encode(), headers={"Authorization": "Bearer " + tok, "Content-Type": "application/json"})
        try:
            urllib.request.urlopen(req, timeout=15).read()
            ok += 1
        except urllib.error.HTTPError as e:
            bad += 1
            print("[fcm] ERRO %d para ...%s: %s" % (e.code, j["token"][-8:], e.read()[:200].decode(errors="replace")), file=sys.stderr)
        except Exception as e:
            bad += 1
            print("[fcm] ERRO para ...%s: %s" % (j["token"][-8:], e), file=sys.stderr)
    print("[fcm] enviadas %d, falharam %d" % (ok, bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
