/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef UNIT_DRAWER_H
#define UNIT_DRAWER_H

#include <vector>
#include <string>
#include <map>

#include "Rendering/GL/myGL.h"
#include "Rendering/GL/LightHandler.h"
#include "System/EventClient.h"

struct UnitDef;
class CWorldObject;
class IWorldObjectModelRenderer;
class CUnit;
class CFeature;
struct Command;
struct BuildInfo;
struct SolidObjectGroundDecal;
struct GhostSolidObject;
struct IUnitDrawerState;

namespace icon {
	class CIconData;
}
namespace GL {
	struct GeometryBuffer;
}

class CUnitDrawer: public CEventClient
{
public:
	// CEventClient interface
	bool WantsEvent(const std::string& eventName) {
		return
			eventName == "RenderUnitCreated"      || eventName == "RenderUnitDestroyed"  ||
			eventName == "UnitCloaked"            || eventName == "UnitDecloaked"        ||
			eventName == "UnitEnteredRadar"       || eventName == "UnitEnteredLos"       ||
			eventName == "UnitLeftRadar"          || eventName == "UnitLeftLos"          ||
			eventName == "PlayerChanged"          || eventName == "SunChanged";
	}
	bool GetFullRead() const { return true; }
	int GetReadAllyTeam() const { return AllAccessTeam; }

	void RenderUnitCreated(const CUnit*, int cloaked);
	void RenderUnitDestroyed(const CUnit*);

	void UnitEnteredRadar(const CUnit* unit, int allyTeam);
	void UnitEnteredLos(const CUnit* unit, int allyTeam);
	void UnitLeftRadar(const CUnit* unit, int allyTeam);
	void UnitLeftLos(const CUnit* unit, int allyTeam);

	void UnitCloaked(const CUnit* unit);
	void UnitDecloaked(const CUnit* unit);
	
	void PlayerChanged(int playerNum);
	void SunChanged(const float3& sunDir);

public:
	CUnitDrawer();
	~CUnitDrawer();

	void Update();

	void Draw(bool drawReflection, bool drawRefraction = false);
	void DrawOpaquePass(const CUnit* excludeUnit, bool deferredPass, bool drawReflection, bool drawRefraction);
	void DrawShadowPass();
	/// cloaked units must be drawn after all others
	void DrawCloakedUnits(bool noAdvShading = false);

	void SetDrawDeferredPass(bool b) { drawDeferred = b; }

	// note: make these static?
	void DrawUnitModel(const CUnit* unit);
	void DrawUnitRawModel(const CUnit* unit);
	void DrawUnitBeingBuilt(const CUnit* unit);

	void DrawUnitNoLists(const CUnit* unit);
	void DrawUnitWithLists(const CUnit* unit, unsigned int preList, unsigned int postList, bool luaCall);
	void DrawUnitRawNoLists(const CUnit* unit);
	void DrawUnitRawWithLists(const CUnit* unit, unsigned int preList, unsigned int postList, bool luaCall);

	void SetTeamColour(int team, float alpha = 1.0f) const;
	void SetupForUnitDrawing(bool deferredPass);
	void CleanUpUnitDrawing(bool deferredPass) const;
	void SetupForGhostDrawing() const;
	void CleanUpGhostDrawing() const;

	void SetUnitDrawDist(float dist);
	void SetUnitIconDist(float dist);

	bool ShowUnitBuildSquare(const BuildInfo& buildInfo);
	bool ShowUnitBuildSquare(const BuildInfo& buildInfo, const std::vector<Command>& commands);

	void CreateSpecularFace(unsigned int glType, int size, float3 baseDir, float3 xDif, float3 yDif, float3 sunDir, float exponent, float3 sunColor);

	static void DrawBuildingSample(const UnitDef* unitdef, int team, float3 pos, int facing = 0);
	static void DrawUnitDef(const UnitDef* unitDef, int team);

	/// Returns true if the given unit should be drawn as icon in the current frame.
	bool DrawAsIcon(const CUnit* unit, const float sqUnitCamDist) const;

	/** LuaOpenGL::Unit{Raw} **/
	void DrawIndividual(CUnit* unit);

	void DrawUnitMiniMapIcons() const;

	const std::vector<CUnit*>& GetUnsortedUnits() const { return unsortedUnits; }
	IWorldObjectModelRenderer* GetOpaqueModelRenderer(int modelType) { return opaqueModelRenderers[modelType]; }
	IWorldObjectModelRenderer* GetCloakedModelRenderer(int modelType) { return cloakedModelRenderers[modelType]; }

