"""Tizen .wgt paketleri icin W3C Widget Digital Signature ureticisi.

XML kutuphanesi kullanmadan, belgeyi dogrudan kanonik (C14N) bicimde yazar:
bos ogeler acik/kapali etiket ciftiyle, satir sonlari yalnizca \\n. Boylece
imzalanan baytlar, TV'nin kanoniklestirdiginde elde edecegi baytlarla aynidir.
"""
import base64
import hashlib
import os
import subprocess
import urllib.parse

DS = "http://www.w3.org/2000/09/xmldsig#"
SHA256 = "http://www.w3.org/2001/04/xmlenc#sha256"
# Tizen Studio'nun urettigi her imzada ayni cikan #prop ozetleri (kanoniklestirme kontrolu icin).
KNOWN_PROP = {"author": "lpo8tUDs054eLlBQXiDPVDVKfw30ZZdtkRs1jd7H5K8=",
              "distributor": "u/jU3U4Zm5ihTMSjKGlGYbWzDfRkGphPPHx3gJIYEJ4="}


def _b64(raw):
    return base64.b64encode(raw).decode()


def _wrap(s, n=76):
    return "\n".join(s[i:i + n] for i in range(0, len(s), n))


def _prop_object(sig_id, role):
    return (
        '<Object Id="prop"><SignatureProperties xmlns:dsp="http://www.w3.org/2009/xmldsig-properties">'
        '<SignatureProperty Id="profile" Target="#{id}"><dsp:Profile URI="http://www.w3.org/ns/widgets-digsig#profile"></dsp:Profile></SignatureProperty>'
        '<SignatureProperty Id="role" Target="#{id}"><dsp:Role URI="http://www.w3.org/ns/widgets-digsig#role-{role}"></dsp:Role></SignatureProperty>'
        '<SignatureProperty Id="identifier" Target="#{id}"><dsp:Identifier></dsp:Identifier></SignatureProperty>'
        '</SignatureProperties></Object>'
    ).format(id=sig_id, role=role)


def prop_digest(sig_id, role):
    canon = _prop_object(sig_id, role).replace('<Object Id="prop">', '<Object xmlns="%s" Id="prop">' % DS, 1)
    return _b64(hashlib.sha256(canon.encode()).digest())


def make_signature(root, files, sig_id, role, key_pem, certs_der):
    refs = []
    for name in files:
        digest = hashlib.sha256(open(os.path.join(root, name), "rb").read()).digest()
        refs.append('<Reference URI="%s">\n<DigestMethod Algorithm="%s"></DigestMethod>\n'
                    '<DigestValue>%s</DigestValue>\n</Reference>\n'
                    % (urllib.parse.quote(name), SHA256, _b64(digest)))
    refs.append('<Reference URI="#prop">\n<Transforms>\n'
                '<Transform Algorithm="http://www.w3.org/2006/12/xml-c14n11"></Transform>\n</Transforms>\n'
                '<DigestMethod Algorithm="%s"></DigestMethod>\n<DigestValue>%s</DigestValue>\n</Reference>\n'
                % (SHA256, prop_digest(sig_id, role)))
    body = ('\n<CanonicalizationMethod Algorithm="http://www.w3.org/2001/10/xml-exc-c14n#"></CanonicalizationMethod>\n'
            '<SignatureMethod Algorithm="http://www.w3.org/2001/04/xmldsig-more#rsa-sha256"></SignatureMethod>\n'
            + "".join(refs))
    canon = '<SignedInfo xmlns="%s">%s</SignedInfo>' % (DS, body)
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key_pem],
                         input=canon.encode(), stdout=subprocess.PIPE, check=True).stdout
    certs = "".join("<X509Certificate>\n%s\n</X509Certificate>\n" % _wrap(_b64(c)) for c in certs_der)
    return ('<Signature xmlns="%s" Id="%s">\n<SignedInfo>%s</SignedInfo>\n'
            '<SignatureValue>\n%s\n</SignatureValue>\n<KeyInfo>\n<X509Data>\n%s</X509Data>\n</KeyInfo>\n%s\n</Signature>'
            % (DS, sig_id, body, _wrap(_b64(sig)), certs, _prop_object(sig_id, role)))


def sign_dir(root, author_key, author_chain, dist_key, dist_chain):
    """root icindeki dosyalari imzalar; author-signature.xml ve signature1.xml yazar."""
    files = []
    for base, _dirs, names in os.walk(root):
        for n in names:
            rel = os.path.relpath(os.path.join(base, n), root).replace(os.sep, "/")
            if rel not in ("author-signature.xml", "signature1.xml"):
                files.append(rel)
    files.sort()
    with open(os.path.join(root, "author-signature.xml"), "w", newline="\n") as f:
        f.write(make_signature(root, files, "AuthorSignature", "author", author_key, author_chain))
    with open(os.path.join(root, "signature1.xml"), "w", newline="\n") as f:
        f.write(make_signature(root, files + ["author-signature.xml"], "DistributorSignature",
                               "distributor", dist_key, dist_chain))
    return files
