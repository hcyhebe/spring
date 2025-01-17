/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include <set>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cctype>

#include "LuaWeaponDefs.h"

#include "LuaInclude.h"

#include "LuaDefs.h"
#include "LuaHandle.h"
#include "LuaUtils.h"
#include "Game/TraceRay.h"
#include "Rendering/Models/IModelParser.h"
#include "Sim/Misc/CategoryHandler.h"
#include "Sim/Misc/DamageArrayHandler.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "System/FileSystem/SimpleParser.h"
#include "System/Log/ILog.h"
#include "System/Util.h"
#include "Sim/Misc/GlobalSynced.h"


static ParamMap paramMap;

static bool InitParamMap();

// iteration routines
static int Next(lua_State* L);
static int Pairs(lua_State* L);

// meta-table calls
static int WeaponDefIndex(lua_State* L);
static int WeaponDefNewIndex(lua_State* L);
static int WeaponDefMetatable(lua_State* L);

// special access functions
static int NoFeatureCollide(lua_State* L, const void* data);
static int NoFriendlyCollide(lua_State* L, const void* data);
static int NoNeutralCollide(lua_State* L, const void* data);

static int VisualsTable(lua_State* L, const void* data);
static int DamagesArray(lua_State* L, const void* data);
static int CustomParamsTable(lua_State* L, const void* data);
static int GuiSoundSetTable(lua_State* L, const void* data);
//static int CategorySetFromBits(lua_State* L, const void* data);


/******************************************************************************/
/******************************************************************************/

bool LuaWeaponDefs::PushEntries(lua_State* L)
{
	InitParamMap();

	typedef int (*IndxFuncType)(lua_State*);
	typedef int (*IterFuncType)(lua_State*);
	typedef std::map<std::string, int> ObjectDefMapType;

	const ObjectDefMapType& defsMap = weaponDefHandler->weaponID;

	const std::array<const LuaHashString, 3> indxOpers = {{
		LuaHashString("__index"),
		LuaHashString("__newindex"),
		LuaHashString("__metatable")
	}};
	const std::array<const LuaHashString, 2> iterOpers = {{
		LuaHashString("pairs"),
		LuaHashString("next")
	}};

	const std::array<const IndxFuncType, 3> indxFuncs = {WeaponDefIndex, WeaponDefNewIndex, WeaponDefMetatable};
	const std::array<const IterFuncType, 2> iterFuncs = {Pairs, Next};

	for (auto it = defsMap.cbegin(); it != defsMap.cend(); ++it) {
		const auto def = weaponDefHandler->GetWeaponDefByID(it->second);

		if (def == NULL)
			continue;

		PushObjectDefProxyTable(L, indxOpers, iterOpers, indxFuncs, iterFuncs, def);
	}

	return true;
}


/******************************************************************************/

static int WeaponDefIndex(lua_State* L)
{
	// not a default value
	if (!lua_isstring(L, 2)) {
		lua_rawget(L, 1);
		return 1;
	}

	const char* name = lua_tostring(L, 2);
	ParamMap::const_iterator it = paramMap.find(name);

	// not a default value
	if (it == paramMap.end()) {
		lua_rawget(L, 1);
		return 1;
	}

	const void* userData = lua_touserdata(L, lua_upvalueindex(1));
	const WeaponDef* wd = static_cast<const WeaponDef*>(userData);
	const DataElement& elem = it->second;
	const void* p = ((const char*)wd) + elem.offset;
	switch (elem.type) {
		case READONLY_TYPE: {
			lua_rawget(L, 1);
			return 1;
		}
		case INT_TYPE: {
			lua_pushnumber(L, *reinterpret_cast<const int*>(p));
			return 1;
		}
		case BOOL_TYPE: {
			lua_pushboolean(L, *reinterpret_cast<const bool*>(p));
			return 1;
		}
		case FLOAT_TYPE: {
			lua_pushnumber(L, *reinterpret_cast<const float*>(p));
			return 1;
		}
		case STRING_TYPE: {
			lua_pushsstring(L, *reinterpret_cast<const std::string*>(p));
			return 1;
		}
		case FUNCTION_TYPE: {
			return elem.func(L, p);
		}
		case ERROR_TYPE: {
			LOG_L(L_ERROR, "[%s] ERROR_TYPE for key \"%s\" in WeaponDefs __index", __FUNCTION__, name);
			lua_pushnil(L);
			return 1;
		}
	}

	return 0;
}


