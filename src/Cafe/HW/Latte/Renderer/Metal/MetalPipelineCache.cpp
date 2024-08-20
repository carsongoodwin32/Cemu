#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Foundation/NSObject.hpp"
#include "HW/Latte/Renderer/Metal/CachedFBOMtl.h"
#include "HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"

#include "HW/Latte/Core/FetchShader.h"
#include "HW/Latte/ISA/RegDefines.h"
#include "Metal/MTLDevice.hpp"
#include "Metal/MTLRenderPipeline.hpp"
#include "config/ActiveSettings.h"

#define INVALID_TITLE_ID 0xFFFFFFFFFFFFFFFF

uint64 s_cacheTitleId = INVALID_TITLE_ID;

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

template<typename T>
void SetFragmentState(T* desc, class CachedFBOMtl* activeFBO, const LatteContextRegister& lcr)
{
    // Color attachments
	const Latte::LATTE_CB_COLOR_CONTROL& colorControlReg = lcr.CB_COLOR_CONTROL;
	uint32 blendEnableMask = colorControlReg.get_BLEND_MASK();
	uint32 renderTargetMask = lcr.CB_TARGET_MASK.get_MASK();
	for (uint8 i = 0; i < 8; i++)
	{
	    const auto& colorBuffer = activeFBO->colorBuffer[i];
		auto texture = static_cast<LatteTextureViewMtl*>(colorBuffer.texture);
		if (!texture)
		{
		    continue;
		}
		auto colorAttachment = desc->colorAttachments()->object(i);
		colorAttachment->setPixelFormat(texture->GetRGBAView()->pixelFormat());
		colorAttachment->setWriteMask(GetMtlColorWriteMask((renderTargetMask >> (i * 4)) & 0xF));

		// Blending
		bool blendEnabled = ((blendEnableMask & (1 << i))) != 0;
		// Only float data type is blendable
		if (blendEnabled && GetMtlPixelFormatInfo(texture->format, false).dataType == MetalDataType::FLOAT)
		{
       		colorAttachment->setBlendingEnabled(true);

       		const auto& blendControlReg = lcr.CB_BLENDN_CONTROL[i];

       		auto rgbBlendOp = GetMtlBlendOp(blendControlReg.get_COLOR_COMB_FCN());
       		auto srcRgbBlendFactor = GetMtlBlendFactor(blendControlReg.get_COLOR_SRCBLEND());
       		auto dstRgbBlendFactor = GetMtlBlendFactor(blendControlReg.get_COLOR_DSTBLEND());

       		colorAttachment->setRgbBlendOperation(rgbBlendOp);
       		colorAttachment->setSourceRGBBlendFactor(srcRgbBlendFactor);
       		colorAttachment->setDestinationRGBBlendFactor(dstRgbBlendFactor);
       		if (blendControlReg.get_SEPARATE_ALPHA_BLEND())
       		{
       			colorAttachment->setAlphaBlendOperation(GetMtlBlendOp(blendControlReg.get_ALPHA_COMB_FCN()));
         		    colorAttachment->setSourceAlphaBlendFactor(GetMtlBlendFactor(blendControlReg.get_ALPHA_SRCBLEND()));
         		    colorAttachment->setDestinationAlphaBlendFactor(GetMtlBlendFactor(blendControlReg.get_ALPHA_DSTBLEND()));
       		}
       		else
       		{
           		colorAttachment->setAlphaBlendOperation(rgbBlendOp);
           		colorAttachment->setSourceAlphaBlendFactor(srcRgbBlendFactor);
           		colorAttachment->setDestinationAlphaBlendFactor(dstRgbBlendFactor);
       		}
		}
	}

	// Depth stencil attachment
	if (activeFBO->depthBuffer.texture)
	{
	    auto texture = static_cast<LatteTextureViewMtl*>(activeFBO->depthBuffer.texture);
           desc->setDepthAttachmentPixelFormat(texture->GetRGBAView()->pixelFormat());
           if (activeFBO->depthBuffer.hasStencil)
           {
               desc->setStencilAttachmentPixelFormat(texture->GetRGBAView()->pixelFormat());
           }
	}
}

