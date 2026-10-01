#include "modelrenderer.h"

#include "model.h"

#include <graphics/render/renderqueue.h>
#include <cstring>

namespace sgd {

namespace {}

ModelRenderer::ModelRenderer()
	:										  //
	  m_opaqueRenderer(new MeshRenderer()),	  //
	  m_blendedRenderer(new MeshRenderer()),  //
	  m_shadowRenderer(new MeshRenderer()) {  //
}

void ModelRenderer::add(CModel* model) {

	auto mesh = model->mesh();
	if (!mesh) {
		model->mesh.changed.connect(this, [=](CMesh*) { //
			model->mesh.changed.disconnect(this);
			add(model);
		});
		return;
	}

	// Always add to opaque list for now
	auto it = m_opaqueLists.find(mesh);
	if (it == m_opaqueLists.end()) {
		it = m_opaqueLists.insert(std::make_pair(mesh, new OpaqueInstanceList())).first;
	}
	auto opaqueList = it->second.get();
	opaqueList->models.push_back(model);
	++m_numOpaqueInsts;

	if (!bool(mesh->flags() & MeshFlags::blendedSurfaces)) {
		model->mesh.changed.connect(this, [=](CMesh*) {
			model->mesh.changed.disconnect(this);
			sgd::remove(opaqueList->models, model); // remove from opaque list only...
			--m_numOpaqueInsts;
			add(model);
		});
		return;
	}

	// Add to blended list
	m_blendedInsts.emplace_back(model);
	++m_numBlendedInsts;

	model->mesh.changed.connect(this, [=](CMesh*) {
		model->mesh.changed.disconnect(this);
		// Remove from opaque list
		sgd::remove(opaqueList->models, model);
		--m_numOpaqueInsts;
		// Remove from blended list
		for (auto it = m_blendedInsts.begin(); it != m_blendedInsts.end(); ++it) {
			if (it->model != model) continue;
			m_blendedInsts.erase(it);
			--m_numBlendedInsts;
			add(model);
			return;
		}
		SGD_ABORT();
	});
}

void ModelRenderer::remove(CModel* model) {

	model->mesh.changed.disconnect(this);

	auto mesh = model->mesh();
	if (!mesh) return;

	// Remove from opaque list
	auto opaqueList = m_opaqueLists.find(mesh)->second.get();
	sgd::remove(opaqueList->models, model);
	--m_numOpaqueInsts;

	if (!bool(mesh->flags() & MeshFlags::blendedSurfaces)) return;

	// Remove from blended list
	for (auto it = m_blendedInsts.begin(); it != m_blendedInsts.end(); ++it) {
		if (it->model != model) continue;
		m_blendedInsts.erase(it);
		--m_numBlendedInsts;
		return;
	}
	SGD_ABORT();
}

void ModelRenderer::update(CVec3r eye) {

	m_eye = eye;

	// Update opaque instances
	if (!m_opaqueLists.empty()) {

		auto instp = m_opaqueRenderer->lockInstances(m_numOpaqueInsts);

		for (auto& kv : m_opaqueLists) {
			auto list = kv.second.get();
			for (CModel* model : list->models) {
				instp->worldMatrix = model->worldMatrix();
				instp->worldMatrix.t.xyz() -= eye;
				instp->color = model->pmColor();
				++instp;
			}
		}
		m_opaqueRenderer->unlockInstances();
	}

	if (!m_blendedInsts.empty()) {

		for (auto& inst : m_blendedInsts) {
			inst.distance = (float)length(inst.model->worldMatrix() * Vec3r(inst.model->mesh()->origin()) - eye);
		}

		std::sort(m_blendedInsts.begin(), m_blendedInsts.end(), [](const BlendedInstance& lhs, const BlendedInstance& rhs) { //
			return lhs.distance > rhs.distance;
		});

		m_blendedSurfs.clear();

		auto instp = m_blendedRenderer->lockInstances(m_blendedInsts.size());

		for (auto& inst : m_blendedInsts) {

			instp->worldMatrix = inst.model->worldMatrix();
			instp->worldMatrix.t.xyz() -= eye;
			instp->color = inst.model->pmColor();
			++instp;

			m_surfaceInsts.clear();
			for (CSurface* surf : inst.model->mesh()->surfaces()) {
				if (surf->material()->blendMode() != BlendMode::alphaBlend) continue;
				float distance = (float)length(inst.model->worldMatrix() * Vec3r(surf->origin()) - eye);
				m_surfaceInsts.emplace_back(surf, distance);
			}

			// Sort surfaces within model instance bounds only
			std::sort(m_surfaceInsts.begin(), m_surfaceInsts.end(),
					  [](const SurfaceInstance& lhs, const SurfaceInstance& rhs) { //
						  return lhs.distance > rhs.distance;
					  });

			for (auto& surf : m_surfaceInsts) {
				m_blendedSurfs.push_back(surf.surface);
			}
			m_blendedSurfs.push_back(nullptr);
		}
		m_blendedRenderer->unlockInstances();
	}
}

void ModelRenderer::render(RenderQueue* rq) const {

	//  Render opaque
	{
		uint32_t first = 0;
		for (auto& kv : m_opaqueLists) {
			auto mesh = kv.first;
			uint32_t count = kv.second->models.size();
			if(!count) continue;
			if (bool(mesh->flags() & MeshFlags::blendedSurfaces)) {
				for (Surface* surf : mesh->surfaces()) {
					if (surf->material()->blendMode() == BlendMode::alphaBlend) continue;
					m_opaqueRenderer->render(rq, surf, count, first);
				}
			} else {
				m_opaqueRenderer->render(rq, mesh, count, first);
			}
			first += count;
		}
	}

	if (m_blendedSurfs.empty()) return;

	// Render blended
	{
		uint32_t index = 0;
		for (auto surf : m_blendedSurfs) {
			if (surf) {
				m_blendedRenderer->render(rq, surf, 1, index);
			} else {
				++index;
			}
		}
	}
}

void ModelRenderer::cullShadowOps(CVector<Vec4f> spheres, Vector<Vector<RenderOp>>& out) {

	static RenderQueuePtr scratch(new RenderQueue());

	out.clear();
	out.resize(spheres.size());

	// Once per frame: instance data + world bounding sphere for every opaque model, grouped by mesh.
	struct Cand {
		MeshInstance inst;
		Vec4f sphere; // world center (eye-relative) xyz, radius w
	};
	struct MeshCands {
		CMesh* mesh;
		Vector<Cand> cands;
	};
	Vector<MeshCands> groups;
	groups.reserve(m_opaqueLists.size());
	for (auto& kv : m_opaqueLists) {
		CMesh* mesh = kv.first;
		if (!mesh->shadowsEnabled()) continue;
		groups.emplace_back();
		auto& g = groups.back();
		g.mesh = mesh;
		g.cands.reserve(kv.second->models.size());

		Boxf b = mesh->bounds();
		if (b.min.x > b.max.x) { // never computed
			const_cast<Mesh*>(mesh)->updateBounds();
			b = mesh->bounds();
		}
		Vec3f lc = (b.min + b.max) * 0.5f;
		float lr = length(b.max - b.min) * 0.5f;

		for (CModel* model : kv.second->models) {
			Cand c;
			c.inst.worldMatrix = model->worldMatrix();
			c.inst.worldMatrix.t.xyz() -= m_eye;
			c.inst.color = model->pmColor();
			Vec3f ax = c.inst.worldMatrix.i.xyz(), ay = c.inst.worldMatrix.j.xyz(), az = c.inst.worldMatrix.k.xyz();
			Vec3f wc = c.inst.worldMatrix.t.xyz() + ax * lc.x + ay * lc.y + az * lc.z;
			c.sphere = Vec4f(wc, lr * std::max(length(ax), std::max(length(ay), length(az))));
			g.cands.push_back(c);
		}
	}

	Vector<MeshInstance> insts;
	insts.reserve(m_numOpaqueInsts);

	for (size_t si = 0; si < spheres.size(); ++si) {

		Vec3f lightPos = spheres[si].xyz();
		float lightRadius = spheres[si].w;

		for (auto& g : groups) {

			uint32_t first = (uint32_t)insts.size();

			for (auto& c : g.cands) {
				float dx = c.sphere.x - lightPos.x, dy = c.sphere.y - lightPos.y, dz = c.sphere.z - lightPos.z;
				float reach = lightRadius + c.sphere.w;
				if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
				insts.push_back(c.inst);
			}

			uint32_t count = (uint32_t)insts.size() - first;
			if (!count) continue;

			CMesh* mesh = g.mesh;
			scratch->clear();
			if (bool(mesh->flags() & MeshFlags::blendedSurfaces)) {
				for (Surface* surf : mesh->surfaces()) {
					if (surf->material()->blendMode() == BlendMode::alphaBlend) continue;
					m_shadowRenderer->render(scratch, surf, count, first);
				}
			} else {
				m_shadowRenderer->render(scratch, mesh, count, first);
			}
			for (auto& op : scratch->renderOps(RenderPassType::shadow)) out[si].push_back(op);
		}
	}

	auto instp = m_shadowRenderer->lockInstances(std::max<uint32_t>(1, (uint32_t)insts.size()));
	if (!insts.empty()) std::memcpy(instp, insts.data(), insts.size() * sizeof(MeshInstance));
	m_shadowRenderer->unlockInstances();
}

} // namespace sgd