	const GL::LightHandler* GetLightHandler() const { return &lightHandler; }
	      GL::LightHandler* GetLightHandler()       { return &lightHandler; }

	const GL::GeometryBuffer* GetGeometryBuffer() const { return geomBuffer; }
	      GL::GeometryBuffer* GetGeometryBuffer()       { return geomBuffer; }

	bool DrawDeferredSupported() const;
	bool DrawDeferred() const { return drawDeferred; }

	bool UseAdvShading() const { return advShading; }
	bool UseAdvFading() const { return advFading; }

	bool& UseAdvShadingRef() { return advShading; }
	bool& UseAdvFadingRef() { return advFading; }

	void SetUseAdvShading(bool b) { advShading = b; }
	void SetUseAdvFading(bool b) { advFading = b; }


private:
	bool CanDrawOpaqueUnit(const CUnit* unit, const CUnit* excludeUnit, bool drawReflection, bool drawRefraction) const;
	bool CanDrawOpaqueUnitShadow(const CUnit* unit) const;

	void DrawOpaqueUnit(CUnit* unit, const CUnit* excludeUnit, bool drawReflection, bool drawRefraction);
	void DrawOpaqueUnitShadow(CUnit* unit);
	void DrawOpaqueUnitsShadow(int modelType);
	void DrawOpaqueUnits(int modelType, const CUnit* excludeUnit, bool drawReflection, bool drawRefraction);

	void DrawOpaqueAIUnits();
	void DrawCloakedAIUnits();
	void DrawGhostedBuildings(int modelType);

	void DrawUnitIcons(bool drawReflection);
	void DrawUnitMiniMapIcon(const CUnit* unit, CVertexArray* va) const;
	void UpdateUnitMiniMapIcon(const CUnit* unit, bool forced, bool killed);

	void UpdateUnitIconState(CUnit* unit);
	static void UpdateUnitDrawPos(CUnit* unit);

	static void DrawIcon(CUnit* unit, bool asRadarBlip);
	void DrawCloakedUnitsHelper(int modelType);
	void DrawCloakedUnit(CUnit* unit, int modelType, bool drawGhostBuildingsPass);

	void SelectRenderState(bool shaderPath) {
		unitDrawerState = shaderPath? unitDrawerStateSSP: unitDrawerStateFFP;
	}

public:
	static void SetupBasicS3OTexture0();
	static void SetupBasicS3OTexture1();
	static void CleanupBasicS3OTexture1();
	static void CleanupBasicS3OTexture0();


public:
	float unitDrawDist;
	float unitDrawDistSqr;
	float unitIconDist;
	float iconLength;

	float3 unitAmbientColor;
	float3 unitSunColor;

	struct TempDrawUnit {
		const UnitDef* unitdef;
		int team;
		float3 pos;
		float rotation;
		int facing;
		bool drawBorder;
	};
	std::multimap<int, TempDrawUnit> tempDrawUnits;
	std::multimap<int, TempDrawUnit> tempTransparentDrawUnits;

	float3 camNorm; ///< used to draw far-textures

private:
	bool advShading;
	bool advFading;
	bool drawDeferred;

	bool useDistToGroundForIcons;
	float sqCamDistToGroundForIcons;

	float cloakAlpha;
	float cloakAlpha1;
	float cloakAlpha2;
	float cloakAlpha3;

	std::vector<IWorldObjectModelRenderer*> opaqueModelRenderers;
	std::vector<IWorldObjectModelRenderer*> cloakedModelRenderers;

	/**
	 * units being rendered (note that this is a completely
	 * unsorted set of 3DO, S3O, opaque, and cloaked models!)
	 */
	std::vector<CUnit*> unsortedUnits;

	/// buildings that were in LOS_PREVLOS when they died and not in LOS since
	std::vector<std::vector<GhostSolidObject*> > deadGhostBuildings;
	/// buildings that left LOS but are still alive
	std::vector<std::vector<CUnit*> > liveGhostBuildings;

	std::vector<CUnit*> drawIcon;

	std::vector<std::vector<CUnit*> > unitRadarIcons;
	std::map<icon::CIconData*, std::vector<const CUnit*> > unitsByIcon;

	IUnitDrawerState* unitDrawerStateSSP; // default shader-driven rendering path
	IUnitDrawerState* unitDrawerStateFFP; // fallback shader-less rendering path
	IUnitDrawerState* unitDrawerState;    // currently selected state

	GL::LightHandler lightHandler;
	GL::GeometryBuffer* geomBuffer;
};

extern CUnitDrawer* unitDrawer;

#endif // UNIT_DRAWER_H
