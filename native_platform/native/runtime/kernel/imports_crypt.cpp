// Xbox SHA-1 exports, including the guest-resident incremental context.
#include <algorithm>
#include <cstring>
#include <rex/hook.h>
#include "crypto/TinySHA1.hpp"
#include "crypto/sha256.h"
#include "kernel.h"
extern "C" {
#include "crypto/rijndael-alg-fst.h"
}

using namespace xn;

namespace {
// XECRYPT_SHA_STATE: byte count, five big-endian state words, 64 buffer bytes.
// Keep all state in guest memory so callers can copy or move a live context.
constexpr uint32_t kShaStateSize = 0x58;
constexpr uint32_t kShaBufferOffset = 0x18;

void LoadShaState(sha1::SHA1& sha, uint32_t context) {
  uint32_t words[5];
  for (unsigned i = 0; i < 5; ++i) words[i] = Load32(context + 4 + i * 4);
  sha.init(words, G(context + kShaBufferOffset), Load32(context));
}

void StoreShaWords(const sha1::SHA1& sha, uint32_t context) {
  for (unsigned i = 0; i < 5; ++i) Store32(context + 4 + i * 4, sha.getDigest()[i]);
}
}  // namespace

void XeCryptShaInit_entry(u32 context) {
  if (!context) return;
  std::memset(G(context), 0, kShaStateSize);
  const sha1::SHA1 sha;
  StoreShaWords(sha, context);
}
XN_IMPORT(__imp__XeCryptShaInit, XeCryptShaInit_entry)

void XeCryptShaUpdate_entry(u32 context, u32 data, u32 length) {
  if (!context || !length) return;
  sha1::SHA1 sha;
  LoadShaState(sha, context);
  sha.processBytes(G(data), length);
  StoreShaWords(sha, context);
  Store32(context, uint32_t(sha.getByteCount()));
  std::memcpy(G(context + kShaBufferOffset), sha.getBlock(), sha.getBlockByteIndex());
}
XN_IMPORT(__imp__XeCryptShaUpdate, XeCryptShaUpdate_entry)

void XeCryptShaFinal_entry(u32 context, u32 output, u32 output_size) {
  if (!context) return;
  sha1::SHA1 sha;
  LoadShaState(sha, context);
  uint8_t digest[20];
  sha.finalize(digest);
  if (output && output_size) std::memcpy(G(output), digest, std::min<uint32_t>(20, output_size));
  // Final publishes the completed state words without changing the input count.
  StoreShaWords(sha, context);
}
XN_IMPORT(__imp__XeCryptShaFinal, XeCryptShaFinal_entry)

void XeCryptSha_entry(u32 a, u32 a_size, u32 b, u32 b_size,
                     u32 c, u32 c_size, u32 output, u32 output_size) {
  sha1::SHA1 sha;
  if (a && a_size) sha.processBytes(G(a), a_size);
  if (b && b_size) sha.processBytes(G(b), b_size);
  if (c && c_size) sha.processBytes(G(c), c_size);
  uint8_t digest[20];
  sha.finalize(digest);
  if (output && output_size) std::memcpy(G(output), digest, std::min<uint32_t>(20, output_size));
}
XN_IMPORT(__imp__XeCryptSha, XeCryptSha_entry)

// XECRYPT_SHA256_STATE: byte count, eight BE words, 64 pending bytes.
// Preserve the guest representation across updates, including copies of a
// live context made by Forza's profile writer.
namespace {
sha256::SHA256 LoadSha256(u32 context) {
  sha256::SHA256 sha;
  for (unsigned i = 0; i < 8; ++i) sha.getHashValues()[i] = Load32(context + 4 + 4 * i);
  std::memcpy(sha.getBuffer(), G(context + 0x24), 64);
  sha.setTotalSize(Load32(context));
  return sha;
}
}

void XeCryptSha256Init_entry(u32 context) {
  if (!context) return;
  std::memset(G(context), 0, 0x64);
  sha256::SHA256 sha;
  for (unsigned i = 0; i < 8; ++i) Store32(context + 4 + 4 * i, sha.getHashValues()[i]);
}
XN_IMPORT(__imp__XeCryptSha256Init, XeCryptSha256Init_entry)

void XeCryptSha256Update_entry(u32 context, u32 data, u32 length) {
  if (!context || !length) return;
  auto sha = LoadSha256(context);
  sha.add(G(data), length);
  Store32(context, uint32_t(sha.getTotalSize()));
  for (unsigned i = 0; i < 8; ++i) Store32(context + 4 + 4 * i, sha.getHashValues()[i]);
  std::memcpy(G(context + 0x24), sha.getBuffer(), 64);
}
XN_IMPORT(__imp__XeCryptSha256Update, XeCryptSha256Update_entry)

void XeCryptSha256Final_entry(u32 context, u32 output, u32 size) {
  if (!context) return;
  auto sha = LoadSha256(context);
  uint8_t digest[32];
  sha.getHash(digest);
  if (output && size) std::memcpy(G(output), digest, std::min<u32>(32, size));
  std::memcpy(G(context + 0x24), digest, 32);
}
XN_IMPORT(__imp__XeCryptSha256Final, XeCryptSha256Final_entry)

namespace {
// Native profile encryption uses the same development console wrapping key as
// the previous runtime. Retail console keys are not used by this port.
constexpr uint8_t kProfileWrappingKey[16] = {};

void UnwrapProfileKey(u32 obscured, uint8_t key[16]) {
  uint32_t round_keys[44];
  int rounds = rijndaelKeySetupDec(round_keys, kProfileWrappingKey, 128);
  rijndaelDecrypt(round_keys, rounds, G(obscured), key);
}
}

u32 XeKeysObscureKey_entry(u32 input, u32 output) {
  if (!input || !output) return X_STATUS_INVALID_PARAMETER;
  uint32_t round_keys[44];
  int rounds = rijndaelKeySetupEnc(round_keys, kProfileWrappingKey, 128);
  uint8_t block[16];
  rijndaelEncrypt(round_keys, rounds, G(input), block);
  std::memcpy(G(output), block, 16);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__XeKeysObscureKey, XeKeysObscureKey_entry)

u32 XeKeysAesCbcUsingKey_entry(u32 obscured, u32 input, u32 length,
                              u32 output, u32 feed, u32 encrypt) {
  if (!obscured || !feed || (length && (!input || !output)) || length % 16)
    return X_STATUS_INVALID_PARAMETER;
  uint8_t key[16], chain[16], block[16], result[16];
  UnwrapProfileKey(obscured, key);
  std::memcpy(chain, G(feed), 16);
  uint32_t round_keys[44];
  int rounds = encrypt ? rijndaelKeySetupEnc(round_keys, key, 128)
                       : rijndaelKeySetupDec(round_keys, key, 128);
  for (u32 offset = 0; offset < length; offset += 16) {
    std::memcpy(block, G(input + offset), 16);
    if (encrypt) {
      for (unsigned i = 0; i < 16; ++i) block[i] ^= chain[i];
      rijndaelEncrypt(round_keys, rounds, block, result);
      std::memcpy(chain, result, 16);
    } else {
      rijndaelDecrypt(round_keys, rounds, block, result);
      for (unsigned i = 0; i < 16; ++i) result[i] ^= chain[i];
      std::memcpy(chain, block, 16);
    }
    std::memcpy(G(output + offset), result, 16);
  }
  std::memcpy(G(feed), chain, 16);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__XeKeysAesCbcUsingKey, XeKeysAesCbcUsingKey_entry)
