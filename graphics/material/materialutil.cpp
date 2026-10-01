#include "materialutil.h"

#include "emissivematerial.h"
#include "pbrmaterial.h"

#include "../core/textureutil.h"

#include <json11.hpp>

namespace sgd {

Material* deserializeMaterial(const json11::Json& json, CPath rootPath) {

	auto mtype = json["type"].string_value();
	auto desc = MaterialDescriptor::forTypeName(mtype);
	if (!desc) {
		SGD_LOG << "Material error: unrecognized type \"" + mtype + "\"";
		return nullptr;
	}

	auto material = new Material(desc);
	if (!json["blendMode"].is_null()) material->blendMode = (BlendMode)json["blendMode"].int_value();
	if (!json["depthFunc"].is_null()) material->depthFunc = (DepthFunc)json["depthFunc"].int_value();
	if (!json["cullMode"].is_null()) material->cullMode = (CullMode)json["cullMode"].int_value();

	for (auto& kv : json["textures"].object_items()) {

		auto qname = '\"' + kv.first + '\"';

		auto it = desc->textureDescs.find(kv.first);
		if (it == desc->textureDescs.end()) {
			SGD_LOG << "Material error: texture" << qname << "not found in material type \"" + mtype + "\"";
			continue;
		}
		auto& texDesc = it->second;

		auto type = texDesc.defType;
		auto format = texDesc.defFormat;
		auto flags = texDesc.defFlags;
		Path path;

		auto& value = kv.second;
		if (value.is_string()) {
			path = rootPath / value.string_value();
		} else if (value.is_object()) {
			if (!value["path"].is_string()) {
				SGD_LOG << "Material error: texture" << qname << "has no path";
				continue;
			}
			path = rootPath / value["path"].string_value();
			if (!value["type"].is_null()) type = (TextureType)value["type"].int_value();
			if (!value["format"].is_null()) format = (TextureFormat)value["format"].int_value();
			if (!value["flags"].is_null()) flags = (TextureFlags)value["flags"].int_value();
		} else {
			SGD_LOG << "Material error: texture" << qname << "has invalid type";
			continue;
		}
		Texture* texture;
		switch (type) {
		case TextureType::e2d:
			texture = load2DTexture(path, format, flags).result();
			break;
		case TextureType::cube:
			texture = loadCubeTexture(path, format, flags).result();
			break;
		case TextureType::array:
			texture = loadArrayTexture(path, format, flags).result();
			break;
		default:
			SGD_LOG << "Material error: invalid type of texture \"" + kv.first + "\"";
			continue;
		}
		material->setTexture(kv.first, texture);
	}
	for (auto& kv : json["colors"].object_items()) {
		auto& items = kv.second.array_items();
		if (items.size() != 3) {
			SGD_LOG << "Material error: invalid type for color \"" + kv.first + "\"";
			continue;
		}
		material->setColor(kv.first, Vec4f((float)items[0].number_value(), (float)items[1].number_value(),
										   (float)items[2].number_value(), (float)items[3].number_value()));
	}
	for (auto& kv : json["floats"].object_items()) {
		material->setFloat(kv.first, (float)kv.second.number_value());
	}
	return material;
}

Material* createMaterial(CString type) {
	auto desc = MaterialDescriptor::forTypeName(type);
	if (!desc) SGD_ERROR("Unknown material type \"" + type + "\"");

	return new Material(desc);
}

Expected<Material*, FileioEx> loadMaterial(CPath tpath) {
	auto path = tpath;
	if (toLower(path.ext()) != ".sgd") path = path / "material.sgd";

	auto str = loadString(path);
	if (!str) return str.error();

	String err;
	auto json = json11::Json::parse(str.result(), err, json11::JsonParse::COMMENTS);
	if (json.is_null()) return SGD_PATHEX("JSON error parsing", path);

	auto material = deserializeMaterial(json, path.parentPath());
	if (!material) return SGD_PATHEX("Failed to deserialize material", path);
	material->path = path;

	return material;
}

Material* createPBRMaterial() {
	return new Material(&pbrMaterialDescriptor);
}

Expected<Material*, FileioEx> loadPBRMaterial(CPath path) {
	static const Set<String> imgExts{".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".exr"};
	if (imgExts.find(toLower(path.ext())) == imgExts.end()) return loadMaterial(path);

	auto texture = load2DTexture(path, TextureFormat::any, TextureFlags::default_);
	if (!texture) return texture.error();

	auto material = new Material(&pbrMaterialDescriptor);
	material->path = path;

	material->setTexture("albedo", texture.result());

	return material;

#if 0
	if (!path.isUrl() && !path.exists()) return SGD_FILEIOEX("Material path does not exist");

	auto flags = TextureFlags::default_;

	Set<String> exts{".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".exr"};
	if (exts.find(toLower(path.ext())) != exts.end()) {

		auto texture = load2DTexture(path, TextureFormat::any, flags);
		if (!texture) return texture.error();

		auto material = new Material(&pbrMaterialDescriptor);
		material->setTexture("albedo", texture.result());
		material->path = path;
		return material;
	}

	auto tryLoad = [=](CString suffix, TextureFormat format) -> Texture* {
		auto texture = load2DTexture(path / path.stem() + suffix, format, flags);
		return texture ? texture.result() : nullptr;
	};

	auto material = new Material(&pbrMaterialDescriptor);
	material->path = path;

	if (auto texture = tryLoad("_Color.jpg", TextureFormat::any)) {
		material->setTexture("albedo", texture);
	}
	if (auto texture = tryLoad("_Emissive.jpg", TextureFormat::any)) {
		material->setTexture("emissive", texture);
	}
	if (auto texture = tryLoad("_Metalness.jpg", TextureFormat::rgba8)) {
		material->setTexture("metallic", texture);
		material->setFloat("metallic", 1);
	}
	if (auto texture = tryLoad("_Roughness.jpg", TextureFormat::rgba8)) {
		material->setTexture("roughness", texture);
		material->setFloat("roughness", 1);
	}
	if (auto texture = tryLoad("_AmbientOcclusion.jpg", TextureFormat::rgba8)) {
		material->setTexture("occlusion", texture);
	}
	if (auto texture = tryLoad("_NormalGL.jpg", TextureFormat::rgba8)) {
		material->setTexture("normal", texture);
	}
	return material;
#endif
}

Material* createEmissiveMaterial() {
	return new Material(&emissiveMaterialDescriptor);
}

Expected<Material*, FileioEx> loadEmissiveMaterial(CPath path) {

	auto texture = load2DTexture(path, TextureFormat::any, TextureFlags::default_);
	if (!texture) return texture.error();

	auto material = createEmissiveMaterial();
	material->path = path;

	material->setTexture("emissive", texture.result());

	return material;
}

} // namespace sgd
