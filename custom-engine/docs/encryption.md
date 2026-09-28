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

The existing test encryptor derives the nonce as the first 12 bytes of
`SHA-256(key || stored_block)` and stores it in the block. This makes builds
reproducible but leaks equality of identical stored blocks and is not a formal
misuse-resistant AEAD construction. Revisit nonce allocation before deploying
this optional encryption path at scale.

## Key handling and honest limits

The key is 32 bytes and is passed to the resolver by an external key provider.
The Android shell holder no longer contains the former demo XOR key. Hardened
Android builds currently provide a public signing key only, so encrypted blocks
cannot be opened there until a deployment supplies an explicit key provider.
The build tool takes the AEAD key as `--key <64 hex>` for host testing. A fully
offline client-side key cannot be truly secret against a determined device
owner. See `signed-format-v3.md` for the shipping offline integrity path.

## Verification (host, no device)

`tests/integration_roundtrip.sh` packs, encrypts with `asset_encryptor`, then:

- resolver **without** the key cannot read the encrypted payload;
- resolver **with** the key reads every asset byte-identically, including an
  encrypted-then-compressed asset (decrypt → inflate);

and the unit tests add wrong-key and tampered-ciphertext cases, both of which
fail authentication and return `nullptr`.

## Separation from integrity

The CRC32 (docs/integrity.md) is a non-cryptographic corruption check; the
Poly1305 tag authenticates an encrypted block. The signed v3 format separately
authenticates metadata and stored blocks without decryption.
