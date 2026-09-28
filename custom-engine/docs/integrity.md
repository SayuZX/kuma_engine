# Integrity vs authentication

These are deliberately separate concerns and are not conflated.

## Integrity (implemented): per-entry CRC32

- **What**: a CRC32 of each entry's *stored* bytes (post-compression,
  pre-decompression) written into the entry's last 4 bytes (offset 28). A header
  flag bit (`kHeaderFlagEntryCrc32`, header `flags` offset 6 bit0) marks that the
  checksums are present.
- **Producer**: `flutter_asset_packer` computes it with the standard IEEE CRC32
  (poly `0xEDB88320`, init/final `0xffffffff`), the same algorithm zlib's
  `crc32()` uses.
- **Consumer**: the engine resolver reads the flag at construction. When set,
  `GetAsMapping` recomputes `crc32(0, block, stored_size)` (zlib) and compares to
  the stored checksum before decoding; a mismatch returns `nullptr`.
  `VerifyIntegrity()` runs the same check across every entry for a proactive
  full-payload validation (used by tests and tooling).
- **Cross-language equivalence** is proven by the integration test: the resolver
  rejects any block whose zlib-CRC32 differs from the packer's Dart-CRC32, so a
  passing round-trip demonstrates the two implementations agree.
- **Purpose and limits**: CRC32 detects *accidental* corruption (bad flash,
  truncated download, packaging bugs). It is **not** tamper-resistant: an
  attacker who edits a block can recompute the CRC. Do not treat a valid CRC as
  proof of authenticity.

## Authentication (implemented in v3): signed index and block digests

Format v3 signs the header, index, and per-block SHA-256/128 digests with
Ed25519. The engine carries the public verification key and verifies each block
on demand. The private key remains with the builder, so a modified block cannot
be accepted by an unchanged engine without forging the signature or digest.
This proves payload origin relative to the compiled public key; it does not
conceal raw bytes. See `signed-format-v3.md`.

The optional ChaCha20-Poly1305 codec has a distinct authentication tag for
encrypted blocks. Offline client-side key storage remains extractable and is
currently not wired into the hardened Android build.

## Why kept apart

The v2 CRC32 is an accidental-corruption check. The v3 signature/digest chain
authenticates a payload against a compiled public key. The optional AEAD tag
authenticates an encrypted block against a symmetric key. These checks answer
different questions and have separate fields and flags.
