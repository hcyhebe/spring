/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <algorithm>
#include <cctype>

#include "GroundDecalHandler.h"
#include "Game/Camera.h"
#include "Game/GameSetup.h"
#include "Game/GlobalUnsynced.h"
#include "Lua/LuaParser.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/Map/InfoTexture/IInfoTextureHandler.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Textures/Bitmap.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Projectiles/ExplosionListener.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/Exceptions.h"
#include "System/Log/ILog.h"
#include "System/myMath.h"
#include "System/Util.h"
#include "System/FileSystem/FileSystem.h"

using std::min;
using std::max;


CONFIG(int, GroundScarAlphaFade).defaultValue(0);

CGroundDecalHandler::CGroundDecalHandler()
	: CEventClient("[CGroundDecalHandler]", 314159, false)
{
	scarField = NULL;
	if (!GetDrawDecals())
		return;

	eventHandler.AddClient(this);
	CExplosionCreator::AddExplosionListener(this);

	groundScarAlphaFade = (configHandler->GetInt("GroundScarAlphaFade") != 0);

	unsigned char buf[512*512*4] = {0};

	LuaParser resourcesParser("gamedata/resources.lua",
	                          SPRING_VFS_MOD_BASE, SPRING_VFS_ZIP);
	if (!resourcesParser.Execute()) {
		LOG_L(L_ERROR, "Failed to load resources: %s",
				resourcesParser.GetErrorLog().c_str());
	}

	const LuaTable scarsTable = resourcesParser.GetRoot().SubTable("graphics").SubTable("scars");
	LoadScar("bitmaps/" + scarsTable.GetString(2, "scars/scar2.bmp"), buf, 0,   0);
	LoadScar("bitmaps/" + scarsTable.GetString(3, "scars/scar3.bmp"), buf, 256, 0);
	LoadScar("bitmaps/" + scarsTable.GetString(1, "scars/scar1.bmp"), buf, 0,   256);
	LoadScar("bitmaps/" + scarsTable.GetString(4, "scars/scar4.bmp"), buf, 256, 256);

	glGenTextures(1, &scarTex);
	glBindTexture(GL_TEXTURE_2D, scarTex);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_NEAREST);
	glBuildMipmaps(GL_TEXTURE_2D,GL_RGBA8 ,512, 512, GL_RGBA, GL_UNSIGNED_BYTE, buf);

	scarFieldX=mapDims.mapx/32;
	scarFieldY=mapDims.mapy/32;
	scarField=new std::vector<Scar*>[scarFieldX*scarFieldY];

	lastTest=0;
	maxOverlap=decalLevel+1;

	LoadDecalShaders();
}



CGroundDecalHandler::~CGroundDecalHandler()
{
	eventHandler.RemoveClient(this);

	for (TrackType* tt: trackTypes) {
		for (UnitTrackStruct* uts: tt->tracks) {
			delete uts;
		}
		glDeleteTextures(1, &tt->texture);
		delete tt;
	}
	for (std::vector<TrackToAdd>::iterator ti = tracksToBeAdded.begin(); ti != tracksToBeAdded.end(); ++ti) {
		delete (*ti).tp;
		if ((*ti).unit == NULL)
			tracksToBeDeleted.push_back((*ti).ts);
	}
	for (std::vector<UnitTrackStruct *>::iterator ti = tracksToBeDeleted.begin(); ti != tracksToBeDeleted.end(); ++ti) {
		delete *ti;
	}

	for (SolidObjectDecalType* dctype: objectDecalTypes) {
		for (SolidObjectGroundDecal* dc: dctype->objectDecals) {
			if (dc->owner)
				dc->owner->groundDecal = nullptr;
			if (dc->gbOwner)
				dc->gbOwner->decal = nullptr;
			delete dc;
		}
		glDeleteTextures(1, &dctype->texture);
		delete dctype;
	}
	for (auto &scar: scars) {
		delete scar;
	}
	for (auto &scar: scarsToBeAdded) {
		delete scar;
	}
	if (scarField != NULL) {
		delete[] scarField;

		glDeleteTextures(1, &scarTex);
	}

	shaderHandler->ReleaseProgramObjects("[GroundDecalHandler]");
	decalShaders.clear();
}

void CGroundDecalHandler::LoadDecalShaders() {
	#define sh shaderHandler
	decalShaders.resize(DECAL_SHADER_LAST, NULL);

	// SM3 maps have no baked lighting, so decals blend differently
	const bool haveShadingTexture = (readMap->GetShadingTexture() != 0);
	const char* fragmentProgramNameARB = haveShadingTexture?
		"ARB/GroundDecalsSMF.fp":
		"ARB/GroundDecalsSM3.fp";
	const std::string extraDef = haveShadingTexture?
		"#define HAVE_SHADING_TEX 1\n":
		"#define HAVE_SHADING_TEX 0\n";

	decalShaders[DECAL_SHADER_ARB ] = sh->CreateProgramObject("[GroundDecalHandler]", "DecalShaderARB",  true);
	decalShaders[DECAL_SHADER_GLSL] = sh->CreateProgramObject("[GroundDecalHandler]", "DecalShaderGLSL", false);
	decalShaders[DECAL_SHADER_CURR] = decalShaders[DECAL_SHADER_ARB];

	if (globalRendering->haveARB && !globalRendering->haveGLSL) {
		decalShaders[DECAL_SHADER_ARB]->AttachShaderObject(sh->CreateShaderObject("ARB/GroundDecals.vp", "", GL_VERTEX_PROGRAM_ARB));
		decalShaders[DECAL_SHADER_ARB]->AttachShaderObject(sh->CreateShaderObject(fragmentProgramNameARB, "", GL_FRAGMENT_PROGRAM_ARB));
		decalShaders[DECAL_SHADER_ARB]->Link();
	} else {
		if (globalRendering->haveGLSL) {
			decalShaders[DECAL_SHADER_GLSL]->AttachShaderObject(sh->CreateShaderObject("GLSL/GroundDecalsVertProg.glsl", "",       GL_VERTEX_SHADER));
			decalShaders[DECAL_SHADER_GLSL]->AttachShaderObject(sh->CreateShaderObject("GLSL/GroundDecalsFragProg.glsl", extraDef, GL_FRAGMENT_SHADER));
			decalShaders[DECAL_SHADER_GLSL]->Link();

			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("decalTex");           // idx 0
			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("shadeTex");           // idx 1
			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("shadowTex");          // idx 2
			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("mapSizePO2");         // idx 3
			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("groundAmbientColor"); // idx 4
			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("shadowMatrix");       // idx 5
			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("shadowParams");       // idx 6
			decalShaders[DECAL_SHADER_GLSL]->SetUniformLocation("shadowDensity");      // idx 7

			decalShaders[DECAL_SHADER_GLSL]->Enable();
			decalShaders[DECAL_SHADER_GLSL]->SetUniform1i(0, 0); // decalTex  (idx 0, texunit 0)
			decalShaders[DECAL_SHADER_GLSL]->SetUniform1i(1, 1); // shadeTex  (idx 1, texunit 1)
			decalShaders[DECAL_SHADER_GLSL]->SetUniform1i(2, 2); // shadowTex (idx 2, texunit 2)
			decalShaders[DECAL_SHADER_GLSL]->SetUniform2f(3, 1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE));
			decalShaders[DECAL_SHADER_GLSL]->SetUniform1f(7, sky->GetLight()->GetGroundShadowDensity());
			decalShaders[DECAL_SHADER_GLSL]->Disable();
			decalShaders[DECAL_SHADER_GLSL]->Validate();

			decalShaders[DECAL_SHADER_CURR] = decalShaders[DECAL_SHADER_GLSL];
		}
	}

	#undef sh
}

