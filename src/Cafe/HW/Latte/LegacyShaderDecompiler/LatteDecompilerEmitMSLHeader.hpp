#pragma once

namespace LatteDecompiler
{
	static void _emitUniformVariables(LatteDecompilerShaderContext* decompilerContext)
	{
	    auto src = decompilerContext->shaderSource;

		LatteDecompilerShaderResourceMapping& resourceMapping = decompilerContext->output->resourceMappingGL;
		auto& uniformOffsets = decompilerContext->output->uniformOffsetsVK;

		src->add("struct SupportBuffer {" _CRLF);

		sint32 uniformCurrentOffset = 0;
		auto shader = decompilerContext->shader;
		auto shaderType = decompilerContext->shader->shaderType;
		if (decompilerContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
		{
			// uniform registers or buffers are accessed statically with predictable offsets
			// this allows us to remap the used entries into a more compact array
			if (shaderType == LatteConst::ShaderType::Vertex)
				src->addFmt("int4 remappedVS[{}];" _CRLF, (sint32)shader->list_remappedUniformEntries.size());
			else if (shaderType == LatteConst::ShaderType::Pixel)
				src->addFmt("int4 remappedPS[{}];" _CRLF, (sint32)shader->list_remappedUniformEntries.size());
			else if (shaderType == LatteConst::ShaderType::Geometry)
				src->addFmt("int4 remappedGS[{}];" _CRLF, (sint32)shader->list_remappedUniformEntries.size());
			else
				debugBreakpoint();
			uniformOffsets.offset_remapped = uniformCurrentOffset;
			uniformCurrentOffset += 16 * shader->list_remappedUniformEntries.size();
		}
		else if (decompilerContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE)
		{
			uint32 cfileSize = decompilerContext->analyzer.uniformRegisterAccessTracker.DetermineSize(decompilerContext->shaderBaseHash, 256);
			// full or partial uniform register file has to be present
			if (shaderType == LatteConst::ShaderType::Vertex)
				src->addFmt("int4 uniformRegisterVS[{}];" _CRLF, cfileSize);
			else if (shaderType == LatteConst::ShaderType::Pixel)
				src->addFmt("int4 uniformRegisterPS[{}];" _CRLF, cfileSize);
			else if (shaderType == LatteConst::ShaderType::Geometry)
				src->addFmt("int4 uniformRegisterGS[{}];" _CRLF, cfileSize);
			uniformOffsets.offset_uniformRegister = uniformCurrentOffset;
			uniformOffsets.count_uniformRegister = cfileSize;
			uniformCurrentOffset += 16 * cfileSize;
		}
		// special uniforms
		bool hasAnyViewportScaleDisabled =
			!decompilerContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_X_SCALE_ENA() ||
			!decompilerContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_Y_SCALE_ENA() ||
			!decompilerContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_Z_SCALE_ENA();

		if (decompilerContext->shaderType == LatteConst::ShaderType::Vertex && hasAnyViewportScaleDisabled)
		{
			// aka GX2 special state 0
			uniformCurrentOffset = (uniformCurrentOffset + 7)&~7;
			src->add("float2 windowSpaceToClipSpaceTransform;" _CRLF);
			uniformOffsets.offset_windowSpaceToClipSpaceTransform = uniformCurrentOffset;
			uniformCurrentOffset += 8;
		}
		bool alphaTestEnable = decompilerContext->contextRegistersNew->SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE();
		if (decompilerContext->shaderType == LatteConst::ShaderType::Pixel && alphaTestEnable)
		{
			uniformCurrentOffset = (uniformCurrentOffset + 3)&~3;
			src->add("float alphaTestRef;" _CRLF);
			uniformOffsets.offset_alphaTestRef = uniformCurrentOffset;
			uniformCurrentOffset += 4;
		}
		if (decompilerContext->analyzer.outputPointSize && decompilerContext->analyzer.writesPointSize == false)
		{
			if ((decompilerContext->shaderType == LatteConst::ShaderType::Vertex && !decompilerContext->options->usesGeometryShader) ||
				decompilerContext->shaderType == LatteConst::ShaderType::Geometry)
			{
				uniformCurrentOffset = (uniformCurrentOffset + 3)&~3;
				src->add("float pointSize;" _CRLF);
				uniformOffsets.offset_pointSize = uniformCurrentOffset;
				uniformCurrentOffset += 4;
			}
		}
		// define fragCoordScale which holds the xy scale for render target resolution vs effective resolution
		if (shader->shaderType == LatteConst::ShaderType::Pixel)
		{
			uniformCurrentOffset = (uniformCurrentOffset + 7)&~7;
			src->add("float2 fragCoordScale;" _CRLF);
			uniformOffsets.offset_fragCoordScale = uniformCurrentOffset;
			uniformCurrentOffset += 8;
		}
		// provide scale factor for every texture that is accessed via texel coordinates (texelFetch)
		for (sint32 t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
		{
			if (decompilerContext->analyzer.texUnitUsesTexelCoordinates.test(t) == false)
				continue;
			uniformCurrentOffset = (uniformCurrentOffset + 7) & ~7;
			src->addFmt("float2 tex{}Scale;" _CRLF, t);
			uniformOffsets.offset_texScale[t] = uniformCurrentOffset;
			uniformCurrentOffset += 8;
		}
		// define verticesPerInstance + streamoutBufferBaseX
		if (decompilerContext->analyzer.useSSBOForStreamout &&
			(shader->shaderType == LatteConst::ShaderType::Vertex && decompilerContext->options->usesGeometryShader == false) ||
			(shader->shaderType == LatteConst::ShaderType::Geometry) )
		{
			src->add("int verticesPerInstance;" _CRLF);
			uniformOffsets.offset_verticesPerInstance = uniformCurrentOffset;
			uniformCurrentOffset += 4;
			for (uint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; i++)
			{
				if (decompilerContext->output->streamoutBufferWriteMask[i])
				{
					src->addFmt("int streamoutBufferBase{};" _CRLF, i);
					uniformOffsets.offset_streamoutBufferBase[i] = uniformCurrentOffset;
					uniformCurrentOffset += 4;
				}
			}
		}

		src->add("};" _CRLF _CRLF);

		uniformOffsets.offset_endOfBlock = uniformCurrentOffset;
	}

	static void _emitUniformBuffers(LatteDecompilerShaderContext* decompilerContext)
	{
		auto shaderSrc = decompilerContext->shaderSource;
		// uniform buffer definition
		if (decompilerContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
		{
			for (uint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
			{
				if (!decompilerContext->analyzer.uniformBufferAccessTracker[i].HasAccess())
					continue;

				cemu_assert_debug(decompilerContext->output->resourceMappingGL.uniformBuffersBindingPoint[i] >= 0);
				cemu_assert_debug(decompilerContext->output->resourceMappingVK.uniformBuffersBindingPoint[i] >= 0);

				//shaderSrc->addFmt("UNIFORM_BUFFER_LAYOUT({}, {}, {}) ", (sint32)decompilerContext->output->resourceMappingGL.uniformBuffersBindingPoint[i], (sint32)decompilerContext->output->resourceMappingVK.setIndex, (sint32)decompilerContext->output->resourceMappingVK.uniformBuffersBindingPoint[i]);

				shaderSrc->addFmt("struct UBuff{} {{" _CRLF, i);
				shaderSrc->addFmt("float4 d{}[{}];" _CRLF, i, decompilerContext->analyzer.uniformBufferAccessTracker[i].DetermineSize(decompilerContext->shaderBaseHash, LATTE_GLSL_DYNAMIC_UNIFORM_BLOCK_SIZE));
				shaderSrc->add("};" _CRLF _CRLF);
			}
		}
		else if (decompilerContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
		{
			// already generated in _emitUniformVariables
		}
		else if (decompilerContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE)
		{
			// already generated in _emitUniformVariables
		}
		else if (decompilerContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_NONE)
		{
			// no uniforms used
		}
		else
		{
			cemu_assert_debug(false);
		}
	}

	static void _emitAttributes(LatteDecompilerShaderContext* decompilerContext)
	{
		auto src = decompilerContext->shaderSource;

		if (decompilerContext->shader->shaderType == LatteConst::ShaderType::Vertex)
		{
		    src->add("struct VertexIn {" _CRLF);
			// attribute inputs
			for (uint32 i = 0; i < LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS; i++)
			{
				if (decompilerContext->analyzer.inputAttributSemanticMask[i])
				{
					cemu_assert_debug(decompilerContext->output->resourceMappingGL.attributeMapping[i] >= 0);
					cemu_assert_debug(decompilerContext->output->resourceMappingVK.attributeMapping[i] >= 0);
					cemu_assert_debug(decompilerContext->output->resourceMappingGL.attributeMapping[i] == decompilerContext->output->resourceMappingVK.attributeMapping[i]);

					src->addFmt("uint4 attrDataSem{} [[attribute({})]];" _CRLF, i, (sint32)decompilerContext->output->resourceMappingVK.attributeMapping[i]);
				}
			}
			src->add("};" _CRLF _CRLF);
		}
	}

	static void _emitVSOutputs(LatteDecompilerShaderContext* shaderContext)
	{
		auto* src = shaderContext->shaderSource;

		src->add("struct VertexOut {" _CRLF);

		src->add("float4 position [[position]];" _CRLF);
		if (shaderContext->analyzer.outputPointSize)
		    src->add("float pointSize[[point_size]];" _CRLF);

		LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
		auto parameterMask = shaderContext->shader->outputParameterMask;
		for (uint32 i = 0; i < 32; i++)
		{
			if ((parameterMask&(1 << i)) == 0)
				continue;
			uint32 vsSemanticId = _getVertexShaderOutParamSemanticId(shaderContext->contextRegisters, i);
			if (vsSemanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
				continue;
			// get import based on semanticId
			sint32 psInputIndex = -1;
			for (sint32 f = 0; f < psInputTable->count; f++)
			{
				if (psInputTable->import[f].semanticId == vsSemanticId)
				{
					psInputIndex = f;
					break;
				}
			}
			if (psInputIndex == -1)
				continue; // no ps input

			src->addFmt("float4 passParameterSem{}", psInputTable->import[psInputIndex].semanticId);
			src->addFmt(" [[user(locn{})]]", psInputIndex);
			if (psInputTable->import[psInputIndex].isFlat)
				src->add(" [[flat]]");
			if (psInputTable->import[psInputIndex].isNoPerspective)
				src->add(" [[center_no_perspective]]");
			src->addFmt(";" _CRLF);
		}

		src->add("};" _CRLF _CRLF);
	}

	static void _emitPSInputs(LatteDecompilerShaderContext* shaderContext)
	{
		auto* src = shaderContext->shaderSource;

		src->add("struct FragmentIn {" _CRLF);

		LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
		for (sint32 i = 0; i < psInputTable->count; i++)
		{
			if (psInputTable->import[i].semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
				continue;
			src->addFmt("float4 passParameterSem{}", psInputTable->import[i].semanticId);
			src->addFmt(" [[user(locn{})]]", i);
			if (psInputTable->import[i].isFlat)
				src->add(" [[flat]]");
			if (psInputTable->import[i].isNoPerspective)
				src->add(" [[center_no_perspective]]");
			src->add(";" _CRLF);
		}

		src->add("};" _CRLF _CRLF);
	}

	static void _emitInputsAndOutputs(LatteDecompilerShaderContext* decompilerContext)
	{
		auto src = decompilerContext->shaderSource;

		if (decompilerContext->shaderType == LatteConst::ShaderType::Vertex)
		{
		    _emitAttributes(decompilerContext);
			_emitVSOutputs(decompilerContext);

			// TODO: transform feedback
		}
		else if (decompilerContext->shaderType == LatteConst::ShaderType::Pixel)
		{
			_emitPSInputs(decompilerContext);

			src->add("struct FragmentOut {" _CRLF);

            // generate pixel outputs for pixel shader
            for (uint32 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
            {
               	if ((decompilerContext->shader->pixelColorOutputMask&(1 << i)) != 0)
               	{
              		src->addFmt("float4 passPixelColor{} [[color({})]];" _CRLF, i, i);
               	}
            }

            src->add("};" _CRLF _CRLF);
		}
	}

	static void emitHeader(LatteDecompilerShaderContext* decompilerContext)
	{
		const bool dump_shaders_enabled = ActiveSettings::DumpShadersEnabled();
		if(dump_shaders_enabled)
			decompilerContext->shaderSource->add("// start of shader inputs/outputs, predetermined by Cemu. Do not touch" _CRLF);
		// uniform variables
		_emitUniformVariables(decompilerContext);
		// uniform buffers
		_emitUniformBuffers(decompilerContext);
		// inputs and outputs
		_emitInputsAndOutputs(decompilerContext);

		if (dump_shaders_enabled)
			decompilerContext->shaderSource->add("// end of shader inputs/outputs" _CRLF);
	}

	static void _emitUniformBufferDefinitions(LatteDecompilerShaderContext* decompilerContext)
	{
		auto src = decompilerContext->shaderSource;
		// uniform buffer definition
		if (decompilerContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
		{
			for (uint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
			{
				if (!decompilerContext->analyzer.uniformBufferAccessTracker[i].HasAccess())
					continue;

				cemu_assert_debug(decompilerContext->output->resourceMappingGL.uniformBuffersBindingPoint[i] >= 0);
				cemu_assert_debug(decompilerContext->output->resourceMappingVK.uniformBuffersBindingPoint[i] >= 0);

				src->addFmt(", constant UBuff{}& ubuff{} [[buffer({})]]", i, i, (sint32)decompilerContext->output->resourceMappingGL.uniformBuffersBindingPoint[i]);
			}
		}
	}

	static void _emitTextureDefinitions(LatteDecompilerShaderContext* shaderContext)
	{
		auto src = shaderContext->shaderSource;
		// texture sampler definition
		for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
		{
			if (!shaderContext->output->textureUnitMask[i])
				continue;

			src->add(", ");

			if (shaderContext->shader->textureIsIntegerFormat[i])
			{
				// integer samplers
				if (shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_1D)
					src->add("texture1d<uint>");
				else if (shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_2D || shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_2D_MSAA)
					src->add("texture2d<uint>");
				else
					cemu_assert_unimplemented();
			}
			else if (shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_2D || shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_2D_MSAA)
				src->add("texture2d<float>");
			else if (shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_1D)
				src->add("texture1d<float>");
			else if (shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_2D_ARRAY)
				src->add("texture2d_array<float>");
			else if (shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_CUBEMAP)
				src->add("texturecube_array<float>");
			else if (shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_3D)
				src->add("texture3d<float>");
			else
			{
				cemu_assert_unimplemented();
			}

			src->addFmt(" tex{} [[texture({})]]", i, shaderContext->output->resourceMappingGL.textureUnitToBindingPoint[i]);
			src->addFmt(", sampler samplr{} [[sampler({})]]", i, shaderContext->output->resourceMappingGL.textureUnitToBindingPoint[i]);
		}
	}

	static void emitInputs(LatteDecompilerShaderContext* decompilerContext)
	{
	    auto src = decompilerContext->shaderSource;

		switch (decompilerContext->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
            src->add("VertexIn");
            break;
        case LatteConst::ShaderType::Pixel:
            src->add("FragmentIn");
            break;
		}

		src->add(" in [[stage_in]], constant SupportBuffer& supportBuffer [[buffer(30)]]");
		switch (decompilerContext->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
            src->add(", uint vid [[vertex_id]]");
            src->add(", uint iid [[instance_id]]");
            break;
        case LatteConst::ShaderType::Pixel:
            src->add(", bool frontFacing [[front_facing]]");
            break;
		}
		// uniform buffers
		_emitUniformBufferDefinitions(decompilerContext);
		// textures
		_emitTextureDefinitions(decompilerContext);
	}
}