void MetalPipelineCache::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
    s_cacheTitleId = cacheTitleId;
}

void MetalPipelineCache::ShaderCacheLoading_end()
{
}

void MetalPipelineCache::ShaderCacheLoading_Close()
{
    g_compiled_shaders_total = 0;
    g_compiled_shaders_async = 0;
}

MetalPipelineCache::~MetalPipelineCache()
{
    for (auto& pair : m_pipelineCache)
    {
        pair.second->release();
    }
    m_pipelineCache.clear();

    NS::Error* error = nullptr;
    m_binaryArchive->serializeToURL(m_binaryArchiveURL, &error);
    if (error)
    {
        debug_printf("failed to serialize binary archive: %s\n", error->localizedDescription()->utf8String());
        error->release();
    }
    m_binaryArchive->release();

    m_binaryArchiveURL->release();
}

MTL::RenderPipelineState* MetalPipelineCache::GetRenderPipelineState(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* pixelShader, CachedFBOMtl* activeFBO, const LatteContextRegister& lcr)
{
    uint64 stateHash = CalculateRenderPipelineHash(fetchShader, vertexShader, pixelShader, activeFBO, lcr);
    auto& pipeline = m_pipelineCache[stateHash];
    if (pipeline)
        return pipeline;

	// Vertex descriptor
	MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::alloc()->init();
	for (auto& bufferGroup : fetchShader->bufferGroups)
	{
		std::optional<LatteConst::VertexFetchType2> fetchType;

		for (sint32 j = 0; j < bufferGroup.attribCount; ++j)
		{
			auto& attr = bufferGroup.attrib[j];

			uint32 semanticId = vertexShader->resourceMapping.attributeMapping[attr.semanticId];
			if (semanticId == (uint32)-1)
				continue; // attribute not used?

			auto attribute = vertexDescriptor->attributes()->object(semanticId);
			attribute->setOffset(attr.offset);
			attribute->setBufferIndex(GET_MTL_VERTEX_BUFFER_INDEX(attr.attributeBufferIndex));
			attribute->setFormat(GetMtlVertexFormat(attr.format));

			if (fetchType.has_value())
				cemu_assert_debug(fetchType == attr.fetchType);
			else
				fetchType = attr.fetchType;

			if (attr.fetchType == LatteConst::INSTANCE_DATA)
			{
				cemu_assert_debug(attr.aluDivisor == 1); // other divisor not yet supported
			}
		}

		uint32 bufferIndex = bufferGroup.attributeBufferIndex;
		uint32 bufferBaseRegisterIndex = mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7;
		uint32 bufferStride = (lcr.GetRawView()[bufferBaseRegisterIndex + 2] >> 11) & 0xFFFF;
		bufferStride = Align(bufferStride, 4);

		// HACK
		if (bufferStride == 0)
		{
		    debug_printf("vertex buffer %u has a vertex stride of 0 bytes, using 4 bytes instead\n", bufferIndex);
			bufferStride = 4;
		}

		auto layout = vertexDescriptor->layouts()->object(GET_MTL_VERTEX_BUFFER_INDEX(bufferIndex));
		layout->setStride(bufferStride);
		if (!fetchType.has_value() || fetchType == LatteConst::VertexFetchType2::VERTEX_DATA)
			layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
		else if (fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA)
			layout->setStepFunction(MTL::VertexStepFunctionPerInstance);
		else
		{
		    debug_printf("unimplemented vertex fetch type %u\n", (uint32)fetchType.value());
			cemu_assert(false);
		}
	}

	auto mtlVertexShader = static_cast<RendererShaderMtl*>(vertexShader->shader);
	auto mtlPixelShader = static_cast<RendererShaderMtl*>(pixelShader->shader);
	mtlVertexShader->CompileVertexFunction();
	mtlPixelShader->CompileFragmentFunction(activeFBO);

	// Render pipeline state
	MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
	desc->setVertexFunction(mtlVertexShader->GetFunction());
	desc->setFragmentFunction(mtlPixelShader->GetFunction());
	// TODO: don't always set the vertex descriptor?
	desc->setVertexDescriptor(vertexDescriptor);

	SetFragmentState(desc, activeFBO, lcr);

	TryLoadBinaryArchive();

	// Load binary
    if (m_binaryArchive)
    {
        NS::Object* binArchives[] = {m_binaryArchive};
        auto binaryArchives = NS::Array::alloc()->init(binArchives, 1);
        desc->setBinaryArchives(binaryArchives);
        binaryArchives->release();
    }

    NS::Error* error = nullptr;
#ifdef CEMU_DEBUG_ASSERT
    desc->setLabel(GetLabel("Cached render pipeline state", desc));
#endif
	pipeline = m_mtlr->GetDevice()->newRenderPipelineState(desc, MTL::PipelineOptionFailOnBinaryArchiveMiss, nullptr, &error);

	//static uint32 oldPipelineCount = 0;
	//static uint32 newPipelineCount = 0;

	// Pipeline wasn't found in the binary archive, we need to compile it
	if (error)
	{
		desc->setBinaryArchives(nullptr);

        error->release();
        error = nullptr;
#ifdef CEMU_DEBUG_ASSERT
        desc->setLabel(GetLabel("New render pipeline state", desc));
#endif
	    pipeline = m_mtlr->GetDevice()->newRenderPipelineState(desc, &error);
		if (error)
		{
		    debug_printf("error creating render pipeline state: %s\n", error->localizedDescription()->utf8String());
			error->release();
			return nullptr;
		}
		else
		{
		    // Save binary
			if (m_binaryArchive)
			{
                NS::Error* error = nullptr;
                m_binaryArchive->addRenderPipelineFunctions(desc, &error);
                if (error)
                {
                    debug_printf("error saving render pipeline functions: %s\n", error->localizedDescription()->utf8String());
                    error->release();
                }
			}
		}

		//newPipelineCount++;
	}
	//else
    //{
    //    oldPipelineCount++;
    //}
    //debug_printf("%u pipelines were found in the binary archive, %u new were created\n", oldPipelineCount, newPipelineCount);
	desc->release();
	vertexDescriptor->release();

	return pipeline;
}