void CGroundDecalHandler::SunChanged(const float3& sunDir) {
	if (globalRendering->haveGLSL && decalShaders.size() > DECAL_SHADER_GLSL) {
		decalShaders[DECAL_SHADER_GLSL]->Enable();
		decalShaders[DECAL_SHADER_GLSL]->SetUniform1f(7, sky->GetLight()->GetGroundShadowDensity());
		decalShaders[DECAL_SHADER_GLSL]->Disable();
	}
}

static inline void AddQuadVertices(CVertexArray* va, int x, float* yv, int z, const float* uv, unsigned char* color)
{
	#define HEIGHT2WORLD(x) ((x) << 3)
	#define VERTEX(x, y, z) float3(HEIGHT2WORLD((x)), (y), HEIGHT2WORLD((z)))
	va->AddVertexTC( VERTEX(x    , yv[0], z    ),   uv[0], uv[1],   color);
	va->AddVertexTC( VERTEX(x + 1, yv[1], z    ),   uv[2], uv[3],   color);
	va->AddVertexTC( VERTEX(x + 1, yv[2], z + 1),   uv[4], uv[5],   color);
	va->AddVertexTC( VERTEX(x    , yv[3], z + 1),   uv[6], uv[7],   color);
	#undef VERTEX
	#undef HEIGHT2WORLD
}


inline void CGroundDecalHandler::DrawObjectDecal(SolidObjectGroundDecal* decal)
{
	const float* hm = readMap->GetCornerHeightMapUnsynced();
	const int gsmx = mapDims.mapx;
	const int gsmx1 = gsmx + 1;
	const int gsmy = mapDims.mapy;

	SColor color(255, 255, 255, int(decal->alpha * 255));

	#ifndef DEBUG
	#define HEIGHT(z, x) (hm[((z) * gsmx1) + (x)])
	#else
	#define HEIGHT(z, x) (assert((z) <= gsmy), assert((x) <= gsmx), (hm[((z) * gsmx1) + (x)]))
	#endif

	if (!decal->va) {
		// NOTE: this really needs CLOD'ing
		decal->va = new CVertexArray();
		decal->va->Initialize();

		const int
			dxsize = decal->xsize,
			dzsize = decal->ysize,
			dxpos  = decal->posx,              // top-left quad x-coordinate
			dzpos  = decal->posy,              // top-left quad z-coordinate
			dxoff  = (dxpos < 0)? -(dxpos): 0, // offset from left map edge
			dzoff  = (dzpos < 0)? -(dzpos): 0; // offset from top map edge

		const float xts = 1.0f / dxsize;
		const float zts = 1.0f / dzsize;

		float yv[4] = {0.0f}; // heights at each sub-quad vertex (tl, tr, br, bl)
		float uv[8] = {0.0f}; // tex-coors at each sub-quad vertex

		// clipped decal dimensions
		int cxsize = dxsize - dxoff;
		int czsize = dzsize - dzoff;

		if ((dxpos + dxsize) > gsmx) { cxsize -= ((dxpos + dxsize) - gsmx); }
		if ((dzpos + dzsize) > gsmy) { czsize -= ((dzpos + dzsize) - gsmy); }

		for (int vx = 0; vx < cxsize; vx++) {
			for (int vz = 0; vz < czsize; vz++) {
				const int rx = dxoff + vx;  // x-coor in decal-space
				const int rz = dzoff + vz;  // z-coor in decal-space
				const int px = dxpos + rx;  // x-coor in heightmap-space
				const int pz = dzpos + rz;  // z-coor in heightmap-space

				yv[0] = HEIGHT(pz,     px    ); yv[1] = HEIGHT(pz,     px + 1);
				yv[2] = HEIGHT(pz + 1, px + 1); yv[3] = HEIGHT(pz + 1, px    );

				switch (decal->facing) {
					case FACING_SOUTH: {
						uv[0] = (rx    ) * xts; uv[1] = (rz    ) * zts; // uv = (0, 0)
						uv[2] = (rx + 1) * xts; uv[3] = (rz    ) * zts; // uv = (1, 0)
						uv[4] = (rx + 1) * xts; uv[5] = (rz + 1) * zts; // uv = (1, 1)
						uv[6] = (rx    ) * xts; uv[7] = (rz + 1) * zts; // uv = (0, 1)
					} break;
					case FACING_NORTH: {
						uv[0] = (dxsize - rx    ) * xts; uv[1] = (dzsize - rz    ) * zts; // uv = (1, 1)
						uv[2] = (dxsize - rx - 1) * xts; uv[3] = (dzsize - rz    ) * zts; // uv = (0, 1)
						uv[4] = (dxsize - rx - 1) * xts; uv[5] = (dzsize - rz - 1) * zts; // uv = (0, 0)
						uv[6] = (dxsize - rx    ) * xts; uv[7] = (dzsize - rz - 1) * zts; // uv = (1, 0)
					} break;

					case FACING_EAST: {
						uv[0] = 1.0f - (rz    ) * zts; uv[1] = (rx    ) * xts; // uv = (1, 0)
						uv[2] = 1.0f - (rz    ) * zts; uv[3] = (rx + 1) * xts; // uv = (1, 1)
						uv[4] = 1.0f - (rz + 1) * zts; uv[5] = (rx + 1) * xts; // uv = (0, 1)
						uv[6] = 1.0f - (rz + 1) * zts; uv[7] = (rx    ) * xts; // uv = (0, 0)
					} break;
					case FACING_WEST: {
						uv[0] = (rz    ) * zts; uv[1] = 1.0f - (rx    ) * xts; // uv = (0, 1)
						uv[2] = (rz    ) * zts; uv[3] = 1.0f - (rx + 1) * xts; // uv = (0, 0)
						uv[4] = (rz + 1) * zts; uv[5] = 1.0f - (rx + 1) * xts; // uv = (1, 0)
						uv[6] = (rz + 1) * zts; uv[7] = 1.0f - (rx    ) * xts; // uv = (1, 1)
					} break;
				}

				AddQuadVertices(decal->va, px, yv, pz, uv, color);
			}
		}
	} else {
		const int num = decal->va->drawIndex() / VA_SIZE_TC;
		decal->va->ResetPos();
		VA_TYPE_TC* mem = decal->va->GetTypedVertexArray<VA_TYPE_TC>(num);

		for (int i = 0; i < num; ++i) {
			const int x = int(mem[i].p.x) >> 3;
			const int z = int(mem[i].p.z) >> 3;

			// update the height and alpha
			mem[i].p.y = hm[z * gsmx1 + x];
			mem[i].c   = color;
		}

		decal->va->DrawArrayTC(GL_QUADS);
	}

	#undef HEIGHT
}


