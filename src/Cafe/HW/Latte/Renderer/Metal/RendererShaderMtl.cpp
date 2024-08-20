#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
//#include "Cemu/FileCache/FileCache.h"
//#include "config/ActiveSettings.h"

#include "Cemu/Logging/CemuLogging.h"
#include "Common/precompiled.h"
#include "HW/Latte/Core/FetchShader.h"
#include "HW/Latte/ISA/RegDefines.h"

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

RendererShaderMtl::RendererShaderMtl(MetalRenderer* mtlRenderer, ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& mslCode)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_mtlr{mtlRenderer}
{
    // TODO: don't compile just-in-time
    m_mslCode = mslCode;

	// Count shader compilation
	g_compiled_shaders_total++;
}

RendererShaderMtl::~RendererShaderMtl()
{
    if (m_function)
        m_function->release();
}

void RendererShaderMtl::CompileObjectFunction(const LatteContextRegister& lcr, const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, Renderer::INDEX_TYPE hostIndexType)
{
    cemu_assert_debug(m_type == ShaderType::kVertex);

    std::string fullCode;

    // Primitive type
    const LattePrimitiveMode primitiveMode = static_cast<LattePrimitiveMode>(lcr.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE());
    fullCode += "#define PRIMITIVE_TYPE ";
    switch (primitiveMode)
    {
    case LattePrimitiveMode::POINTS:
        fullCode += "point";
        break;
    case LattePrimitiveMode::LINES:
        fullCode += "line";
        break;
    case LattePrimitiveMode::TRIANGLES:
        fullCode += "triangle";
        break;
    default:
        break;
    }
    fullCode += "\n";

    // Vertex buffers
    std::string vertexBufferDefinitions = "#define VERTEX_BUFFER_DEFINITIONS ";
    std::string vertexBuffers = "#define VERTEX_BUFFERS ";
    std::string inputFetchDefinition = "VertexIn fetchInput(thread uint& vid VERTEX_BUFFER_DEFINITIONS) {\n";
    inputFetchDefinition += "VertexIn in;\n";
    if (hostIndexType != Renderer::INDEX_TYPE::NONE)
    {
        vertexBufferDefinitions += ", device ";
        switch (hostIndexType)
        {
        case Renderer::INDEX_TYPE::U16:
            vertexBufferDefinitions += "ushort";
            break;
        case Renderer::INDEX_TYPE::U32:
            vertexBufferDefinitions += "uint";
            break;
        default:
            cemu_assert_suspicious();
            break;
        }
        // TODO: don't hardcode the index
        vertexBufferDefinitions += "* indexBuffer [[buffer(20)]]";
        vertexBuffers += ", indexBuffer";
        inputFetchDefinition += "vid = indexBuffer[vid]\n";
    }
    for (auto& bufferGroup : fetchShader->bufferGroups)
	{
        std::optional<LatteConst::VertexFetchType2> fetchType;

       	for (sint32 j = 0; j < bufferGroup.attribCount; ++j)
       	{
      		auto& attr = bufferGroup.attrib[j];

      		uint32 semanticId = vertexShader->resourceMapping.attributeMapping[attr.semanticId];
      		if (semanticId == (uint32)-1)
     			continue; // attribute not used?

            std::string formatName;
            switch (GetMtlVertexFormat(attr.format))
            {
            case MTL::VertexFormatUChar:
                formatName = "uchar";
                break;
            case MTL::VertexFormatUChar2:
                formatName = "uchar2";
                break;
            case MTL::VertexFormatUChar3:
                formatName = "uchar3";
                break;
            case MTL::VertexFormatUChar4:
                formatName = "uchar4";
                break;
            case MTL::VertexFormatUShort:
                formatName = "ushort";
                break;
            case MTL::VertexFormatUShort2:
                formatName = "ushort2";
                break;
            case MTL::VertexFormatUShort3:
                formatName = "ushort3";
                break;
            case MTL::VertexFormatUShort4:
                formatName = "ushort4";
                break;
            case MTL::VertexFormatUInt:
                formatName = "uint";
                break;
            case MTL::VertexFormatUInt2:
                formatName = "uint2";
                break;
            case MTL::VertexFormatUInt3:
                formatName = "uint3";
                break;
            case MTL::VertexFormatUInt4:
                formatName = "uint4";
                break;
            }

            // Fetch the attribute
            inputFetchDefinition += "in.ATTRIBUTE_NAME" + std::to_string(semanticId) + " = ";
            inputFetchDefinition += "*(device " + formatName + "*)";
            inputFetchDefinition += "(vertexBuffer" + std::to_string(attr.attributeBufferIndex);
            inputFetchDefinition += " + vid + " + std::to_string(attr.offset) + ");\n";

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

		fullCode += ", device uchar* vertexBuffer" + std::to_string(bufferIndex) + " [[buffer(" + std::to_string(GET_MTL_VERTEX_BUFFER_INDEX(bufferIndex)) + ")]]";
	}
	inputFetchDefinition += "return in;\n";
	inputFetchDefinition += "}\n";

	fullCode += vertexBufferDefinitions + "\n";
	fullCode += vertexBuffers + "\n";
    fullCode += m_mslCode;
    fullCode += inputFetchDefinition;

    Compile(fullCode);
}

void RendererShaderMtl::CompileMeshFunction(const LatteContextRegister& lcr, const LatteFetchShader* fetchShader)
{
    cemu_assert_debug(m_type == ShaderType::kGeometry);

    std::string fullCode;

    // Primitive type
    const LattePrimitiveMode primitiveMode = static_cast<LattePrimitiveMode>(lcr.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE());
    fullCode += "#define PRIMITIVE_TYPE ";
    switch (primitiveMode)
    {
    case LattePrimitiveMode::POINTS:
        fullCode += "point";
        break;
    case LattePrimitiveMode::LINES:
        fullCode += "line";
        break;
    case LattePrimitiveMode::TRIANGLES:
        fullCode += "triangle";
        break;
    default:
        break;
    }
    fullCode += "\n";

    fullCode += m_mslCode;
    Compile(fullCode);
}

void RendererShaderMtl::CompileFragmentFunction(CachedFBOMtl* activeFBO)
{
    cemu_assert_debug(m_type == ShaderType::kFragment);

    std::string fullCode;

    // Define color attachment data types
    for (uint8 i = 0; i < 8; i++)
	{
	    const auto& colorBuffer = activeFBO->colorBuffer[i];
		if (!colorBuffer.texture)
		{
		    continue;
		}
		auto dataType = GetMtlPixelFormatInfo(colorBuffer.texture->format, false).dataType;
		fullCode += "#define " + GetColorAttachmentTypeStr(i) + " ";
		switch (dataType)
		{
		case MetalDataType::INT:
		    fullCode += "int4";
			break;
		case MetalDataType::UINT:
		    fullCode += "uint4";
			break;
		case MetalDataType::FLOAT:
		    fullCode += "float4";
			break;
		default:
		    cemu_assert_suspicious();
			break;
		}
		fullCode += "\n";
	}

    fullCode += m_mslCode;
    Compile(fullCode);
}

void RendererShaderMtl::Compile(const std::string& mslCode)
{
    if (m_function)
        m_function->release();

    NS::Error* error = nullptr;
	MTL::Library* library = m_mtlr->GetDevice()->newLibrary(ToNSString(mslCode), nullptr, &error);
	if (error)
    {
        printf("failed to create library (error: %s) -> source:\n%s\n", error->localizedDescription()->utf8String(), mslCode.c_str());
        error->release();
        return;
    }
    m_function = library->newFunction(ToNSString("main0"));
    library->release();
}
