#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "glm/glm.hpp"
#include "glm/gtx/hash.hpp"

#include <vulkan/vulkan.h>
#include <unordered_map>

#include "assimp/Importer.hpp"
#include "assimp/scene.h"    
#include "assimp/postprocess.h"
#include "assimp/cimport.h"
#include "vutils.h"


#define DIFF_IRRADIANCE_MAP_SIZE 32
#define SPEC_IRRADIANCE_MAP_SIZE 512


struct Vertex
{
	glm::vec3 pos;
	glm::vec3 normal;
	glm::vec2 texCoord;

	bool operator==(const Vertex& other) const
	{
		return pos == other.pos &&
			normal == other.normal &&
			texCoord == other.texCoord;
	}

	static VkVertexInputBindingDescription getBindingDescription()
	{
		VkVertexInputBindingDescription bindingDescription = {};
		bindingDescription.binding = 0;
		bindingDescription.stride = sizeof(Vertex);
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		return bindingDescription;
	}

	static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions()
	{
		std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions = {};

		attributeDescriptions[0].binding = 0;
		attributeDescriptions[0].location = 0;
		attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[0].offset = offsetof(Vertex, pos);

		attributeDescriptions[1].binding = 0;
		attributeDescriptions[1].location = 1;
		attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[1].offset = offsetof(Vertex, normal);

		attributeDescriptions[2].binding = 0;
		attributeDescriptions[2].location = 2;
		attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
		attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

		return attributeDescriptions;
	}
};

namespace std
{
	template<> struct hash<Vertex>
	{
		size_t operator()(Vertex const& vertex) const
		{
			size_t seed = 0;
			hash_combine(seed, vertex.pos);
			hash_combine(seed, vertex.normal);
			hash_combine(seed, vertex.texCoord);
			return seed;
		}
	};
}

enum MaterialType
{
	MATERIAL_TYPE_HDR_PROBE = 0,
	MATERIAL_TYPE_FSCHLICK_DGGX_GSMITH,
	MATERIAL_TYPE_COUNT
};

typedef uint32_t MaterialType_t;

struct PerModelUniformBuffer
{
	glm::mat4 M;
	glm::mat4 M_invTrans;
};

class VMesh
{
public:
	const static uint32_t numMapsPerMesh = 5;

	PerModelUniformBuffer *uPerModelInfo = nullptr;

	bool uniformDataChanged = true;
	glm::vec3 worldPosition{ 0.f, 0.f, 0.f };
	glm::quat worldRotation{ glm::vec3(0.f, 0.f, 0.f) }; // Euler angles to quaternion
	float scale = 1.f;

	BufferWrapper vertexBuffer;
	BufferWrapper indexBuffer;

	ImageWrapper albedoMap;
	ImageWrapper normalMap;
	ImageWrapper roughnessMap;
	ImageWrapper metalnessMap;
	ImageWrapper aoMap;

	MaterialType_t materialType = MATERIAL_TYPE_FSCHLICK_DGGX_GSMITH;

	static void loadMeshIntoHostBuffers(
		const std::string &modelFileName,
		std::vector<Vertex> &hostVerts, std::vector<uint32_t> &hostIndices)
	{
		Assimp::Importer meshImporter;
		const aiScene *scene = nullptr;

		const uint32_t defaultFlags =
			aiProcess_FlipWindingOrder |
			aiProcess_Triangulate |
			aiProcess_PreTransformVertices |
			//aiProcess_CalcTangentSpace |
			aiProcess_GenSmoothNormals;

		scene = meshImporter.ReadFile(modelFileName, defaultFlags);

		std::unordered_map<Vertex, uint32_t> vert2IdxLut;

		for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
		{
			const aiMesh *mesh = scene->mMeshes[i];
			const aiVector3D *vertices = mesh->mVertices;
			const aiVector3D *normals = mesh->mNormals;
			const aiVector3D *texCoords = mesh->mTextureCoords[0];
			const auto *faces = mesh->mFaces;

			if (!normals || !texCoords)
			{
				throw std::runtime_error("model must have normals, tangents, and uvs.");
			}

			for (uint32_t j = 0; j < mesh->mNumFaces; ++j)
			{
				const aiFace &face = faces[j];

				if (face.mNumIndices != 3) continue;

				for (uint32_t k = 0; k < face.mNumIndices; ++k)
				{
					uint32_t idx = face.mIndices[k];
					const aiVector3D &pos = vertices[idx];
					const aiVector3D &nrm = normals[idx];
					const aiVector3D &texCoord = texCoords[idx];

					Vertex vert =
					{
						glm::vec3(pos.x, pos.y, pos.z),
						glm::vec3(nrm.x, nrm.y, nrm.z),
						glm::vec2(texCoord.x, 1.f - texCoord.y)
					};

					const auto searchResult = vert2IdxLut.find(vert);
					if (searchResult == vert2IdxLut.end())
					{
						uint32_t newIdx = static_cast<uint32_t>(hostVerts.size());
						vert2IdxLut[vert] = newIdx;
						hostIndices.emplace_back(newIdx);
						hostVerts.emplace_back(vert);
					}
					else
					{
						hostIndices.emplace_back(searchResult->second);
					}
				}
			}
		}
	}

