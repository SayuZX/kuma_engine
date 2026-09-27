# Optional encryption (authenticated)

Encryption is optional and modular: a payload is encrypted only when its header
`kHeaderFlagEncrypted` bit is set, and the resolver only decrypts when it holds a
key. Unencrypted payloads are unaffected.

## Primitive

ChaCha20-Poly1305 (AEAD) via the in-tree BoringSSL (`EVP_AEAD`), used on **both**
sides through one wrapper (`packed_asset_crypto.{h,cc}`): the `asset_encryptor`
build tool seals, the engine resolver opens. No cryptographic algorithm is
hand-rolled. ChaCha20-Poly1305 was chosen over AES-256-GCM because it needs no
AES hardware and BoringSSL ships a constant-time implementation.

## Pipeline order

Build: `plaintext → compress (codec) → encrypt (AEAD) → store`. Runtime:
`read → integrity (CRC32) → authenticate + decrypt → decompress → return`.
Encrypting after compressing is required — encrypted data does not compress.

## Block layout when encrypted

```
[ 12-byte nonce ][ ciphertext || 16-byte Poly1305 tag ]
```
`stored_size` covers all of it; `original_size` stays the decompressed size;
`codec` still describes the pre-encryption compression. The per-entry CRC32 is
computed over these stored (encrypted) bytes and catches corruption before a
decrypt is attempted; the AEAD tag catches tampering.

## Nonce

The encryptor derives the nonce as the first 12 bytes of
`SHA-256(key || stored_block)` and stores it in the block. This is deterministic
(reproducible builds) and content-sensitive, so the same key is never reused with
a different plaintext under the same asset — while identical content yields an
identical nonce, which is safe.

## Key handling and honest limits

The key is 32 bytes. In the engine it is assembled at runtime by XOR-ing two
separately-stored components (`kPackedKeyPartA`, `kPackedKeyPartB`) rather than
sitting as one plaintext array. This **only raises the effort of static
extraction** — a client-side key is not, and cannot be, truly secret; a
determined reverse-engineer can recover it. Encryption here is a
static-extraction hardening measure, not a DRM guarantee. The build tool takes
the key as `--key <64 hex>`; a real deployment must feed the same key to the
encryptor and the engine components.

## Verification (host, no device)

`tests/integration_roundtrip.sh` packs, encrypts with `asset_encryptor`, then:

- resolver **without** the key cannot read the encrypted payload;
- resolver **with** the key reads every asset byte-identically, including an
  encrypted-then-compressed asset (decrypt → inflate);

and the unit tests add wrong-key and tampered-ciphertext cases, both of which
fail authentication and return `nullptr`.

## Separation from integrity

The CRC32 (docs/integrity.md) is a non-cryptographic corruption check; the
Poly1305 tag is the authenticity check. They are distinct fields with distinct
meaning and are never conflated.
