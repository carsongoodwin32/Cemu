#pragma once

#include <Metal/Metal.hpp>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
//#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

struct Uvec2 {
    uint32 x;
    uint32 y;
};

struct MtlPixelFormatInfo {
    MTL::PixelFormat pixelFormat;
    size_t bytesPerBlock;
    Uvec2 blockTexelSize = {1, 1};
};

const MtlPixelFormatInfo GetMtlPixelFormatInfo(Latte::E_GX2SURFFMT format, bool isDepth);

size_t GetMtlTextureBytesPerRow(Latte::E_GX2SURFFMT format, bool isDepth, uint32 width);

size_t GetMtlTextureBytesPerImage(Latte::E_GX2SURFFMT format, bool isDepth, uint32 height, size_t bytesPerRow);

MTL::PrimitiveType GetMtlPrimitiveType(LattePrimitiveMode mode);

MTL::VertexFormat GetMtlVertexFormat(uint8 format);

MTL::IndexType GetMtlIndexType(Renderer::INDEX_TYPE indexType);