static int WeaponDefNewIndex(lua_State* L)
{
	// not a default value, set it
	if (!lua_isstring(L, 2)) {
		lua_rawset(L, 1);
		return 0;
	}

	const char* name = lua_tostring(L, 2);
	ParamMap::const_iterator it = paramMap.find(name);

	// not a default value, set it
	if (it == paramMap.end()) {
		lua_rawset(L, 1);
		return 0;
	}

	const void* userData = lua_touserdata(L, lua_upvalueindex(1));
	const WeaponDef* wd = static_cast<const WeaponDef*>(userData);

	// write-protected
	if (!gs->editDefsEnabled) {
	 	luaL_error(L, "Attempt to write WeaponDefs[%d].%s", wd->id, name);
	 	return 0;
	}

	// Definition editing
	const DataElement& elem = it->second;
	const void* p = ((const char*)wd) + elem.offset;

	switch (elem.type) {
		case FUNCTION_TYPE:
		case READONLY_TYPE: {
			luaL_error(L, "Can not write to %s", name);
			return 0;
		}
		case INT_TYPE: {
			*((int*)p) = lua_toint(L, -1);
			return 0;
		}
		case BOOL_TYPE: {
			*((bool*)p) = lua_toboolean(L, -1);
			return 0;
		}
		case FLOAT_TYPE: {
			*((float*)p) = lua_tofloat(L, -1);
			return 0;
		}
		case STRING_TYPE: {
			*((string*)p) = lua_tostring(L, -1);
			return 0;
		}
		case ERROR_TYPE: {
			LOG_L(L_ERROR, "[%s] ERROR_TYPE for key \"%s\" in WeaponDefs __newindex", __FUNCTION__, name);
			lua_pushnil(L);
			return 1;
		}
	}

	return 0;
}


static int WeaponDefMetatable(lua_State* L)
{
	//const void* userData = lua_touserdata(L, lua_upvalueindex(1));
	//const WeaponDef* wd = (const WeaponDef*)userData;
	return 0;
}


/******************************************************************************/

static int Next(lua_State* L)
{
	return LuaUtils::Next(paramMap, L);
}


static int Pairs(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushcfunction(L, Next);	// iterator
	lua_pushvalue(L, 1);        // state (table)
	lua_pushnil(L);             // initial value
	return 3;
}


/******************************************************************************/
/******************************************************************************/

static int DamagesArray(lua_State* L, const void* data)
{
	const DamageArray& d = *static_cast<const DamageArray*>(data);
	lua_newtable(L);
	HSTR_PUSH_NUMBER(L, "impulseFactor",      d.impulseFactor);
	HSTR_PUSH_NUMBER(L, "impulseBoost",       d.impulseBoost);
	HSTR_PUSH_NUMBER(L, "craterMult",         d.craterMult);
	HSTR_PUSH_NUMBER(L, "craterBoost",        d.craterBoost);
	HSTR_PUSH_NUMBER(L, "paralyzeDamageTime", d.paralyzeDamageTime);

	// damage values
	const int typeCount = damageArrayHandler->GetNumTypes();
	for (int i = 0; i < typeCount; i++) {
		lua_pushnumber(L, i);
		lua_pushnumber(L, d[i]);
		lua_rawset(L, -3);
	}

	return 1;
}


static int VisualsTable(lua_State* L, const void* data)
{
	const struct WeaponDef::Visuals& v = *static_cast<const struct WeaponDef::Visuals*>(data);
	lua_newtable(L);
	HSTR_PUSH_STRING(L, "modelName",      modelParser->FindModelPath(v.modelName));
	HSTR_PUSH_NUMBER(L, "colorR",         v.color.x);
	HSTR_PUSH_NUMBER(L, "colorG",         v.color.y);
	HSTR_PUSH_NUMBER(L, "colorB",         v.color.z);
	HSTR_PUSH_NUMBER(L, "color2R",        v.color2.x);
	HSTR_PUSH_NUMBER(L, "color2G",        v.color2.y);
	HSTR_PUSH_NUMBER(L, "color2B",        v.color2.z);
	HSTR_PUSH_BOOL  (L, "smokeTrail",     v.smokeTrail);
	HSTR_PUSH_NUMBER(L, "tileLength",     v.tilelength);
	HSTR_PUSH_NUMBER(L, "scrollSpeed",    v.scrollspeed);
	HSTR_PUSH_NUMBER(L, "pulseSpeed",     v.pulseSpeed);
	HSTR_PUSH_NUMBER(L, "laserFlareSize", v.laserflaresize);
	HSTR_PUSH_NUMBER(L, "thickness",      v.thickness);
	HSTR_PUSH_NUMBER(L, "coreThickness",  v.corethickness);
	HSTR_PUSH_NUMBER(L, "beamDecay",      v.beamdecay);
	HSTR_PUSH_NUMBER(L, "stages",         v.stages);
	HSTR_PUSH_NUMBER(L, "sizeDecay",      v.sizeDecay);
	HSTR_PUSH_NUMBER(L, "alphaDecay",     v.alphaDecay);
	HSTR_PUSH_NUMBER(L, "separation",     v.separation);
	HSTR_PUSH_BOOL  (L, "noGap",          v.noGap);
	HSTR_PUSH_BOOL  (L, "alwaysVisible",  v.alwaysVisible);
	HSTR_PUSH_BOOL  (L, "beamWeapon",     false); // DEPRECATED

	return 1;
//	CColorMap *colorMap;
//	AtlasedTexture *texture1;
//	AtlasedTexture *texture2;
//	AtlasedTexture *texture3;
//	AtlasedTexture *texture4;
}