inline void CGroundDecalHandler::DrawGroundScar(CGroundDecalHandler::Scar* scar, bool fade)
{
	// TODO: do we want LOS-checks for decals?
	if (!camera->InView(scar->pos, scar->radius + 16))
		return;

	SColor color(255, 255, 255, 255);

	if (scar->va == NULL) {
		scar->va = new CVertexArray();
		scar->va->Initialize();

		float3 pos = scar->pos;
		const float radius = scar->radius;
		const float radius4 = radius * 4.0f;
		const float tx = scar->texOffsetX;
		const float ty = scar->texOffsetY;

		int sx = (int) max(0.0f,                 (pos.x - radius) * 0.0625f);
		int ex = (int) min(float(mapDims.hmapx - 1), (pos.x + radius) * 0.0625f);
		int sz = (int) max(0.0f,                 (pos.z - radius) * 0.0625f);
		int ez = (int) min(float(mapDims.hmapy - 1), (pos.z + radius) * 0.0625f);

		// create the scar texture-quads
		float px1 = sx * 16;
		for (int x = sx; x <= ex; ++x) {
			float px2 = px1 + 16;
			float pz1 = sz * 16;

			for (int z = sz; z <= ez; ++z) {
				float pz2 = pz1 + 16;
				float tx1 = min(0.5f, (pos.x - px1) / radius4 + 0.25f);
				float tx2 = max(0.0f, (pos.x - px2) / radius4 + 0.25f);
				float tz1 = min(0.5f, (pos.z - pz1) / radius4 + 0.25f);
				float tz2 = max(0.0f, (pos.z - pz2) / radius4 + 0.25f);
				float h1 = CGround::GetHeightReal(px1, pz1, false);
				float h2 = CGround::GetHeightReal(px2, pz1, false);
				float h3 = CGround::GetHeightReal(px2, pz2, false);
				float h4 = CGround::GetHeightReal(px1, pz2, false);

				scar->va->AddVertexTC(float3(px1, h1, pz1), tx1 + tx, tz1 + ty, color);
				scar->va->AddVertexTC(float3(px2, h2, pz1), tx2 + tx, tz1 + ty, color);
				scar->va->AddVertexTC(float3(px2, h3, pz2), tx2 + tx, tz2 + ty, color);
				scar->va->AddVertexTC(float3(px1, h4, pz2), tx1 + tx, tz2 + ty, color);
				pz1 = pz2;
			}

			px1 = px2;
		}
	} else {
		if (fade) {
			if ((scar->creationTime + 10) > gs->frameNum) {
				color[3] = (int) (scar->startAlpha * (gs->frameNum - scar->creationTime) * 0.1f);
			} else {
				color[3] = (int) (scar->startAlpha - (gs->frameNum - scar->creationTime) * scar->alphaFalloff);
			}

			const int gsmx1 = mapDims.mapx + 1;
			const float* hm = readMap->GetCornerHeightMapUnsynced();

			const int num = scar->va->drawIndex() / VA_SIZE_TC;
			scar->va->ResetPos();
			VA_TYPE_TC* mem = scar->va->GetTypedVertexArray<VA_TYPE_TC>(num);

			for (int i = 0; i < num; ++i) {
				const int x = int(mem[i].p.x) >> 3;
				const int z = int(mem[i].p.z) >> 3;

				// update the height and alpha
				mem[i].p.y = hm[z * gsmx1 + x];
				mem[i].c   = color;
			}
		}

		scar->va->DrawArrayTC(GL_QUADS);
	}
}



