#!/usr/bin/env python3
"""Gera uma chave RSA de TESTE (openssl), o arquivo JWK correspondente e idTokens no formato do Firebase assinados com ela.
Só para testes do servidor (nettest persist / authtest): o servidor de teste aponta --jwk para o JWK gerado aqui.
Uso: make_test_jwt.py DIR PROJETO UID [UID...]   -> DIR/test.jwk, DIR/token_<uid>.txt (+ DIR/token_<uid>_expirado.txt, DIR/token_<uid>_outroaud.txt)"""
import base64, json, os, subprocess, sys, time

def b64u(b): return base64.urlsafe_b64encode(b).rstrip(b'=').decode()
d, proj, uids = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(d, exist_ok=True)
key = os.path.join(d, 'test_key.pem')
if not os.path.exists(key):
    subprocess.run(['openssl', 'genrsa', '-out', key, '2048'], check=True, capture_output=True)
mod = subprocess.run(['openssl', 'rsa', '-in', key, '-noout', '-modulus'], check=True, capture_output=True, text=True).stdout.strip().split('=')[1]
kid = 'teste-' + mod[:12].lower()
jwk = {'keys': [{'kty': 'RSA', 'alg': 'RS256', 'use': 'sig', 'kid': kid, 'n': b64u(bytes.fromhex(mod)), 'e': 'AQAB'}]}
open(os.path.join(d, 'test.jwk'), 'w').write(json.dumps(jwk, indent=1))
def tok(uid, exp_off=3600, aud=proj):
    now = int(time.time())
    h = b64u(json.dumps({'alg': 'RS256', 'kid': kid, 'typ': 'JWT'}).encode())
    p = b64u(json.dumps({'iss': 'https://securetoken.google.com/' + proj, 'aud': aud, 'auth_time': now - 10, 'user_id': uid, 'sub': uid,
                         'iat': now - 10, 'exp': now + exp_off, 'firebase': {'identities': {}, 'sign_in_provider': 'anonymous'}}).encode())
    msg = (h + '.' + p).encode()
    sig = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', key], input=msg, check=True, capture_output=True).stdout
    return h + '.' + p + '.' + b64u(sig)
for u in uids:
    open(os.path.join(d, 'token_%s.txt' % u), 'w').write(tok(u))
    open(os.path.join(d, 'token_%s_expirado.txt' % u), 'w').write(tok(u, exp_off=-3600))
    open(os.path.join(d, 'token_%s_outroaud.txt' % u), 'w').write(tok(u, aud='outro-projeto'))
print('ok', d, kid)