MTL::RenderPipelineState* MetalPipelineCache::GetMeshPipelineState(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, CachedFBOMtl* activeFBO, const LatteContextRegister& lcr, Renderer::INDEX_TYPE hostIndexType)
{
    uint64 stateHash = CalculateRenderPipelineHash(fetchShader, vertexShader, pixelShader, activeFBO, lcr);

	stateHash += lcr.GetRawView()[mmVGT_PRIMITIVE_TYPE];
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += (uint8)hostIndexType;
	stateHash = std::rotl<uint64>(stateHash, 7); // TODO: 7?s

    auto& pipeline = m_pipelineCache[stateHash];
    if (pipeline)
        return pipeline;

	auto mtlObjectShader = static_cast<RendererShaderMtl*>(vertexShader->shader);
	auto mtlMeshShader = static_cast<RendererShaderMtl*>(geometryShader->shader);
	auto mtlPixelShader = static_cast<RendererShaderMtl*>(pixelShader->shader);
	mtlObjectShader->CompileObjectFunction(lcr, fetchShader, vertexShader, hostIndexType);
	mtlMeshShader->CompileMeshFunction(lcr, fetchShader);
	mtlPixelShader->CompileFragmentFunction(activeFBO);

	// Render pipeline state
	MTL::MeshRenderPipelineDescriptor* desc = MTL::MeshRenderPipelineDescriptor::alloc()->init();
	desc->setObjectFunction(mtlObjectShader->GetFunction());
	desc->setMeshFunction(mtlMeshShader->GetFunction());
	desc->setFragmentFunction(mtlPixelShader->GetFunction());

	SetFragmentState(desc, activeFBO, lcr);

	TryLoadBinaryArchive();

	// Load binary
    // TODO: no binary archives? :(

    NS::Error* error = nullptr;
#ifdef CEMU_DEBUG_ASSERT
    desc->setLabel(GetLabel("Mesh pipeline state", desc));
#endif
	pipeline = m_mtlr->GetDevice()->newRenderPipelineState(desc, MTL::PipelineOptionNone, nullptr, &error);
	if (error)
	{
    	debug_printf("error creating render pipeline state: %s\n", error->localizedDescription()->utf8String());
        error->release();
        return nullptr;
	}
	desc->release();

	return pipeline;
}