void CGroundDecalHandler::GatherDecalsForType(CGroundDecalHandler::SolidObjectDecalType* decalType) {
	decalsToDraw.clear();

	auto &objectDecals = decalType->objectDecals;

	for (int i = 0; i < objectDecals.size();) {
		SolidObjectGroundDecal* decal = objectDecals[i];
		CSolidObject* decalOwner = decal->owner;

		if (decalOwner == NULL) {
			if (decal->gbOwner == NULL) {
				decal->alpha -= (decal->alphaFalloff * globalRendering->lastFrameTime * 0.001f * gs->speedFactor);
			}
			if (decal->alpha < 0.0f) {
			// make sure RemoveSolidObject() won't try to modify this decal
				if (decalOwner != NULL) {
					decalOwner->groundDecal = NULL;
				}

				objectDecals[i] = objectDecals.back();
				objectDecals.pop_back();

				delete decal;
				continue;
			}
			++i;
		} else {
			++i;
			if (decalOwner->GetBlockingMapID() < unitHandler->MaxUnits()) {
				const CUnit* decalOwnerUnit = static_cast<const CUnit*>(decalOwner);
				if (decalOwnerUnit->isIcon)
					continue;
				if ((decalOwnerUnit->losStatus[gu->myAllyTeam] & LOS_INLOS) == 0 && !gu->spectatingFullView)
					continue;
				if (!gameSetup->ghostedBuildings || (decalOwnerUnit->losStatus[gu->myAllyTeam] & LOS_PREVLOS) == 0)
					continue;
				decal->alpha = std::max(0.0f, decalOwnerUnit->buildProgress);
			} else {
				const CFeature* decalOwnerFeature = static_cast<const CFeature*>(decalOwner);
				if (!decalOwnerFeature->IsInLosForAllyTeam(gu->myAllyTeam))
					continue;
				if (decalOwnerFeature->drawAlpha < 0.01f)
					continue;
				decal->alpha = decalOwnerFeature->drawAlpha;
			}
		}
		if (!camera->InView(decal->pos, decal->radius))
			continue;
		decalsToDraw.push_back(decal);
	}
}

void CGroundDecalHandler::DrawObjectDecals() {
	// create and draw the quads for each building decal
	for (SolidObjectDecalType* decalType: objectDecalTypes) {

		if (decalType->objectDecals.empty())
			continue;

		{
			GatherDecalsForType(decalType);
		}

		if (decalsToDraw.size() > 0) {
			glBindTexture(GL_TEXTURE_2D, decalType->texture);
			for (SolidObjectGroundDecal* decal: decalsToDraw) {
				DrawObjectDecal(decal);
			}
		}

		// glBindTexture(GL_TEXTURE_2D, 0);
	}
}



void CGroundDecalHandler::AddTracks() {
	{
		// Delayed addition of new tracks
		for (TrackToAdd &tta: tracksToBeAdded) {

			if (tta.ts->owner == NULL) {
				delete tta.tp;

				if (tta.unit == NULL)
					tracksToBeDeleted.push_back(tta.ts);

				continue; // unit removed
			}

			const CUnit* unit = tta.unit;

			if (unit == NULL) {
				unit = tta.ts->owner;
				auto &tracks = trackTypes[unit->unitDef->decalDef.trackDecalType]->tracks;
				assert(std::find(tracks.begin(), tracks.end(), tta.ts) == tracks.end());
				tracks.push_back(tta.ts);
			}

			TrackPart* tp = tta.tp;

			// if the unit is moving in a straight line only place marks at half the rate by replacing old ones
			bool replace = false;

			if (unit->myTrack->parts.size() > 1) {
				std::deque<TrackPart *>::iterator pi = --unit->myTrack->parts.end();
				std::deque<TrackPart *>::iterator pi2 = pi--;

				replace = (((tp->pos1 + (*pi)->pos1) * 0.5f).SqDistance((*pi2)->pos1) < 1.0f);
			}

			if (replace) {
				delete unit->myTrack->parts.back();
				unit->myTrack->parts.back() = tp;
			} else {
				unit->myTrack->parts.push_back(tp);
			}
		}

		tracksToBeAdded.clear();
	}

	for (UnitTrackStruct *uts: tracksToBeDeleted) {
		delete uts;
	}

	tracksToBeDeleted.clear();
	tracksToBeCleaned.clear();
}

void CGroundDecalHandler::DrawTracks() {
	unsigned char curPartColor[4] = {255, 255, 255, 255};
	unsigned char nxtPartColor[4] = {255, 255, 255, 255};

	// create and draw the unit footprint quads
	for (TrackType* tt: trackTypes) {

		if (tt->tracks.empty())
			continue;


		CVertexArray* va = GetVertexArray();
		va->Initialize();
		glBindTexture(GL_TEXTURE_2D, tt->texture);

		for (UnitTrackStruct* track: tt->tracks) {

			if (track->parts.empty()) {
				tracksToBeCleaned.push_back(TrackToClean(track, &(tt->tracks)));
				continue;
			}

			if (gs->frameNum > (track->parts.front()->creationTime + track->lifeTime)) {
				tracksToBeCleaned.push_back(TrackToClean(track, &(tt->tracks)));
				// still draw the track to avoid flicker
				// continue;
			}

			const auto frontPart = track->parts.front();
			const auto backPart = track->parts.back();

			if (!camera->InView((frontPart->pos1 + backPart->pos1) * 0.5f, frontPart->pos1.distance(backPart->pos1) + 500.0f))
				continue;

			// walk across the track parts from front (oldest) to back (newest) and draw
			// a quad between "connected" parts (ie. parts differing 8 sim-frames in age)
			std::deque<TrackPart*>::const_iterator curPart =   (track->parts.begin());
			std::deque<TrackPart*>::const_iterator nxtPart = ++(track->parts.begin());

			curPartColor[3] = std::max(0.0f, (1.0f - (gs->frameNum - (*curPart)->creationTime) * track->alphaFalloff) * 255.0f);

			va->EnlargeArrays(track->parts.size() * 4, 0, VA_SIZE_TC);

			for (; nxtPart != track->parts.end(); ++nxtPart) {
				nxtPartColor[3] = std::max(0.0f, (1.0f - (gs->frameNum - (*nxtPart)->creationTime) * track->alphaFalloff) * 255.0f);

				if ((*nxtPart)->connected) {
					va->AddVertexQTC((*curPart)->pos1, (*curPart)->texPos, 0, curPartColor);
					va->AddVertexQTC((*curPart)->pos2, (*curPart)->texPos, 1, curPartColor);
					va->AddVertexQTC((*nxtPart)->pos2, (*nxtPart)->texPos, 1, nxtPartColor);
					va->AddVertexQTC((*nxtPart)->pos1, (*nxtPart)->texPos, 0, nxtPartColor);
				}

				curPartColor[3] = nxtPartColor[3];
				curPart = nxtPart;
			}
		}

		va->DrawArrayTC(GL_QUADS);
	}
}