	VMesh(const VDeleter<VkDevice> &device) :
		vertexBuffer{ device },
		indexBuffer{ device },
		albedoMap{ device },
		normalMap{ device },
		roughnessMap{ device },
		metalnessMap{ device },
		aoMap{ device }
	{}

	void load(
		VkPhysicalDevice physicalDevice,
		const VDeleter<VkDevice> &device,
		VkCommandPool commandPool,
		VkQueue submitQueue,
		const std::string &modelFileName,
		const std::string &albedoMapName,
		const std::string &normalMapName,
		const std::string &roughnessMapName,
		const std::string &metalnessMapName,
		const std::string &aoMapName = "")
	{
		// load textures
		if (albedoMapName != "")
		{
			loadTexture(physicalDevice, device, commandPool, submitQueue, albedoMapName, albedoMap);
		}
		if (normalMapName != "")
		{
			loadTexture(physicalDevice, device, commandPool, submitQueue, normalMapName, normalMap);
		}
		if (roughnessMapName != "")
		{
			loadTexture(physicalDevice, device, commandPool, submitQueue, roughnessMapName, roughnessMap);
		}
		if (metalnessMapName != "")
		{
			loadTexture(physicalDevice, device, commandPool, submitQueue, metalnessMapName, metalnessMap);
		}
		if (aoMapName != "")
		{
			loadTexture(physicalDevice, device, commandPool, submitQueue, aoMapName, aoMap);
		}

		// load mesh
		std::vector<Vertex> hostVerts;
		std::vector<uint32_t> hostIndices;
		loadMeshIntoHostBuffers(modelFileName, hostVerts, hostIndices);

		// create vertex buffer
		createBufferFromHostData(
			physicalDevice, device, commandPool, submitQueue,
			hostVerts.data(), hostVerts.size(), sizeof(hostVerts[0]) * hostVerts.size(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			vertexBuffer);

		// create index buffer
		createBufferFromHostData(
			physicalDevice, device, commandPool, submitQueue,
			hostIndices.data(), hostIndices.size(), sizeof(hostIndices[0]) * hostIndices.size(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			indexBuffer);
	}

	virtual void updateHostUniformBuffer()
	{
		assert(uPerModelInfo);
		if (!uniformDataChanged) return;
		uPerModelInfo->M = glm::translate(glm::mat4_cast(worldRotation) * glm::scale(glm::mat4(), glm::vec3(scale)), worldPosition);
		uPerModelInfo->M_invTrans = glm::transpose(glm::inverse(uPerModelInfo->M));
		uniformDataChanged = false;
	}
};

class Skybox : public VMesh
{
public:
	ImageWrapper radianceMap; // unfiltered map
	ImageWrapper specularIrradianceMap;
	ImageWrapper diffuseIrradianceMap; // TODO: use spherical harmonics instead

	bool specMapReady = false;
	bool diffMapReady = false;
	bool shouldSaveSpecMap = false;
	bool shouldSaveDiffMap = false;

	Skybox(const VDeleter<VkDevice> &device) :
		VMesh{ device },
		radianceMap{ device, VK_FORMAT_R32G32_SFLOAT },
		specularIrradianceMap{ device, VK_FORMAT_R32G32B32A32_SFLOAT },
		diffuseIrradianceMap{ device, VK_FORMAT_R32G32B32A32_SFLOAT }
	{
		materialType = MATERIAL_TYPE_HDR_PROBE;
	}

	void load(
		VkPhysicalDevice physicalDevice,
		const VDeleter<VkDevice> &device,
		VkCommandPool commandPool,
		VkQueue submitQueue,
		const std::string &modelFileName,
		const std::string &radianceMapName,
		const std::string &specMapName,
		const std::string &diffuseMapName)
	{
		if (radianceMapName != "")
		{
			loadCubemap(physicalDevice, device, commandPool, submitQueue, radianceMapName, radianceMap);
		}
		else
		{
			throw std::invalid_argument("radiance map required but not provided.");
		}

		if (specMapName != "")
		{
			loadCubemap(physicalDevice, device, commandPool, submitQueue, specMapName, specularIrradianceMap);
			specMapReady = true;
		}
		else
		{
			uint32_t mipLevels = static_cast<uint32_t>(floor(log2f(SPEC_IRRADIANCE_MAP_SIZE) + 0.5f)) + 1;
			specularIrradianceMap.mipLevels = mipLevels;
			specularIrradianceMap.format = VK_FORMAT_R32G32B32A32_SFLOAT;

			createCubemapImage(physicalDevice, device,
				SPEC_IRRADIANCE_MAP_SIZE, SPEC_IRRADIANCE_MAP_SIZE,
				mipLevels,
				specularIrradianceMap.format, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				specularIrradianceMap.image, specularIrradianceMap.imageMemory);

			createImageViewCube(device, specularIrradianceMap.image, specularIrradianceMap.format,
				VK_IMAGE_ASPECT_COLOR_BIT, specularIrradianceMap.mipLevels, specularIrradianceMap.imageView);

			specularIrradianceMap.imageViews.resize(mipLevels, { device, vkDestroyImageView });
			for (uint32_t level = 0; level < mipLevels; ++level)
			{
				createImageViewCube(device, specularIrradianceMap.image, specularIrradianceMap.format,
					VK_IMAGE_ASPECT_COLOR_BIT, level, 1, specularIrradianceMap.imageViews[level]);
			}

			VkSamplerCreateInfo samplerInfo = {};
			getDefaultSamplerCreateInfo(samplerInfo);
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.maxLod = static_cast<float>(specularIrradianceMap.mipLevels - 1);

			if (vkCreateSampler(device, &samplerInfo, nullptr, specularIrradianceMap.sampler.replace()) != VK_SUCCESS)
			{
				throw std::runtime_error("failed to create specular irradiance map sampler!");
			}

			shouldSaveSpecMap = true;
		}

		if (diffuseMapName != "")
		{
			loadCubemap(physicalDevice, device, commandPool, submitQueue, diffuseMapName, diffuseIrradianceMap);
			diffMapReady = true;
		}
		else
		{
			diffuseIrradianceMap.mipLevels = 1;
			diffuseIrradianceMap.format = VK_FORMAT_R32G32B32A32_SFLOAT;

			createCubemapImage(physicalDevice, device,
				DIFF_IRRADIANCE_MAP_SIZE, DIFF_IRRADIANCE_MAP_SIZE,
				diffuseIrradianceMap.mipLevels,
				diffuseIrradianceMap.format, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				diffuseIrradianceMap.image, diffuseIrradianceMap.imageMemory);

			createImageViewCube(device, diffuseIrradianceMap.image, diffuseIrradianceMap.format,
				VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, diffuseIrradianceMap.imageView);

			VkSamplerCreateInfo samplerInfo = {};
			getDefaultSamplerCreateInfo(samplerInfo);
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.maxLod = static_cast<float>(diffuseIrradianceMap.mipLevels - 1);

			if (vkCreateSampler(device, &samplerInfo, nullptr, diffuseIrradianceMap.sampler.replace()) != VK_SUCCESS)
			{
				throw std::runtime_error("failed to create diffuse irradiance map sampler!");
			}

			shouldSaveDiffMap = true;
		}

		VMesh::load(physicalDevice, device, commandPool, submitQueue, modelFileName, "", "", "", "");
	}
};