static int NoEnemyCollide(lua_State* L, const void* data)
{
	const int bits = *((const int*) data);
	lua_pushboolean(L, (bits & Collision::NOENEMIES));
	return 1;
}

static int NoFriendlyCollide(lua_State* L, const void* data)
{
	const int bits = *((const int*) data);
	lua_pushboolean(L, (bits & Collision::NOFRIENDLIES));
	return 1;
}

static int NoFeatureCollide(lua_State* L, const void* data)
{
	const int bits = *((const int*) data);
	lua_pushboolean(L, (bits & Collision::NOFEATURES));
	return 1;
}

static int NoNeutralCollide(lua_State* L, const void* data)
{
	const int bits = *((const int*) data);
	lua_pushboolean(L, (bits & Collision::NONEUTRALS));
	return 1;
}

static int NoGroundCollide(lua_State* L, const void* data)
{
	const int bits = *((const int*) data);
	lua_pushboolean(L, (bits & Collision::NOGROUND));
	return 1;
}



static inline int BuildCategorySet(lua_State* L, const vector<string>& cats)
{
	lua_newtable(L);
	const int count = (int)cats.size();
	for (int i = 0; i < count; i++) {
		lua_pushsstring(L, cats[i]);
		lua_pushboolean(L, true);
		lua_rawset(L, -3);
	}
	return 1;
}


/*
static int CategorySetFromBits(lua_State* L, const void* data)
{
	const int bits = *((const int*)data);
	const vector<string> &cats =
		CCategoryHandler::Instance()->GetCategoryNames(bits);
	return BuildCategorySet(L, cats);
}
*/

/*static int CategorySetFromString(lua_State* L, const void* data)
{
	const string& str = *((const string*)data);
	const string lower = StringToLower(str);
	const vector<string> &cats = CSimpleParser::Tokenize(lower, 0);
	return BuildCategorySet(L, cats);
}*/


static int CustomParamsTable(lua_State* L, const void* data)
{
	const map<string, string>& params = *((const map<string, string>*)data);
	lua_newtable(L);
	map<string, string>::const_iterator it;
	for (it = params.begin(); it != params.end(); ++it) {
		lua_pushsstring(L, it->first);
		lua_pushsstring(L, it->second);
		lua_rawset(L, -3);
	}
	return 1;
}


static int GuiSoundSetTable(lua_State* L, const void* data)
{
	const GuiSoundSet& soundSet = *static_cast<const GuiSoundSet*>(data);
	const int soundCount = (int)soundSet.sounds.size();
	lua_newtable(L);
	for (int i = 0; i < soundCount; i++) {
		lua_pushnumber(L, i + 1);
		lua_newtable(L);
		const GuiSoundSet::Data& sound = soundSet.sounds[i];
		HSTR_PUSH_STRING(L, "name",   sound.name);
		HSTR_PUSH_NUMBER(L, "volume", sound.volume);
		if (!CLuaHandle::GetHandleSynced(L)) {
			HSTR_PUSH_NUMBER(L, "id", sound.id);
		}
		lua_rawset(L, -3);
	}
	return 1;
}



/******************************************************************************/
/******************************************************************************/