void CGroundDecalHandler::CleanTracks()
{
	// Cleanup old tracks
	for (TrackToClean &ttc: tracksToBeCleaned) {
		UnitTrackStruct* track = ttc.track;

		while (!track->parts.empty()) {
			// stop at the first part that is still too young for deletion
			if (gs->frameNum < (track->parts.front()->creationTime + track->lifeTime))
				break;

			delete track->parts.front();
			track->parts.pop_front();
		}

		if (track->parts.empty()) {
			if (track->owner != NULL) {
				track->owner->myTrack = NULL;
				track->owner = NULL;
			}
			auto &tracks = *ttc.tracks;
			auto it = std::find(tracks.begin(), tracks.end(), track);
			assert(it != tracks.end());
			*it = tracks.back();
			tracks.pop_back();
			tracksToBeDeleted.push_back(track);
		}
	}
}



void CGroundDecalHandler::AddScars()
{
	for (Scar* s: scarsToBeAdded) {
		TestOverlaps(s);

		int x1 = s->x1 / 16;
		int x2 = min(scarFieldX - 1, s->x2 / 16);
		int y1 = s->y1 / 16;
		int y2 = min(scarFieldY - 1, s->y2 / 16);

		for (int y = y1; y <= y2; ++y) {
			for (int x = x1; x <= x2; ++x) {
				auto &quad = scarField[y * scarFieldX + x];
				assert(std::find(quad.begin(), quad.end(), s) == quad.end());
				quad.push_back(s);
			}
		}
		assert(std::find(scars.begin(), scars.end(), s) == scars.end());
		scars.push_back(s);
	}

	scarsToBeAdded.clear();
}

void CGroundDecalHandler::DrawScars() {
	// create and draw the 16x16 quads for each ground scar
	for (int i = 0; i < scars.size();) {
		Scar* scar = scars[i];

		if (scar->lifeTime < gs->frameNum) {
			RemoveScar(scar, false);
			scars[i] = scars.back();
			scars.pop_back();
			continue;
		}

		DrawGroundScar(scar, groundScarAlphaFade);
		++i;
	}
}




void CGroundDecalHandler::Draw()
{
	if (!drawDecals) {
		return;
	}

	const float3 ambientColor = mapInfo->light.groundAmbientColor * CGlobalRendering::SMF_INTENSITY_MULT;

	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(-10, -200);
	glDepthMask(0);

	glActiveTexture(GL_TEXTURE1);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, readMap->GetShadingTexture());
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_PREVIOUS_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

		glMultiTexCoord4f(GL_TEXTURE1_ARB, 1.0f,1.0f,1.0f,1.0f); // workaround a nvidia bug with TexGen
		SetTexGen(1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE), 0, 0);

	if (infoTextureHandler->IsEnabled()) {
		glActiveTexture(GL_TEXTURE3);
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_ADD_SIGNED_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_PREVIOUS_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_ARB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

		glMultiTexCoord4f(GL_TEXTURE3_ARB, 1.0f,1.0f,1.0f,1.0f); // workaround a nvidia bug with TexGen
		SetTexGen(1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE), 0, 0);

		glBindTexture(GL_TEXTURE_2D, infoTextureHandler->GetCurrentInfoTexture());
	}

	if (shadowHandler->shadowsLoaded) {
		glActiveTexture(GL_TEXTURE2);
			glEnable(GL_TEXTURE_2D);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			glBindTexture(GL_TEXTURE_2D, shadowHandler->shadowTexture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_ARB, GL_COMPARE_R_TO_TEXTURE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC_ARB, GL_LEQUAL);
			glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE_ARB, GL_LUMINANCE);

		decalShaders[DECAL_SHADER_CURR]->Enable();

		if (decalShaders[DECAL_SHADER_CURR] == decalShaders[DECAL_SHADER_ARB]) {
			decalShaders[DECAL_SHADER_CURR]->SetUniformTarget(GL_VERTEX_PROGRAM_ARB);
			decalShaders[DECAL_SHADER_CURR]->SetUniform4f(10, 1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE), 0.0f, 1.0f);
			decalShaders[DECAL_SHADER_CURR]->SetUniformTarget(GL_FRAGMENT_PROGRAM_ARB);
			decalShaders[DECAL_SHADER_CURR]->SetUniform4f(10, ambientColor.x, ambientColor.y, ambientColor.z, 1.0f);
			decalShaders[DECAL_SHADER_CURR]->SetUniform4f(11, 0.0f, 0.0f, 0.0f, sky->GetLight()->GetGroundShadowDensity());

			glMatrixMode(GL_MATRIX0_ARB);
			glLoadMatrixf(shadowHandler->shadowMatrix.m);
			glMatrixMode(GL_MODELVIEW);
		} else {
			decalShaders[DECAL_SHADER_CURR]->SetUniform4f(4, ambientColor.x, ambientColor.y, ambientColor.z, 1.0f);
			decalShaders[DECAL_SHADER_CURR]->SetUniformMatrix4fv(5, false, &shadowHandler->shadowMatrix.m[0]);
			decalShaders[DECAL_SHADER_CURR]->SetUniform4fv(6, &(shadowHandler->GetShadowParams().x));
		}
	}

	glActiveTexture(GL_TEXTURE0);
	DrawObjectDecals();


	if (shadowHandler->shadowsLoaded) {
		decalShaders[DECAL_SHADER_CURR]->Disable();

		glActiveTexture(GL_TEXTURE2);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_ARB, GL_NONE);
			glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE_ARB, GL_LUMINANCE);
			glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE1);

		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);

		glActiveTexture(GL_TEXTURE0);
	}



	glPolygonOffset(-10, -20);

	AddTracks();
	DrawTracks();
	CleanTracks();

	glBindTexture(GL_TEXTURE_2D, scarTex);
	glPolygonOffset(-10, -400);

	AddScars();
	DrawScars();

	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_BLEND);

	glActiveTexture(GL_TEXTURE1);
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_TEXTURE_GEN_S);
		glDisable(GL_TEXTURE_GEN_T);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_PREVIOUS_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_ARB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glActiveTexture(GL_TEXTURE3); //! infotex
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_TEXTURE_GEN_S);
		glDisable(GL_TEXTURE_GEN_T);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_PREVIOUS_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_ARB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glActiveTexture(GL_TEXTURE0);
}