uint64 MetalPipelineCache::CalculateRenderPipelineHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* pixelShader, class CachedFBOMtl* activeFBO, const LatteContextRegister& lcr)
{
    // Hash
    uint64 stateHash = 0;
    for (int i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; ++i)
	{
		auto textureView = static_cast<LatteTextureViewMtl*>(activeFBO->colorBuffer[i].texture);
		if (!textureView)
		    continue;

		stateHash += textureView->GetRGBAView()->pixelFormat() + i * 31;
		stateHash = std::rotl<uint64>(stateHash, 7);
	}

	if (activeFBO->depthBuffer.texture)
	{
	    auto textureView = static_cast<LatteTextureViewMtl*>(activeFBO->depthBuffer.texture);
		stateHash += textureView->GetRGBAView()->pixelFormat();
		stateHash = std::rotl<uint64>(stateHash, 7);
	}

	for (auto& group : fetchShader->bufferGroups)
	{
		uint32 bufferStride = group.getCurrentBufferStride(lcr.GetRawView());
		stateHash = std::rotl<uint64>(stateHash, 7);
		stateHash += bufferStride * 3;
	}

	stateHash += fetchShader->getVkPipelineHashFragment();
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += lcr.GetRawView()[mmVGT_STRMOUT_EN];
	stateHash = std::rotl<uint64>(stateHash, 7);

	if(lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL())
		stateHash += 0x333333;

	stateHash = (stateHash >> 8) + (stateHash * 0x370531ull) % 0x7F980D3BF9B4639Dull;

	uint32* ctxRegister = lcr.GetRawView();

	if (vertexShader)
		stateHash += vertexShader->baseHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	if (pixelShader)
		stateHash += pixelShader->baseHash + pixelShader->auxHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	uint32 polygonCtrl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	stateHash += polygonCtrl;
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += ctxRegister[Latte::REGADDR::PA_CL_CLIP_CNTL];
	stateHash = std::rotl<uint64>(stateHash, 7);

	const auto colorControlReg = ctxRegister[Latte::REGADDR::CB_COLOR_CONTROL];
	stateHash += colorControlReg;

	stateHash += ctxRegister[Latte::REGADDR::CB_TARGET_MASK];

	const uint32 blendEnableMask = (colorControlReg >> 8) & 0xFF;
	if (blendEnableMask)
	{
		for (auto i = 0; i < 8; ++i)
		{
			if (((blendEnableMask & (1 << i))) == 0)
				continue;
			stateHash = std::rotl<uint64>(stateHash, 7);
			stateHash += ctxRegister[Latte::REGADDR::CB_BLEND0_CONTROL + i];
		}
	}

	return stateHash;
}

void MetalPipelineCache::TryLoadBinaryArchive()
{
    if (m_binaryArchive || s_cacheTitleId == INVALID_TITLE_ID)
        return;

    const std::string cacheFilename = fmt::format("{:016x}_mtl_pipelines.bin", s_cacheTitleId);
	const fs::path cachePath = ActiveSettings::GetCachePath("shaderCache/precompiled/{}", cacheFilename);
    m_binaryArchiveURL = NS::URL::fileURLWithPath(ToNSString((const char*)cachePath.generic_u8string().c_str()));

    MTL::BinaryArchiveDescriptor* desc = MTL::BinaryArchiveDescriptor::alloc()->init();
    desc->setUrl(m_binaryArchiveURL);

    NS::Error* error = nullptr;
    m_binaryArchive = m_mtlr->GetDevice()->newBinaryArchive(desc, &error);
    if (error)
    {
        desc->setUrl(nullptr);

        error->release();
        error = nullptr;
        m_binaryArchive = m_mtlr->GetDevice()->newBinaryArchive(desc, &error);
        if (error)
        {
            debug_printf("failed to create binary archive: %s\n", error->localizedDescription()->utf8String());
            error->release();
        }
    }
    desc->release();
}
