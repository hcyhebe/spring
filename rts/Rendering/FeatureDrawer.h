/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef FEATUREDRAWER_H_
#define FEATUREDRAWER_H_

#include <vector>
#include <array>
#include "System/creg/creg_cond.h"
#include "System/EventClient.h"
#include "Rendering/Models/WorldObjectModelRenderer.h"

class CFeature;
class IWorldObjectModelRenderer;


class CFeatureDrawer: public CEventClient
{
	CR_DECLARE_STRUCT(CFeatureDrawer)
	typedef std::vector<CFeature*>   FeatureSet;
	typedef std::array<IWorldObjectModelRenderer*, MODELTYPE_OTHER> quadRenderers;

public:
	CFeatureDrawer();
	~CFeatureDrawer();

	void UpdateDrawQuad(CFeature* feature);
	void Update();

	void Draw();
	void DrawShadowPass();

	void DrawFadeFeatures(bool noAdvShading = false);

	bool WantsEvent(const std::string& eventName) {
		return (eventName == "RenderFeatureCreated" || eventName == "RenderFeatureDestroyed" || eventName == "FeatureMoved");
	}
	bool GetFullRead() const { return true; }
	int GetReadAllyTeam() const { return AllAccessTeam; }

	virtual void RenderFeatureCreated(const CFeature* feature);
	virtual void RenderFeatureDestroyed(const CFeature* feature);
	virtual void FeatureMoved(const CFeature* feature, const float3& oldpos);

private:
	static void UpdateDrawPos(CFeature* f);

	void DrawOpaqueFeatures(int);
	void DrawFarFeatures();
	bool DrawFeatureNow(const CFeature*, float alpha = 0.99f);
	void DrawFadeFeaturesHelper(int);
	void DrawFadeFeaturesSet(const FeatureSet&, int);
	void GetVisibleFeatures(int, bool drawFar);

	void PostLoad();

private:
	std::vector<CFeature*> unsortedFeatures;

	int drawQuadsX;
	int drawQuadsY;

	float farDist;
	float featureDrawDistance;
	float featureFadeDistance;

	std::vector<std::pair<quadRenderers, bool>> modelRenderers;

	friend class CFeatureQuadDrawer;
};

extern CFeatureDrawer* featureDrawer;


#endif /* FEATUREDRAWER_H_ */
