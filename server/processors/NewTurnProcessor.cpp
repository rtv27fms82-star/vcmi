/*
 * NewTurnProcessor.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "NewTurnProcessor.h"

#include "HeroPoolProcessor.h"

#include "../CGameHandler.h"

#include "../../lib/CPlayerState.h"
#include "../../lib/IGameSettings.h"
#include "../../lib/StartInfo.h"
#include "../../lib/callback/GameRandomizer.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CTownHandler.h"
#include "../../lib/entities/ResourceTypeHandler.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/gameState/SThievesGuildInfo.h"
#include "../../lib/mapObjectConstructors/CObjectClassesHandler.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/IOwnableObject.h"
#include "../../lib/mapping/CMap.h"
#include "../../lib/mapping/CCastleEvent.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/StackLocation.h"
#include "../../lib/pathfinder/TurnInfo.h"
#include "../../lib/texts/CGeneralTextHandler.h"

#include "../../lib/CCreatureHandler.h"
#include "../../lib/filesystem/Filesystem.h"
#include "../../lib/json/JsonNode.h"
#include "../../lib/mapObjects/CGCreature.h"

#include <limits>
#include <vstd/RNG.h>

NewTurnProcessor::NewTurnProcessor(CGameHandler * gameHandler)
	:gameHandler(gameHandler)
{
}

void NewTurnProcessor::handleTimeEvents(PlayerColor color)
{
	for (auto const & event : gameHandler->gameState().getMap().events)
	{
		if (!event.occursToday(gameHandler->gameState().day))
			continue;

		if (!event.affectsDifficulty(gameHandler->gameInfo().getStartInfo()->getDifficulty()))
			continue;

		if (!event.affectsPlayer(color, gameHandler->gameInfo().getPlayerState(color)->isHuman()))
			continue;

		InfoWindow iw;
		iw.player = color;
		iw.text = event.message;

		//give resources
		if (!event.resources.empty())
		{
			gameHandler->giveResources(color, event.resources);
			for (GameResID i : LIBRARY->resourceTypeHandler->getAllObjects())
				if (event.resources[i])
					iw.components.emplace_back(ComponentType::RESOURCE, i, event.resources[i]);
		}

		//remove objects specified by event
		for(const ObjectInstanceID objectIdToRemove : event.deletedObjectsInstances)
		{
			const auto * objectInstance = gameHandler->gameInfo().getObj(objectIdToRemove, false);
			if(objectInstance != nullptr)
				gameHandler->removeObject(objectInstance, PlayerColor::NEUTRAL);
		}
		gameHandler->sendAndApply(iw); //show dialog
	}
}

void NewTurnProcessor::handleTownEvents(const CGTownInstance * town)
{
	for (auto const & event : town->events)
	{
		if (!event.occursToday(gameHandler->gameState().day))
			continue;

		if (!event.affectsDifficulty(gameHandler->gameInfo().getStartInfo()->getDifficulty()))
			continue;

		PlayerColor player = town->getOwner();
		if (!event.affectsPlayer(player, gameHandler->gameInfo().getPlayerState(player)->isHuman()))
			continue;

		// dialog
		InfoWindow iw;
		iw.player = player;
		iw.text = event.message;

		if (event.resources.nonZero())
		{
			gameHandler->giveResources(player, event.resources);

			for (GameResID i : LIBRARY->resourceTypeHandler->getAllObjects())
				if (event.resources[i])
					iw.components.emplace_back(ComponentType::RESOURCE, i, event.resources[i]);
		}

		for (auto & i : event.buildings)
		{
			// Only perform action if:
			// 1. Building exists in town (don't attempt to build Lvl 5 guild in Fortress
			// 2. Building was not built yet
			// othervice, silently ignore / skip it
			if (town->getTown()->buildings.count(i) && !town->hasBuilt(i))
			{
				gameHandler->buildStructure(town->id, i, true);
				iw.components.emplace_back(ComponentType::BUILDING, BuildingTypeUniqueID(town->getFactionID(), i));
			}
		}

		if (!event.creatures.empty())
		{
			SetAvailableCreatures sac;
			sac.tid = town->id;
			sac.creatures = town->creatures;

			for (si32 i=0;i<event.creatures.size();i++) //creature growths
			{
				if (i < town->creatures.size() && !town->creatures.at(i).second.empty() && event.creatures.at(i) > 0)//there is dwelling
				{
					sac.creatures[i].first += event.creatures.at(i);
					iw.components.emplace_back(ComponentType::CREATURE, town->creatures.at(i).second.back(), event.creatures.at(i));
				}
			}

			gameHandler->sendAndApply(sac); //show dialog
		}
		gameHandler->sendAndApply(iw); //show dialog
	}
}

void NewTurnProcessor::onPlayerTurnStarted(PlayerColor which)
{
	const auto * playerState = gameHandler->gameState().getPlayerState(which);

	handleTimeEvents(which);
	for (const auto * t : playerState->getTowns())
		handleTownEvents(t);

	for (const auto * t : playerState->getTowns())
	{
		//garrison hero first - consistent with original H3 Mana Vortex and Battle Scholar Academy levelup windows order
		if (t->getGarrisonHero() != nullptr)
			gameHandler->objectVisited(t, t->getGarrisonHero());

		if (t->getVisitingHero() != nullptr)
			gameHandler->objectVisited(t, t->getVisitingHero());
	}
}

void NewTurnProcessor::onPlayerTurnEnded(PlayerColor which)
{
	const auto * playerState = gameHandler->gameState().getPlayerState(which);
	assert(playerState->status == EPlayerStatus::INGAME);

	if (playerState->getTowns().empty())
	{
		DaysWithoutTown pack;
		pack.player = which;
		pack.daysWithoutCastle = playerState->daysWithoutCastle.value_or(0) + 1;
		gameHandler->sendAndApply(pack);
	}
	else
	{
		if (playerState->daysWithoutCastle.has_value())
		{
			DaysWithoutTown pack;
			pack.player = which;
			pack.daysWithoutCastle = std::nullopt;
			gameHandler->sendAndApply(pack);
		}
	}

	// check for 7 days without castle
	gameHandler->checkVictoryLossConditionsForPlayer(which);

	bool newWeek = gameHandler->gameInfo().getDate(Date::DAY_OF_WEEK) == LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_DAYS_PER_WEEK); // end of 7th day

	if (newWeek) //new heroes in tavern
		gameHandler->heroPool->onNewWeek(which);
}

// DIPLO_ENGINE_BEGIN
// Diplo options that need this source patch. Every number lives in a JSON file of an active Diplo submod;
// without the file the engine keeps the stock VCMI 1.7.5 behaviour.
namespace DiploEngine
{
static JsonNode loadConfig(const std::string & path)
{
	const JsonPath resource = JsonPath::builtin(path);
	if(!CResourceHandler::get()->existsResource(resource))
		return JsonNode();
	return JsonNode(resource);
}

static int64_t saturatingMultiply(int64_t a, int64_t b, int64_t limit)
{
	if(a <= 0 || b <= 0)
		return 0;
	if(a > limit / b)
		return limit;
	return std::min(a * b, limit);
}

static int64_t readInteger(const JsonNode & node, const std::string & key, int64_t fallback, int64_t minimum, int64_t maximum)
{
	if(!node[key].isNumber())
		return fallback;
	return std::clamp<int64_t>(node[key].Integer(), minimum, maximum);
}

/// Option 02. Troops: N x (1 + K x finishedWeeks). Resources: ratio x N x (1 + K x finishedWeeks).
/// Returns 0 when neither the N nor the K submod is active.
static int64_t aiFactor(int64_t finishedWeeks, bool resources)
{
	static const JsonNode configN = loadConfig("config/vnd30/engine/aiGrowthN.json");
	static const JsonNode configK = loadConfig("config/vnd30/engine/aiGrowthK.json");
	if(configN.isNull() && configK.isNull())
		return 0;
	const int64_t limit = 1000000000;
	const int64_t multiplierN = readInteger(configN, "N", 1, 0, limit);
	const int64_t multiplierK = readInteger(configK, "K", 0, 0, limit);
	const int64_t ratio = readInteger(configN, "resourceRatio", 10, 0, limit);
	const int64_t ramp = std::min(limit, 1 + saturatingMultiply(multiplierK, std::max<int64_t>(finishedWeeks, 0), limit));
	const int64_t troops = saturatingMultiply(multiplierN, ramp, limit);
	return resources ? saturatingMultiply(troops, ratio, limit) : troops;
}

/// Option 12. Weekly random neutral stacks of the configured levels.
static void spawnWeeklyMonsters(CGameHandler * gameHandler, int64_t newWeek)
{
	static const JsonNode config = loadConfig("config/vnd30/engine/weeklySpawn.json");
	if(config.isNull())
		return;

	const int64_t startWeek = readInteger(config, "startWeek", 2, 1, std::numeric_limits<int>::max());
	if(newWeek < startWeek)
		return;

	const int64_t minLevel = readInteger(config, "minLevel", 7, 0, 1000);
	const int64_t maxLevel = readInteger(config, "maxLevel", 10, minLevel, 1000);
	const int64_t strengthPercent = readInteger(config, "strengthPercent", 60, 0, 1000000);
	const int64_t tilesPerStack = readInteger(config, "tilesPerStack", 200, 1, std::numeric_limits<int>::max());
	const int64_t maxAmount = readInteger(config, "maxAmount", 2000000, 1, std::numeric_limits<int>::max());

	std::vector<CreatureID> candidates;
	for(const auto & creature : LIBRARY->creh->objects)
	{
		if(!creature || creature->special || creature->getAIValue() <= 0)
			continue;
		if(creature->getLevel() < minLevel || creature->getLevel() > maxLevel)
			continue;
		candidates.push_back(creature->getId());
	}
	if(candidates.empty())
	{
		logGlobal->warn("Diplo weekly spawn: no creature of level %d-%d", minLevel, maxLevel);
		return;
	}

	// Strongest neutral stack on the map before this week's spawn: sum of unit count x AI value.
	uint64_t strongest = 0;
	for(const auto * monster : gameHandler->gameState().getMap().getObjects<CGCreature>())
		strongest = std::max<uint64_t>(strongest, monster->getArmyStrength());
	const auto targetStrength = static_cast<long double>(strongest) * strengthPercent / 100;

	std::vector<int3> tiles;
	gameHandler->gameState().getFreeTiles(tiles, true);
	const size_t stacks = tiles.size() / static_cast<size_t>(tilesPerStack);
	RandomGeneratorUtil::randomShuffle(tiles, gameHandler->getRandomGenerator());

	logGlobal->info("Diplo weekly spawn: week %d, %d stacks, strongest neutral %d", newWeek, stacks, strongest);
	for(size_t i = 0; i < stacks && i < tiles.size(); ++i)
	{
		const CreatureID creatureID = *RandomGeneratorUtil::nextItem(candidates, gameHandler->getRandomGenerator());
		const long double aiValue = creatureID.toCreature()->getAIValue();
		const long double rawAmount = targetStrength / aiValue;
		const int amount = static_cast<int>(std::clamp<long double>(rawAmount, 1, static_cast<long double>(maxAmount)));
		gameHandler->createWanderingMonster(tiles[i], creatureID, amount);
	}
}
}
// DIPLO_ENGINE_END

ResourceSet NewTurnProcessor::generatePlayerIncome(PlayerColor playerID, bool newWeek)
{
	const auto & playerSettings = gameHandler->gameInfo().getPlayerSettings(playerID);
	const PlayerState & state = gameHandler->gameState().players.at(playerID);
	ResourceSet income;

	for (const auto & town : state.getTowns())
	{
		if (newWeek && town->hasBuilt(BuildingSubID::TREASURY))
		{
			//give 10% of starting gold
			income[EGameResID::GOLD] += state.resources[EGameResID::GOLD] / 10;
		}

		//give resources if there's a Mystic Pond
		if (newWeek && town->hasBuilt(BuildingSubID::MYSTIC_POND))
		{
			static constexpr std::array rareResources = {
				GameResID::MERCURY,
				GameResID::SULFUR,
				GameResID::CRYSTAL,
				GameResID::GEMS
			};

			auto resID = *RandomGeneratorUtil::nextItem(rareResources, gameHandler->getRandomGenerator());
			int resVal = gameHandler->getRandomGenerator().nextInt(1, 4);

			income[resID] += resVal;

			gameHandler->setObjPropertyValue(town->id, ObjProperty::BONUS_VALUE_FIRST, resID);
			gameHandler->setObjPropertyValue(town->id, ObjProperty::BONUS_VALUE_SECOND, resVal);
		}
	}

	for(auto & k : LIBRARY->resourceTypeHandler->getAllObjects())
	{
		income += state.valOfBonuses(BonusType::RESOURCES_CONSTANT_BOOST, BonusSubtypeID(k));
		income += state.valOfBonuses(BonusType::RESOURCES_TOWN_MULTIPLYING_BOOST, BonusSubtypeID(k)) * state.getTowns().size();
	}

	if(newWeek) //weekly crystal generation if 1 or more crystal dragons in any hero army or town garrison
	{
		bool hasCrystalGenCreature = false;
		for (const auto & hero : state.getHeroes())
			for(const auto & stack : hero->stacks)
				if(stack.second->hasBonusOfType(BonusType::SPECIAL_CRYSTAL_GENERATION))
					hasCrystalGenCreature = true;

		for(const auto & town : state.getTowns())
			for(const auto & stack : town->stacks)
				if(stack.second->hasBonusOfType(BonusType::SPECIAL_CRYSTAL_GENERATION))
					hasCrystalGenCreature = true;

		if(hasCrystalGenCreature)
			income[EGameResID::CRYSTAL] += 3;
	}

	TResources incomeHandicapped = income;
	incomeHandicapped.applyHandicap(playerSettings->handicap.percentIncome);

	for (const auto * obj :	state.getOwnedObjects())
		incomeHandicapped += obj->asOwnable()->dailyIncome();

	// DIPLO_ENGINE option 02: AI income = income x ratio x N x (1 + K x finishedWeeks); replaces the difficulty bonus below.
	// Income is booked for the day about to start; the date has not been switched yet.
	const int64_t diploDaysPerWeek = std::max<int64_t>(1, LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_DAYS_PER_WEEK));
	const int64_t diploIncomeFactor = state.isHuman() ? 0 : DiploEngine::aiFactor(gameHandler->gameInfo().getDate(Date::DAY) / diploDaysPerWeek, true);
	if (diploIncomeFactor > 0)
	{
		for (GameResID i : LIBRARY->resourceTypeHandler->getAllObjects())
		{
			const int64_t stock = std::clamp<int64_t>(state.resources[i], 0, GameConstants::PLAYER_RESOURCES_CAP);
			const int64_t room = GameConstants::PLAYER_RESOURCES_CAP - stock;
			const int64_t dailyIncome = incomeHandicapped[i];
			if (dailyIncome > 0)
				incomeHandicapped[i] = DiploEngine::saturatingMultiply(dailyIncome, diploIncomeFactor, room);
		}
	}
	else if (!state.isHuman())
	{
		// Initialize bonuses for different resources
		int difficultyIndex = gameHandler->gameState().getStartInfo()->difficulty;
		const std::string & difficultyName = GameConstants::DIFFICULTY_NAMES[difficultyIndex];
		const JsonNode & weeklyBonusesConfig = gameHandler->gameState().getSettings().getValue(EGameSettings::RESOURCES_WEEKLY_BONUSES_AI);
		const JsonNode & difficultyConfig = weeklyBonusesConfig[difficultyName];

		// Distribute weekly bonuses over 7 days, depending on the current day of the week
		for (GameResID i : LIBRARY->resourceTypeHandler->getAllObjects())
		{
			const std::string & name = i.toResource()->getJsonKey();
			int64_t weeklyBonus = difficultyConfig[name].Integer();
			int64_t dayOfWeek = gameHandler->gameState().getDate(Date::DAY_OF_WEEK);
			int64_t dailyIncome = incomeHandicapped[i];
			int64_t amountTillToday = dailyIncome * weeklyBonus * (dayOfWeek-1) / LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_DAYS_PER_WEEK) / 100;
			int64_t amountAfterToday = dailyIncome * weeklyBonus * dayOfWeek / LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_DAYS_PER_WEEK) / 100;
			int64_t dailyBonusToday = amountAfterToday - amountTillToday;
			int64_t totalIncomeToday = std::min(GameConstants::PLAYER_RESOURCES_CAP, incomeHandicapped[i] + dailyBonusToday);
			incomeHandicapped[i] = totalIncomeToday;
		}
	}

	return incomeHandicapped;
}

SetAvailableCreatures NewTurnProcessor::generateTownGrowth(const CGTownInstance * t, EWeekType weekType, CreatureID creatureWeek, bool firstDay, int additionalGrowth)
{
	SetAvailableCreatures sac;
	PlayerColor player = t->tempOwner;

	sac.tid = t->id;
	sac.creatures = t->creatures;

	for (int k=0; k < t->getTown()->creatures.size(); k++)
	{
		if (t->creatures.at(k).second.empty())
			continue;

		uint64_t creaturesBefore = t->creatures.at(k).first;
		uint64_t creatureGrowth = 0;
		const CCreature *cre = t->creatures.at(k).second.back().toCreature();

		if (firstDay)
		{
			creatureGrowth = cre->getGrowth();
			// DIPLO_ENGINE option 02: the first week's AI recruits use N as well (no finished week yet).
			const auto * diploOwner = player.isValidPlayer() ? gameHandler->gameInfo().getPlayerState(player, false) : nullptr;
			const int64_t diploFactor = diploOwner && !diploOwner->isHuman() ? DiploEngine::aiFactor(0, false) : 0;
			if (diploFactor > 1)
				creatureGrowth = DiploEngine::saturatingMultiply(creatureGrowth, diploFactor, std::numeric_limits<int32_t>::max());
		}
		else
		{
			creatureGrowth = t->creatureGrowth(k);

			//Deity of fire week - upgrade both imps and upgrades
			if (weekType == EWeekType::DEITYOFFIRE && vstd::contains(t->creatures.at(k).second, creatureWeek))
				creatureGrowth += additionalGrowth;

			//bonus week, effect applies only to identical creatures
			if (weekType == EWeekType::BONUS_GROWTH && cre->getId() == creatureWeek)
				creatureGrowth += additionalGrowth;
		}

		// Neutral towns have halved creature growth
		if (!player.isValidPlayer())
			creatureGrowth /= 2;

		uint64_t resultingCreatures = 0;

		if (weekType == EWeekType::PLAGUE)
			resultingCreatures = creaturesBefore / 2;
		else if (weekType == EWeekType::DOUBLE_GROWTH && vstd::contains(t->creatures.at(k).second, creatureWeek))
			resultingCreatures = (creaturesBefore + creatureGrowth) * 2;
		else
			resultingCreatures = creaturesBefore + creatureGrowth;

		// DIPLO_ENGINE: 64-bit sum, clamped to the engine's 32-bit creature counter.
		sac.creatures.at(k).first = static_cast<int32_t>(std::min<uint64_t>(resultingCreatures, std::numeric_limits<int32_t>::max()));
	}

	return sac;
}

void NewTurnProcessor::updateNeutralTownGarrison(const CGTownInstance * t, int currentWeek) const
{
	assert(t);
	assert(!t->getOwner().isValidPlayer());

	constexpr int randomRollsCounts = 3; // H3 makes around 3 random rolls to make simple bell curve distribution
	constexpr int upgradeChance = 5; // Chance for a unit to get an upgrade
	constexpr int growthChanceFort = 80; // Chance for growth to occur in towns with fort built
	constexpr int growthChanceVillage = 40; // Chance for growth to occur in towns without fort

	const auto & takeFromAvailable = [this, t](CreatureID creatureID)
	{
		int tierToSubstract = -1;
		for (int i = 0; i < t->getTown()->creatures.size(); ++i)
			if (vstd::contains(t->getTown()->creatures[i], creatureID))
				tierToSubstract = i;

		if (tierToSubstract == -1)
			return; // impossible?

		int creaturesAvailable = t->creatures[tierToSubstract].first;
		int creaturesRecruited = creatureID.toCreature()->getGrowth();
		int creaturesLeft = std::max(0, creaturesAvailable - creaturesRecruited);

		if (creaturesLeft != creaturesAvailable)
		{
			SetAvailableCreatures sac;
			sac.tid = t->id;
			sac.creatures = t->creatures;
			sac.creatures[tierToSubstract].first = creaturesLeft;
			gameHandler->sendAndApply(sac);
		}
	};

	int growthChance = t->hasFort()	? growthChanceFort : growthChanceVillage;
	int growthRoll = gameHandler->getRandomGenerator().nextInt(0, 99);

	if (growthRoll >= growthChance)
		return;

	int tierRoll = 0;
	for(int i = 0; i < randomRollsCounts; ++i)
		tierRoll += gameHandler->getRandomGenerator().nextInt(0, currentWeek);

	// NOTE: determined by observing H3 games, might not match H3 100%
	int tierToGrow = std::clamp(tierRoll / randomRollsCounts, 0, 6) + 1;

	bool upgradeUnit = gameHandler->getRandomGenerator().nextInt(0, 99) < upgradeChance;

	// Check if town garrison already has unit of specified tier
	for(const auto & slot : t->Slots())
	{
		const auto * creature = slot.second->getCreature();

		if (creature->getFactionID() != t->getFactionID())
			continue;

		if (creature->getLevel() != tierToGrow)
			continue;

		StackLocation stackLocation(t->id, slot.first);
		gameHandler->changeStackCount(stackLocation, creature->getGrowth(), ChangeValueMode::RELATIVE);
		takeFromAvailable(creature->getGrowth());

		if (upgradeUnit && !creature->upgrades.empty())
		{
			CreatureID upgraded = *RandomGeneratorUtil::nextItem(creature->upgrades, gameHandler->getRandomGenerator());
			gameHandler->changeStackType(stackLocation, upgraded.toCreature());
		}
		else
			gameHandler->changeStackType(stackLocation, creature);
		return;
	}

	// No existing creatures in garrison, but we have a free slot we can use
	SlotID freeSlotID = t->getFreeSlot();
	if (freeSlotID.validSlot())
	{
		for (auto const & tierVector : t->getTown()->creatures)
		{
			CreatureID baseCreature	= tierVector.at(0);

			if (baseCreature.toEntity(LIBRARY)->getLevel() != tierToGrow)
				continue;

			StackLocation stackLocation(t->id, freeSlotID);

			if (upgradeUnit && !baseCreature.toCreature()->upgrades.empty())
			{
				CreatureID upgraded = *RandomGeneratorUtil::nextItem(baseCreature.toCreature()->upgrades, gameHandler->getRandomGenerator());
				gameHandler->insertNewStack(stackLocation, upgraded.toCreature(), upgraded.toCreature()->getGrowth());
				takeFromAvailable(upgraded.toCreature()->getGrowth());
			}
			else
			{
				gameHandler->insertNewStack(stackLocation, baseCreature.toCreature(), baseCreature.toCreature()->getGrowth());
				takeFromAvailable(baseCreature.toCreature()->getGrowth());
			}

			return;
		}
	}
}

RumorState NewTurnProcessor::pickNewRumor()
{
	RumorState newRumor;

	static const std::vector<RumorState::ERumorType> rumorTypes = {RumorState::TYPE_MAP, RumorState::TYPE_SPECIAL, RumorState::TYPE_RAND, RumorState::TYPE_RAND};
	std::vector<RumorState::ERumorTypeSpecial> sRumorTypes = {
															  RumorState::RUMOR_OBELISKS, RumorState::RUMOR_ARTIFACTS, RumorState::RUMOR_ARMY, RumorState::RUMOR_INCOME};
	if(gameHandler->gameState().getMap().grailPos.isValid()) // Grail should always be on map, but I had related crash I didn't manage to reproduce
		sRumorTypes.push_back(RumorState::RUMOR_GRAIL);

	int rumorId = -1;
	int rumorExtra = -1;
	auto & rand = gameHandler->getRandomGenerator();
	newRumor.type = *RandomGeneratorUtil::nextItem(rumorTypes, rand);

	do
	{
		switch(newRumor.type)
		{
			case RumorState::TYPE_SPECIAL:
			{
				SThievesGuildInfo tgi;
				gameHandler->gameState().obtainPlayersStats(tgi, 20);
				rumorId = *RandomGeneratorUtil::nextItem(sRumorTypes, rand);
				if(rumorId == RumorState::RUMOR_GRAIL)
				{
					rumorExtra = gameHandler->gameState().getTile(gameHandler->gameState().getMap().grailPos)->getTerrainID().getNum();
					break;
				}

				std::vector<PlayerColor> players = {};
				switch(rumorId)
				{
					case RumorState::RUMOR_OBELISKS:
						players = tgi.obelisks[0];
						break;

					case RumorState::RUMOR_ARTIFACTS:
						players = tgi.artifacts[0];
						break;

					case RumorState::RUMOR_ARMY:
						players = tgi.army[0];
						break;

					case RumorState::RUMOR_INCOME:
						players = tgi.income[0];
						break;
				}
				rumorExtra = RandomGeneratorUtil::nextItem(players, rand)->getNum();

				break;
			}
			case RumorState::TYPE_MAP:
				// Makes sure that map rumors only used if there enough rumors too choose from
				if(!gameHandler->gameState().getMap().rumors.empty() && (gameHandler->gameState().getMap().rumors.size() > 1 || !gameHandler->gameState().currentRumor.last.count(RumorState::TYPE_MAP)))
				{
					rumorId = rand.nextInt(gameHandler->gameState().getMap().rumors.size() - 1);
					break;
				}
				else
					newRumor.type = RumorState::TYPE_RAND;
				[[fallthrough]];

			case RumorState::TYPE_RAND:
				auto vector = LIBRARY->generaltexth->findStringsWithPrefix("core.randtvrn");
				rumorId = rand.nextInt(static_cast<int>(vector.size()) - 1);

				break;
		}
	}
	while(!newRumor.update(rumorId, rumorExtra));

	return newRumor;
}

std::tuple<EWeekType, CreatureID, int> NewTurnProcessor::pickWeekType(bool newMonth)
{
	// creatures without a map object cannot be spawned as wandering monsters, so exclude them from special weeks
	auto canSpawnAsMapObject = [](const CreatureID & creatureID)
	{
		return LIBRARY->objtypeh->knownSubObjects(Obj::MONSTER).contains(creatureID.getNum());
	};

	std::vector<std::tuple<CreatureID, int>> creaturesWithDeityOfFireBonus;
	for(const auto & bonus : *gameHandler->gameState().globalEffects.getBonusesOfType(BonusType::DEITYOFFIRE))
	{
		CreatureID creatureID = bonus->subtype.as<CreatureID>();
		if(canSpawnAsMapObject(creatureID))
			creaturesWithDeityOfFireBonus.push_back({creatureID, bonus->val});
	}
	if(!creaturesWithDeityOfFireBonus.empty())
	{
		auto item = *RandomGeneratorUtil::nextItem(creaturesWithDeityOfFireBonus, gameHandler->getRandomGenerator());
		return { EWeekType::DEITYOFFIRE, std::get<0>(item), std::get<1>(item)};
	}

	if(!gameHandler->gameInfo().getSettings().getBoolean(EGameSettings::CREATURES_ALLOW_RANDOM_SPECIAL_WEEKS))
		return { EWeekType::NORMAL, CreatureID::NONE, 0};

	int monthType = gameHandler->getRandomGenerator().nextInt(99);
	if (newMonth) //new month
	{
		if (monthType < gameHandler->gameInfo().getSettings().getInteger(EGameSettings::CREATURES_MONTH_DOUBLE_GROWTH_PROBABILITY)) //double growth
		{
			if (gameHandler->gameInfo().getSettings().getBoolean(EGameSettings::CREATURES_ALLOW_ALL_FOR_DOUBLE_MONTH))
			{
				CreatureID creatureID = gameHandler->randomizer->rollCreature();
				return { EWeekType::DOUBLE_GROWTH, creatureID, 0};
			}
			else
			{
				std::vector<CreatureID> spawnableDoubledCreatures;
				for(const auto & creatureID : LIBRARY->creh->doubledCreatures)
					if(canSpawnAsMapObject(creatureID))
						spawnableDoubledCreatures.push_back(creatureID);

				if(!spawnableDoubledCreatures.empty())
				{
					CreatureID creatureID = *RandomGeneratorUtil::nextItem(spawnableDoubledCreatures, gameHandler->getRandomGenerator());
					return { EWeekType::DOUBLE_GROWTH, creatureID, 0};
				}
				else
				{
					gameHandler->complain("Cannot find creature that can be spawned!");
					return { EWeekType::NORMAL, CreatureID::NONE, 0};
				}
			}
		}

		if (monthType < gameHandler->gameInfo().getSettings().getInteger(EGameSettings::CREATURES_MONTH_DOUBLE_GROWTH_PROBABILITY) +
						 gameHandler->gameInfo().getSettings().getInteger(EGameSettings::CREATURES_MONTH_PLAGUE_PROBABILITY)) //plague
			return { EWeekType::PLAGUE, CreatureID::NONE, 0};

		return { EWeekType::NORMAL, CreatureID::NONE, 0};
	}
	else //it's a week, but not full month
	{
		if (monthType < gameHandler->gameInfo().getSettings().getInteger(EGameSettings::CREATURES_WEEK_SPECIAL_PROBABILITY))
		{
			std::pair<int, CreatureID> newMonster(54, CreatureID());
			do
			{
				newMonster.second = gameHandler->randomizer->rollCreature();
			} while (newMonster.second.toEntity(LIBRARY)->getFactionID().toFaction()->town == nullptr); // find first non neutral creature

			return { EWeekType::BONUS_GROWTH, newMonster.second, gameHandler->gameInfo().getSettings().getInteger(EGameSettings::CREATURES_ADDITIONAL_WEEKLY_GROWTH_SPECIAL_WEEK)};
		}
		return { EWeekType::NORMAL, CreatureID::NONE, 0};
	}
}

std::vector<SetMana> NewTurnProcessor::updateHeroesManaPoints()
{
	std::vector<SetMana> result;

	for (const auto & elem : gameHandler->gameState().players)
	{
		for (const CGHeroInstance *h : elem.second.getHeroes())
		{
			int32_t newMana = h->getManaNewTurn();

			if (newMana != h->mana)
				result.emplace_back(h->id, newMana, ChangeValueMode::ABSOLUTE);
		}
	}
	return result;
}

std::vector<SetMovePoints> NewTurnProcessor::updateHeroesMovementPoints()
{
	std::vector<SetMovePoints> result;

	for (const auto & elem : gameHandler->gameState().players)
	{
		for (const CGHeroInstance *h : elem.second.getHeroes())
		{
			auto ti = h->getTurnInfo(1);
			// NOTE: this code executed when bonuses of previous day not yet updated (this happen in NewTurn::applyGs). See issue 2356
			int32_t newMovementPoints = h->movementPointsLimitCached(gameHandler->gameState().getMap().getTile(h->visitablePos()).isLand(), ti.get());

			if (newMovementPoints != h->movementPointsRemaining())
				result.emplace_back(h->id, newMovementPoints);
		}
	}
	return result;
}

InfoWindow NewTurnProcessor::createInfoWindow(EWeekType weekType, CreatureID creatureWeek, bool newMonth, int additionalGrowth)
{
	InfoWindow iw;
	switch (weekType)
	{
		case EWeekType::DOUBLE_GROWTH:
			iw.text.appendLocalString(EMetaText::ARRAY_TXT, 131);
			iw.text.replaceNameSingular(creatureWeek);
			iw.text.replaceNameSingular(creatureWeek);
			break;
		case EWeekType::PLAGUE:
			iw.text.appendLocalString(EMetaText::ARRAY_TXT, 132);
			break;
		case EWeekType::BONUS_GROWTH:
			iw.text.appendLocalString(EMetaText::ARRAY_TXT, 134);
			iw.text.replaceNameSingular(creatureWeek);
			iw.text.replaceNameSingular(creatureWeek);
			break;
		case EWeekType::DEITYOFFIRE:
		{
			const CCreature *base = creatureWeek.toCreature();
			CreatureID upgradedCreature = creatureWeek;
			if(base && !base->upgrades.empty())
				upgradedCreature = *base->upgrades.rbegin();
			iw.text.appendLocalString(EMetaText::ARRAY_TXT, 135);
			iw.text.replaceNameSingular(creatureWeek);
			iw.text.replaceNameSingular(creatureWeek);
			iw.text.replacePositiveNumber(additionalGrowth);
			iw.text.replaceNameSingular(upgradedCreature);
			iw.text.replacePositiveNumber(additionalGrowth);
			break;
		}
		default:
			if (newMonth)
			{
				iw.text.appendLocalString(EMetaText::ARRAY_TXT, 130);
				iw.text.replaceLocalString(EMetaText::ARRAY_TXT, gameHandler->getRandomGenerator().nextInt(32, 41));
			}
			else
			{
				iw.text.appendLocalString(EMetaText::ARRAY_TXT, 133);
				iw.text.replaceLocalString(EMetaText::ARRAY_TXT, gameHandler->getRandomGenerator().nextInt(43, 57));
			}
	}
	return iw;
}

NewTurn NewTurnProcessor::generateNewTurnPack()
{
	NewTurn n;
	n.specialWeek = EWeekType::FIRST_WEEK;
	n.creatureid = CreatureID::NONE;
	n.day = gameHandler->gameState().day + 1;

	int daysPerWeek = LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_DAYS_PER_WEEK);
	int daysPerMonth = LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_WEEKS_PER_MONTH) * daysPerWeek;

	bool firstTurn = !gameHandler->gameInfo().getDate(Date::DAY);
	bool newWeek = gameHandler->gameInfo().getDate(Date::DAY_OF_WEEK) == daysPerWeek; //day numbers are confusing, as day was not yet switched
	bool newMonth = gameHandler->gameInfo().getDate(Date::DAY_OF_MONTH) == daysPerMonth;

	int additionalGrowth = 0;

	if (!firstTurn)
	{
		for (const auto & player : gameHandler->gameState().players)
			n.playerIncome[player.first] = generatePlayerIncome(player.first, newWeek);
	}

	if (newWeek && !firstTurn)
	{
		auto [specialWeek, creatureID, addGrowth] = pickWeekType(newMonth);
		n.specialWeek = specialWeek;
		n.creatureid = creatureID;
		additionalGrowth = addGrowth;
	}

	n.heroesMana = updateHeroesManaPoints();
	n.heroesMovement = updateHeroesMovementPoints();

	if (newWeek)
	{
		for (const auto & townID : gameHandler->gameState().getMap().getAllTowns())
		{
			const auto * t = gameHandler->gameState().getTown(townID);
			n.availableCreatures.push_back(generateTownGrowth(t, n.specialWeek, n.creatureid, firstTurn, additionalGrowth));
		}

		n.newRumor = pickNewRumor();

		//new week info popup
		if (n.specialWeek != EWeekType::FIRST_WEEK)
			n.newWeekNotification = createInfoWindow(n.specialWeek, n.creatureid, newMonth, additionalGrowth);
	}

	return n;
}

void NewTurnProcessor::onNewTurn()
{
	NewTurn n = generateNewTurnPack();

	int daysPerWeek = LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_DAYS_PER_WEEK);
	int daysPerMonth = LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_WEEKS_PER_MONTH) * daysPerWeek;

	bool firstTurn = !gameHandler->gameInfo().getDate(Date::DAY);
	bool newWeek = gameHandler->gameInfo().getDate(Date::DAY_OF_WEEK) == daysPerWeek; //day numbers are confusing, as day was not yet switched
	bool newMonth = gameHandler->gameInfo().getDate(Date::DAY_OF_MONTH) == daysPerMonth;

	gameHandler->sendAndApply(n);

	if (newWeek)
	{
		for (const auto & townID : gameHandler->gameState().getMap().getAllTowns())
		{
			const auto * t = gameHandler->gameState().getTown(townID);
			if (t->hasBuilt(BuildingSubID::PORTAL_OF_SUMMONING))
				gameHandler->setPortalDwelling(t, true, n.specialWeek == EWeekType::PLAGUE); //set creatures for Portal of Summoning
		}
	}

	if (newWeek && !firstTurn)
	{
		for (const auto & townID : gameHandler->gameState().getMap().getAllTowns())
		{
			const auto * t = gameHandler->gameState().getTown(townID);
			if (!t->getOwner().isValidPlayer())
				updateNeutralTownGarrison(t, 1 + gameHandler->gameInfo().getDate(Date::DAY) / LIBRARY->engineSettings()->getInteger(EGameSettings::GENERAL_DAYS_PER_WEEK));
		}
	}

	//spawn wandering monsters
	if (newMonth && (n.specialWeek == EWeekType::DOUBLE_GROWTH || n.specialWeek == EWeekType::DEITYOFFIRE))
	{
		gameHandler->spawnWanderingMonsters(n.creatureid);
	}

	// DIPLO_ENGINE option 12: weekly neutral stacks; the month rule above is unchanged.
	if (newWeek && !firstTurn)
		DiploEngine::spawnWeeklyMonsters(gameHandler, 1 + gameHandler->gameInfo().getDate(Date::DAY) / daysPerWeek);

	logGlobal->trace("Info about turn %d has been sent!", n.day);
}