void CGroundDecalHandler::UnitMoved(const CUnit* unit)
{
	if (decalLevel == 0)
		return;

	AddDecalAndTrack(const_cast<CUnit*>(unit), unit->pos);
}


void CGroundDecalHandler::AddDecalAndTrack(CUnit* unit, const float3& newPos)
{
	SolidObjectDecalDef& decalDef = *const_cast<SolidObjectDecalDef*>(&unit->unitDef->decalDef);

	if (decalDef.useGroundDecal)
		MoveSolidObject(const_cast<CUnit *>(unit), newPos);

	if (!unit->leaveTracks)
		return;

	if (!unit->unitDef->IsGroundUnit())
		return;

	if (decalDef.trackDecalType < -1)
		return;

	if (decalDef.trackDecalType < 0) {
		decalDef.trackDecalType = GetTrackType(decalDef.trackDecalTypeName);
		if (decalDef.trackDecalType < -1)
			return;
	}

	if (unit->myTrack != NULL && unit->myTrack->lastUpdate >= (gs->frameNum - 7))
		return;

	if (!((unit->losStatus[gu->myAllyTeam] & LOS_INLOS) || gu->spectatingFullView))
		return;

	// calculate typemap-index
	const int tmz = newPos.z / (SQUARE_SIZE * 2);
	const int tmx = newPos.x / (SQUARE_SIZE * 2);
	const int tmi = Clamp(tmz * mapDims.hmapx + tmx, 0, mapDims.hmapx * mapDims.hmapy - 1);

	const unsigned char* typeMap = readMap->GetTypeMapSynced();
	const CMapInfo::TerrainType& terType = mapInfo->terrainTypes[ typeMap[tmi] ];

	if (!terType.receiveTracks)
		return;

	const float trackLifeTime = GAME_SPEED * decalLevel * decalDef.trackDecalStrength;

	if (trackLifeTime <= 0.0f)
		return;

	const float3 pos = newPos + unit->frontdir * decalDef.trackDecalOffset;

	TrackPart* tp = new TrackPart();
	tp->pos1 = pos + unit->rightdir * decalDef.trackDecalWidth * 0.5f;
	tp->pos2 = pos - unit->rightdir * decalDef.trackDecalWidth * 0.5f;
	tp->pos1.y = CGround::GetHeightReal(tp->pos1.x, tp->pos1.z, false);
	tp->pos2.y = CGround::GetHeightReal(tp->pos2.x, tp->pos2.z, false);
	tp->creationTime = gs->frameNum;

	TrackToAdd tta;
	tta.tp = tp;
	tta.unit = unit;

	if (unit->myTrack == NULL) {
		unit->myTrack = new UnitTrackStruct(unit);
		unit->myTrack->lifeTime = trackLifeTime;
		unit->myTrack->alphaFalloff = 1.0f / trackLifeTime;

		tta.unit = NULL; // signal new trackstruct

		tp->texPos = 0;
		tp->connected = false;
	} else {
		const TrackPart* prevPart = unit->myTrack->lastAdded;

		tp->texPos = prevPart->texPos + (tp->pos1.distance(prevPart->pos1) / decalDef.trackDecalWidth) * decalDef.trackDecalStretch;
		tp->connected = (prevPart->creationTime == (gs->frameNum - 8));
	}

	unit->myTrack->lastUpdate = gs->frameNum;
	unit->myTrack->lastAdded = tp;

	tta.ts = unit->myTrack;
	tracksToBeAdded.push_back(tta);
}


int CGroundDecalHandler::GetTrackType(const std::string& name)
{
	if (decalLevel == 0) {
		return -2;
	}

	const std::string lowerName = StringToLower(name);

	int a = 0;
	std::vector<TrackType*>::iterator ti;
	for(ti = trackTypes.begin(); ti != trackTypes.end(); ++ti) {
		if ((*ti)->name == lowerName) {
			return a;
		}
		++a;
	}

	TrackType* tt = new TrackType;
	tt->name = lowerName;
	tt->texture = LoadTexture(lowerName);

	trackTypes.push_back(tt);

	return (trackTypes.size() - 1);
}


unsigned int CGroundDecalHandler::LoadTexture(const std::string& name)
{
	std::string fullName = name;
	if (fullName.find_first_of('.') == string::npos) {
		fullName += ".bmp";
	}
	if ((fullName.find_first_of('\\') == string::npos) &&
	    (fullName.find_first_of('/')  == string::npos)) {
		fullName = string("bitmaps/tracks/") + fullName;
	}

	CBitmap bm;
	if (!bm.Load(fullName)) {
		throw content_error("Could not load ground decal from file " + fullName);
	}
	if (FileSystem::GetExtension(fullName) == "bmp") {
		//! bitmaps don't have an alpha channel
		//! so use: red := brightness & green := alpha
		for (int y = 0; y < bm.ysize; ++y) {
			for (int x = 0; x < bm.xsize; ++x) {
				const int index = ((y * bm.xsize) + x) * 4;
				bm.mem[index + 3]    = bm.mem[index + 1];
				const int brightness = bm.mem[index + 0];
				bm.mem[index + 0] = (brightness * 90) / 255;
				bm.mem[index + 1] = (brightness * 60) / 255;
				bm.mem[index + 2] = (brightness * 30) / 255;
			}
		}
	}

	return bm.CreateTexture(true);
}


