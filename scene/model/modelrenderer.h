#pragma once

#include "model.h"

#include <graphics/exports.h>
#include <graphics/render/renderop.h>

namespace sgd {

SGD_SHARED(ModelRenderer);

struct ModelRenderer : Shared {
	SGD_OBJECT_TYPE(ModelRenderer, Shared);

	ModelRenderer();

	void add(CModel* model);
	void remove(CModel* model);

	void update(CVec3r eye);
	void render(RenderQueue* rq) const;

	// Experimental: build per-light shadow draw ops containing only instances inside each light sphere
	// (eye-relative xyz, radius w). Instances are compacted into a separate instance buffer.
	void cullShadowOps(CVector<Vec4f> spheres, Vector<Vector<RenderOp>>& out);

	BindGroup* opaqueBindGroup() const {
		return m_opaqueRenderer->bindGroup();
	}

private:
	Vec3r m_eye;
	MeshRendererPtr m_shadowRenderer;

	struct OpaqueInstanceList {
		Vector<CModel*> models;
	};

	struct BlendedInstance {
		CModel* model;
		float distance{};
		explicit BlendedInstance(CModel* model):model(model){}
	};

	struct SurfaceInstance {
		CSurface* surface;
		float distance{};
		SurfaceInstance(CSurface* surface,float distance):surface(surface),distance(distance){}
	};

	uint32_t m_numOpaqueInsts{};
	uint32_t m_numBlendedInsts{};

	MeshRendererPtr m_opaqueRenderer;
	MeshRendererPtr m_blendedRenderer;

	Map<CMesh*, UniquePtr<OpaqueInstanceList>> m_opaqueLists;

	Vector<BlendedInstance> m_blendedInsts;

	Vector<SurfaceInstance> m_surfaceInsts;

	Vector<CSurface*> m_blendedSurfs;
};

}
