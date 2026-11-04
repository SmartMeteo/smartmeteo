#!/usr/bin/env bash
# Sign a built image for release, and prove the signature before handing it to anything.
#
# Signing is deliberately NOT part of the build. Build-time signing wants the key path inside
# sdkconfig, and the key lives outside the repository on purpose — so that path would be absolute
# and the project would stop building on any other machine, which is exactly the trap the
# ArduinoJson include path was. Builds stay key-free and reproducible; this signs.
#
# NOTE on reproducibility: the UNSIGNED image is byte-identical across clean builds and that is the
# property to check. The SIGNED one is not, and cannot be — ECDSA draws a random nonce for every
# signature, so signing the same image twice gives two different, equally valid files. Compare
# unsigned artefacts; verify signed ones.
set -euo pipefail

KEY="${SMARTMETEO_SIGNING_KEY:-$HOME/.smartmeteo-signing/smartmeteo_sbv2_key1.pem}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN="${1:-$HERE/build/smart-meteo.bin}"
OUT="${2:-$HERE/build/smart-meteo-signed.bin}"

[ -f "$KEY" ] || { echo "signing key not found: $KEY" >&2
                   echo "set SMARTMETEO_SIGNING_KEY, or see README.md, Signing and flashing" >&2; exit 1; }
[ -f "$IN" ]  || { echo "no image to sign: $IN (run idf.py build first)" >&2; exit 1; }

echo "unsigned : $(shasum -a 256 "$IN" | cut -d' ' -f1)  <- this is the reproducible one"
espsecure.py sign_data --version 2 --keyfile "$KEY" --output "$OUT" "$IN" >/dev/null

# Refuse to hand over an image whose signature has not been checked. It costs a second and it is
# the only step that can catch a wrong key, a truncated write or a corrupted source image.
if espsecure.py verify_signature --version 2 --keyfile "$KEY" "$OUT" 2>&1 | grep -q "verification successful"; then
    echo "signed   : $(shasum -a 256 "$OUT" | cut -d' ' -f1)"
    echo "verified against $(basename "$KEY")"
else
    echo "SIGNATURE DID NOT VERIFY — not releasing this file" >&2
    rm -f "$OUT"; exit 1
fi