void CGroundDecalHandler::AddExplosion(float3 pos, float damage, float radius, bool addScar)
{
	if (decalLevel == 0 || !addScar)
		return;

	const float altitude = pos.y - CGround::GetHeightReal(pos.x, pos.z, false);

	// no decals for below-ground explosions
	if (altitude <= -1.0f)
		return;
	if (altitude >= radius)
		return;

	pos.y -= altitude;
	radius -= altitude;

	if (radius < 5.0f)
		return;

	damage = std::min(damage, radius * 30.0f);
	damage *= (radius / (radius + altitude));
	radius = std::min(radius, damage * 0.25f);

	if (damage > 400.0f)
		damage = 400.0f + math::sqrt(damage - 399.0f);

	const int ttl = std::max(1.0f, decalLevel * damage * 3.0f);

	Scar* s = new Scar();
	s->pos = pos.cClampInBounds();
	s->radius = radius * 1.4f;
	s->creationTime = gs->frameNum;
	s->startAlpha = std::max(50.0f, std::min(255.0f, damage));
	s->lifeTime = int(gs->frameNum + ttl);
	s->alphaFalloff = s->startAlpha / ttl;
	// atlas contains 2x2 textures, pick one of them
	s->texOffsetX = (gu->RandInt() & 128)? 0: 0.5f;
	s->texOffsetY = (gu->RandInt() & 128)? 0: 0.5f;

	s->x1 = int(std::max(0.f,                  (s->pos.x - radius) / (SQUARE_SIZE * 2)    ));
	s->x2 = int(std::min(float(mapDims.hmapx - 1), (s->pos.x + radius) / (SQUARE_SIZE * 2) + 1));
	s->y1 = int(std::max(0.f,                  (s->pos.z - radius) / (SQUARE_SIZE * 2)    ));
	s->y2 = int(std::min(float(mapDims.hmapy - 1), (s->pos.z + radius) / (SQUARE_SIZE * 2) + 1));

	s->basesize = (s->x2 - s->x1) * (s->y2 - s->y1);
	s->overdrawn = 0;
	s->lastTest = 0;

	scarsToBeAdded.push_back(s);
}


void CGroundDecalHandler::LoadScar(const std::string& file, unsigned char* buf,
                                   int xoffset, int yoffset)
{
	CBitmap bm;
	if (!bm.Load(file)) {
		throw content_error("Could not load scar from file " + file);
	}

	if (FileSystem::GetExtension(file) == "bmp") {
		//! bitmaps don't have an alpha channel
		//! so use: red := brightness & green := alpha
		for (int y = 0; y < bm.ysize; ++y) {
			for (int x = 0; x < bm.xsize; ++x) {
				const int memIndex = ((y * bm.xsize) + x) * 4;
				const int bufIndex = (((y + yoffset) * 512) + x + xoffset) * 4;
				buf[bufIndex + 3]    = bm.mem[memIndex + 1];
				const int brightness = bm.mem[memIndex + 0];
				buf[bufIndex + 0] = (brightness * 90) / 255;
				buf[bufIndex + 1] = (brightness * 60) / 255;
				buf[bufIndex + 2] = (brightness * 30) / 255;
			}
		}
	} else {
		for (int y = 0; y < bm.ysize; ++y) {
			for (int x = 0; x < bm.xsize; ++x) {
				const int memIndex = ((y * bm.xsize) + x) * 4;
				const int bufIndex = (((y + yoffset) * 512) + x + xoffset) * 4;
				buf[bufIndex + 0]    = bm.mem[memIndex + 0];
				buf[bufIndex + 1]    = bm.mem[memIndex + 1];
				buf[bufIndex + 2]    = bm.mem[memIndex + 2];
				buf[bufIndex + 3]    = bm.mem[memIndex + 3];
			}
		}
	}
}


int CGroundDecalHandler::OverlapSize(Scar* s1, Scar* s2)
{
	if(s1->x1>=s2->x2 || s1->x2<=s2->x1)
		return 0;
	if(s1->y1>=s2->y2 || s1->y2<=s2->y1)
		return 0;

	int xs;
	if(s1->x1<s2->x1)
		xs=s1->x2-s2->x1;
	else
		xs=s2->x2-s1->x1;

	int ys;
	if(s1->y1<s2->y1)
		ys=s1->y2-s2->y1;
	else
		ys=s2->y2-s1->y1;

	return xs*ys;
}


void CGroundDecalHandler::TestOverlaps(Scar* scar)
{
	int x1=scar->x1/16;
	int x2=min(scarFieldX-1,scar->x2/16);
	int y1=scar->y1/16;
	int y2=min(scarFieldY-1,scar->y2/16);

	++lastTest;

	for(int y=y1;y<=y2;++y){
		for(int x=x1;x<=x2;++x){
			auto &quad = scarField[y*scarFieldX+x];
			for(int i = 0;i < quad.size();){
				Scar* tested = quad[i];
				if(lastTest != tested->lastTest && scar->lifeTime >= tested->lifeTime) {
					tested->lastTest = lastTest;
					int overlap = OverlapSize(scar, tested);
					if(overlap > 0 && tested->basesize > 0){
						float part = overlap / tested->basesize;
						tested->overdrawn += part;
						if(tested->overdrawn > maxOverlap){
							RemoveScar(tested, true);
						} else {
							++i;
						}
					} else {
						++i;
					}
				} else {
					++i;
				}
			}
		}
	}
}


void CGroundDecalHandler::RemoveScar(Scar* scar, bool removeFromScars)
{
	int x1 = scar->x1 / 16;
	int x2 = min(scarFieldX - 1, scar->x2 / 16);
	int y1 = scar->y1 / 16;
	int y2 = min(scarFieldY - 1, scar->y2 / 16);

	for (int y = y1;y <= y2; ++y) {
		for (int x = x1; x <= x2; ++x) {
			auto &quad = scarField[y * scarFieldX + x];
			auto it = std::find(quad.begin(), quad.end(), scar);
			assert(it != quad.end());
			*it = quad.back();
			quad.pop_back();
		}
	}

	if (removeFromScars) {
		auto it = std::find(scars.begin(), scars.end(), scar);
		assert(it != scars.end());
		*it = scars.back();
		scars.pop_back();
	}

	delete scar;
}


