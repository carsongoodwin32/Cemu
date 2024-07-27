#include "Cafe/HW/Latte/Renderer/Metal/CachedFBOMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"
#include "Metal/MTLRenderPass.hpp"

void CachedFBOMtl::CreateRenderPass()
{
	m_renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();

	for (int i = 0; i < 8; ++i)
	{
		auto& buffer = colorBuffer[i];
		auto textureView = (LatteTextureViewMtl*)buffer.texture;
		if (!textureView)
		{
			continue;
		}
		auto colorAttachment = m_renderPassDescriptor->colorAttachments()->object(i);
		colorAttachment->setTexture(textureView->GetTexture());
		colorAttachment->setLoadAction(MTL::LoadActionLoad);
		colorAttachment->setStoreAction(MTL::StoreActionStore);
	}

	// setup depth attachment
	if (depthBuffer.texture)
	{
		auto textureView = static_cast<LatteTextureViewMtl*>(depthBuffer.texture);
		auto depthAttachment = m_renderPassDescriptor->depthAttachment();
		depthAttachment->setTexture(textureView->GetTexture());
		depthAttachment->setLoadAction(MTL::LoadActionLoad);
		depthAttachment->setStoreAction(MTL::StoreActionStore);
	}
}

CachedFBOMtl::~CachedFBOMtl()
{
	m_renderPassDescriptor->release();
}
