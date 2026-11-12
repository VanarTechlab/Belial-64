/**
 * @file codec.h
 *
 * Interface of save game encryption algorithm.
 */
#pragma once

#include <cstddef>

namespace devilution {

std::size_t codec_decode(std::byte *pbSrcDst, std::size_t size, const char *pszPassword);
std::size_t codec_get_encoded_len(std::size_t dwSrcBytes);
void codec_encode(std::byte *pbSrcDst, std::size_t size, std::size_t size_64, const char *pszPassword);
#ifdef __N64__
// N64 v0.9.0: descifra solo el primer bloque de 64 bytes (para mirar la cabecera sin leer todo el archivo)
void codec_decode_first_block(std::byte *block64, const char *pszPassword);
#endif

} // namespace devilution