static bool InitParamMap()
{
	paramMap.clear();

	paramMap["next"]  = DataElement(READONLY_TYPE);
	paramMap["pairs"] = DataElement(READONLY_TYPE);

	// dummy WeaponDef for offset generation
	const WeaponDef wd;
	const char* start = ADDRESS(wd);

	ADD_FUNCTION("damages",      wd.damages,   DamagesArray);
	ADD_FUNCTION("visuals",      wd.visuals,   VisualsTable);

	ADD_FUNCTION("hitSound",     wd.hitSound,  GuiSoundSetTable);
	ADD_FUNCTION("fireSound",    wd.fireSound, GuiSoundSetTable);

	ADD_FUNCTION("customParams",         wd.customParams,   CustomParamsTable);
	ADD_FUNCTION("noEnemyCollide",       wd.collisionFlags, NoEnemyCollide);
	ADD_FUNCTION("noFriendlyCollide",    wd.collisionFlags, NoFriendlyCollide);
	ADD_FUNCTION("noFeatureCollide",     wd.collisionFlags, NoFeatureCollide);
	ADD_FUNCTION("noNeutralCollide",     wd.collisionFlags, NoNeutralCollide);
	ADD_FUNCTION("noGroundCollide",      wd.collisionFlags, NoGroundCollide);

	ADD_DEPRECATED_LUADEF_KEY("areaOfEffect");
	ADD_DEPRECATED_LUADEF_KEY("maxVelocity");
	ADD_DEPRECATED_LUADEF_KEY("onlyTargetCategories");
	ADD_DEPRECATED_LUADEF_KEY("restTime");

	ADD_INT("id", wd.id);

	ADD_INT("tdfId", wd.tdfId);

	ADD_STRING("name",        wd.name);
	ADD_STRING("description", wd.description);

	// FIXME: why is this expgen-tag exposed but not the other two?
	ADD_STRING("cegTag", wd.visuals.ptrailExpGenTag);

	ADD_STRING("type", wd.type);

	ADD_FLOAT("range", wd.range);
	ADD_FLOAT("heightMod", wd.heightmod);
	ADD_FLOAT("accuracy", wd.accuracy);
	ADD_FLOAT("sprayAngle", wd.sprayAngle);
	ADD_FLOAT("movingAccuracy", wd.movingAccuracy);
	ADD_FLOAT("targetMoveError", wd.targetMoveError);
	ADD_FLOAT("leadLimit", wd.leadLimit);
	ADD_FLOAT("leadBonus", wd.leadBonus);
	ADD_FLOAT("predictBoost", wd.predictBoost);
	ADD_INT("highTrajectory", wd.highTrajectory);
 
	ADD_FLOAT("dynDamageExp", wd.dynDamageExp);
	ADD_FLOAT("dynDamageMin", wd.dynDamageMin);
	ADD_FLOAT("dynDamageRange", wd.dynDamageRange);
	ADD_BOOL("dynDamageInverted", wd.dynDamageInverted);

	ADD_BOOL("noSelfDamage",  wd.noSelfDamage);
	ADD_BOOL("impactOnly",    wd.impactOnly);

	ADD_FLOAT("craterAreaOfEffect", wd.craterAreaOfEffect);
	ADD_FLOAT("damageAreaOfEffect", wd.damageAreaOfEffect);
	ADD_FLOAT("edgeEffectiveness",  wd.edgeEffectiveness);
	ADD_FLOAT("fireStarter",        wd.fireStarter);
	ADD_FLOAT("size",               wd.size);
	ADD_FLOAT("sizeGrowth",         wd.sizeGrowth);
	ADD_FLOAT("collisionSize",      wd.collisionSize);

	ADD_INT("salvoSize",    wd.salvosize);
	ADD_INT("projectiles",  wd.projectilespershot);
	ADD_FLOAT("salvoDelay", wd.salvodelay);
	ADD_FLOAT("reload",     wd.reload);
	ADD_FLOAT("beamtime",   wd.beamtime);
	ADD_BOOL("beamburst",   wd.beamburst);

	ADD_BOOL("waterbounce",    wd.waterBounce);
	ADD_BOOL("groundbounce",   wd.groundBounce);
	ADD_FLOAT("groundslip",    wd.bounceSlip);
	ADD_FLOAT("bouncerebound", wd.bounceRebound);
	ADD_INT("numbounce",       wd.numBounce);

	ADD_FLOAT("maxAngle", wd.maxAngle);

	ADD_FLOAT("uptime", wd.uptime);

	ADD_FLOAT("metalCost",  wd.metalcost);
	ADD_FLOAT("energyCost", wd.energycost);

	ADD_BOOL("turret", wd.turret);
	ADD_BOOL("onlyForward", wd.onlyForward);
	ADD_BOOL("waterWeapon", wd.waterweapon);
	ADD_BOOL("tracks", wd.tracks);
	ADD_BOOL("paralyzer", wd.paralyzer);

	ADD_BOOL("noAutoTarget",   wd.noAutoTarget);
	ADD_BOOL("manualFire",     wd.manualfire);
	ADD_INT("targetable",      wd.targetable);
	ADD_BOOL("stockpile",      wd.stockpile);
	ADD_INT("interceptor",     wd.interceptor);
	ADD_BOOL("interceptSolo",  wd.interceptSolo);
	ADD_FLOAT("coverageRange", wd.coverageRange);

	ADD_FLOAT("stockpileTime", wd.stockpileTime);

	ADD_FLOAT("intensity", wd.intensity);
	ADD_FLOAT("duration", wd.duration);
	ADD_INT("beamTTL", wd.beamLaserTTL);

	ADD_BOOL("soundTrigger", wd.soundTrigger);

	ADD_BOOL("selfExplode", wd.selfExplode);
	ADD_BOOL("gravityAffected", wd.gravityAffected);
	ADD_FLOAT("myGravity", wd.myGravity);
	ADD_BOOL("noExplode", wd.noExplode);
	ADD_FLOAT("startvelocity", wd.startvelocity);
	ADD_FLOAT("weaponAcceleration", wd.weaponacceleration);
	ADD_FLOAT("turnRate", wd.turnrate);

	ADD_FLOAT("projectilespeed", wd.projectilespeed);
	ADD_FLOAT("explosionSpeed", wd.explosionSpeed);

	ADD_FLOAT("wobble", wd.wobble);
	ADD_FLOAT("dance",  wd.dance);

	ADD_FLOAT("trajectoryHeight", wd.trajectoryHeight);
	ADD_INT("flightTime", wd.flighttime);

	ADD_BOOL("largeBeamLaser", wd.largeBeamLaser);
	ADD_BOOL("laserHardStop", wd.laserHardStop);

	ADD_BOOL("isShield",                wd.isShield);
	ADD_BOOL("shieldRepulser",          wd.shieldRepulser);
	ADD_BOOL("smartShield",             wd.smartShield);
	ADD_BOOL("exteriorShield",          wd.exteriorShield);
	ADD_BOOL("visibleShield",           wd.visibleShield);
	ADD_BOOL("visibleShieldRepulse",    wd.visibleShieldRepulse);
	ADD_INT( "visibleShieldHitFrames",  wd.visibleShieldHitFrames);
	ADD_FLOAT("shieldEnergyUse",        wd.shieldEnergyUse);
	ADD_FLOAT("shieldRadius",           wd.shieldRadius);
	ADD_FLOAT("shieldForce",            wd.shieldForce);
	ADD_FLOAT("shieldMaxSpeed",         wd.shieldMaxSpeed);
	ADD_FLOAT("shieldPower",            wd.shieldPower);
	ADD_FLOAT("shieldPowerRegen",       wd.shieldPowerRegen);
	ADD_FLOAT("shieldPowerRegenEnergy", wd.shieldPowerRegenEnergy);
	ADD_INT(  "shieldRechargeDelay",    wd.shieldRechargeDelay);
	ADD_FLOAT("shieldGoodColorR",       wd.shieldGoodColor.x);
	ADD_FLOAT("shieldGoodColorG",       wd.shieldGoodColor.y);
	ADD_FLOAT("shieldGoodColorB",       wd.shieldGoodColor.z);
	ADD_FLOAT("shieldBadColorR",        wd.shieldBadColor.x);
	ADD_FLOAT("shieldBadColorG",        wd.shieldBadColor.y);
	ADD_FLOAT("shieldBadColorB",        wd.shieldBadColor.z);
	ADD_FLOAT("shieldAlpha",            wd.shieldAlpha);

	ADD_INT("shieldInterceptType",      wd.shieldInterceptType);
	ADD_INT("interceptedByShieldType",  wd.interceptedByShieldType);

	ADD_BOOL("avoidFriendly", wd.avoidFriendly);
	ADD_BOOL("avoidFeature",  wd.avoidFeature);
	ADD_BOOL("avoidNeutral",  wd.avoidNeutral);

	ADD_FLOAT("targetBorder",       wd.targetBorder);
	ADD_FLOAT("cylinderTargeting", wd.cylinderTargeting);
	ADD_FLOAT("cylinderTargetting", wd.cylinderTargeting); // FIXME deprecated misspelling
	ADD_FLOAT("minIntensity",       wd.minIntensity);
	ADD_FLOAT("heightBoostFactor",  wd.heightBoostFactor);
	ADD_FLOAT("proximityPriority",  wd.proximityPriority);

	ADD_BOOL("sweepFire", wd.sweepFire);

	ADD_BOOL("canAttackGround", wd.canAttackGround);

	return true;
}


/******************************************************************************/
/******************************************************************************/
