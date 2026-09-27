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

## Authentication (not implemented here): AEAD

Tamper resistance requires a keyed MAC / authenticated encryption
(AES-256-GCM or ChaCha20-Poly1305 via the in-tree BoringSSL), which is the
separate, optional encryption milestone. Its authentication tag — not the CRC32
— is what proves a block was produced by the key holder. Because the key ships
in the client, even that only raises the cost of static extraction; it is not a
secret-keeping guarantee.

## Why kept apart

Mixing a non-cryptographic checksum with an authentication tag invites treating
one as the other. The format carries the CRC32 as an integrity field only; when
encryption lands, the AEAD tag will be a distinct field with distinct meaning.