void CGroundDecalHandler::MoveSolidObject(CSolidObject* object, const float3& pos)
{
	if (decalLevel == 0)
		return;

	SolidObjectDecalDef& decalDef = *const_cast<SolidObjectDecalDef*>(&object->objectDef->decalDef);
	if (!decalDef.useGroundDecal || decalDef.groundDecalType < -1)
		return;

	if (decalDef.groundDecalType < 0) {
		decalDef.groundDecalType = GetSolidObjectDecalType(decalDef.groundDecalTypeName);
		if (!decalDef.useGroundDecal || decalDef.groundDecalType < -1)
			return;
	}

	SolidObjectGroundDecal* olddecal = object->groundDecal;
	if (olddecal != NULL) {
		olddecal->owner = NULL;
		olddecal->gbOwner = NULL;
	}

	const int sizex = decalDef.groundDecalSizeX;
	const int sizey = decalDef.groundDecalSizeY;

	SolidObjectGroundDecal* decal = new SolidObjectGroundDecal();

	decal->owner = object;
	decal->gbOwner = 0;
	decal->alphaFalloff = decalDef.groundDecalDecaySpeed;
	decal->alpha = 0.0f;
	decal->pos = pos;
	decal->radius = math::sqrt(float(sizex * sizex + sizey * sizey)) * SQUARE_SIZE + 20.0f;
	decal->facing = object->buildFacing;
	// convert to heightmap coors
	decal->xsize = sizex << 1;
	decal->ysize = sizey << 1;

	if (object->buildFacing == FACING_EAST || object->buildFacing == FACING_WEST) {
		// swap xsize and ysize if object faces East or West
		std::swap(decal->xsize, decal->ysize);
	}

	// position of top-left corner
	decal->posx = (pos.x / SQUARE_SIZE) - (decal->xsize >> 1);
	decal->posy = (pos.z / SQUARE_SIZE) - (decal->ysize >> 1);

	object->groundDecal = decal;
	objectDecalTypes[decalDef.groundDecalType]->objectDecals.push_back(decal);
}


void CGroundDecalHandler::RemoveSolidObject(CSolidObject* object, GhostSolidObject* gb)
{
	if (decalLevel == 0)
		return;

	assert(object);
	SolidObjectGroundDecal* decal = object->groundDecal;

	if (decal == NULL)
		return;

	if (gb != NULL)
		gb->decal = decal;

	decal->owner = NULL;
	decal->gbOwner = gb;
	object->groundDecal = NULL;
}


/**
 * @brief immediately remove an object's ground decal, if any (without fade out)
 */
void CGroundDecalHandler::ForceRemoveSolidObject(CSolidObject* object)
{
	if (decalLevel == 0)
		return;

	SolidObjectGroundDecal* decal = object->groundDecal;

	if (decal == NULL)
		return;

	decal->owner = NULL;
	decal->alpha = 0.0f;
	object->groundDecal = NULL;
}


int CGroundDecalHandler::GetSolidObjectDecalType(const std::string& name)
{
	if (decalLevel == 0)
		return -2;

	const std::string& lowerName = StringToLower(name);
	const std::string& fullName = "unittextures/" + lowerName;

	int decalType = 0;

	std::vector<SolidObjectDecalType*>::iterator bi;
	for (bi = objectDecalTypes.begin(); bi != objectDecalTypes.end(); ++bi) {
		if ((*bi)->name == lowerName) {
			return decalType;
		}
		++decalType;
	}

	CBitmap bm;
	if (!bm.Load(fullName)) {
		LOG_L(L_ERROR, "[%s] Could not load object-decal from file \"%s\"", __FUNCTION__, fullName.c_str());
		return -2;
	}

	SolidObjectDecalType* tt = new SolidObjectDecalType();
	tt->name = lowerName;
	tt->texture = bm.CreateTexture(true);

	objectDecalTypes.push_back(tt);
	return (objectDecalTypes.size() - 1);
}

void CGroundDecalHandler::GhostCreated(CSolidObject* object, GhostSolidObject* gb) {
	if (object->objectDef->decalDef.useGroundDecal)
		RemoveSolidObject(object, gb);
}

void CGroundDecalHandler::GhostDestroyed(GhostSolidObject* gb) {
	if (gb->decal)
		gb->decal->gbOwner = NULL;
}


SolidObjectGroundDecal::~SolidObjectGroundDecal() {
	SafeDelete(va);
}

CGroundDecalHandler::Scar::~Scar() {
	SafeDelete(va);
}

void CGroundDecalHandler::ExplosionOccurred(const CExplosionEvent& event) {
	AddExplosion(event.GetPos(), event.GetDamage(), event.GetRadius(), ((event.GetWeaponDef() != NULL) && event.GetWeaponDef()->visuals.explosionScar));
}

void CGroundDecalHandler::RenderUnitCreated(const CUnit* unit, int cloaked) {
	if (unit->unitDef->decalDef.useGroundDecal)
		MoveSolidObject(const_cast<CUnit*>(unit), unit->pos);
}

void CGroundDecalHandler::RenderUnitDestroyed(const CUnit* unit) {
	if (decalLevel == 0)
		return;

	CUnit* u = const_cast<CUnit*>(unit);
	RemoveSolidObject(u, NULL);

	if (unit->myTrack != NULL) {
		u->myTrack->owner = NULL;
		u->myTrack = NULL;
	}
}

void CGroundDecalHandler::RenderFeatureCreated(const CFeature* feature)
{
	if (feature->objectDef->decalDef.useGroundDecal)
		MoveSolidObject(const_cast<CFeature*>(feature), feature->pos);
}

void CGroundDecalHandler::FeatureMoved(const CFeature* feature, const float3& oldpos) {
	if (feature->objectDef->decalDef.useGroundDecal && (feature->def->drawType == DRAWTYPE_MODEL))
		MoveSolidObject(const_cast<CFeature *>(feature), feature->pos);
}

void CGroundDecalHandler::UnitLoaded(const CUnit* unit, const CUnit* transport) {
	if (unit->unitDef->decalDef.useGroundDecal)
		RemoveSolidObject(const_cast<CUnit *>(unit), NULL); // FIXME: Add a RenderUnitLoaded event
}

void CGroundDecalHandler::UnitUnloaded(const CUnit* unit, const CUnit* transport) {
	if (unit->unitDef->decalDef.useGroundDecal)
		MoveSolidObject(const_cast<CUnit *>(unit), unit->pos); // FIXME: Add a RenderUnitUnloaded event
}
