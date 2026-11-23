// NieR: an empty Renut shader cache (no translated game shader), for building Renut before NieR's shaders are
// translated (NIER_RENUT_SHADER_CACHE_CPP unset). The game's translated shaders are game data: their cache is made
// on the player's machine from their own copy of the game, never committed.
#include "shader_cache.h"

ShaderCacheEntry g_shaderCacheEntries[1] = {};
const size_t g_shaderCacheEntryCount = 0;
ShaderVertexInput g_shaderVertexInputs[1] = {};
ShaderBaseKey g_shaderBaseKeys[1] = {};
const size_t g_shaderBaseKeyCount = 0;
ShaderLiteral g_shaderLiterals[1] = {};
const uint32_t g_shaderCacheLayout = kShaderCacheLayout;

const uint8_t g_compressedDxilCache[1] = {};
const size_t g_dxilCacheCompressedSize = 0;
const size_t g_dxilCacheDecompressedSize = 0;

extern "C" const uint8_t g_compressedSpirvCache[1] = {};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;
