/*
 * DotPackCommand.h
 *
 *  Created on: 11/08/2010
 *      Author: victor
 */

#ifndef DOTPACKCOMMAND_H_
#define DOTPACKCOMMAND_H_

#include "server/zone/objects/building/BuildingObject.h"
#include "server/zone/objects/scene/SceneObject.h"
#include "server/zone/objects/tangible/pharmaceutical/DotPack.h"
#include "server/zone/ZoneServer.h"
#include "server/zone/Zone.h"
#include "server/zone/managers/player/PlayerManager.h"
#include "server/zone/managers/collision/CollisionManager.h"
#include "server/zone/objects/creature/buffs/DelayedBuff.h"
#include "server/zone/packets/object/CombatAction.h"
#include "QueueCommand.h"
#include "server/zone/managers/combat/CombatManager.h"
#include "server/zone/objects/tangible/threat/ThreatMap.h"

class DotPackCommand : public QueueCommand {
protected:
	String effectName;
	String skillName;
public:
	DotPackCommand(const String& name, ZoneProcessServer* server)
		: QueueCommand(name, server) {
		effectName = "clienteffect/healing_healenhance.cef";
		//defaultTime = 0;
	}

	void doAnimationsRange(CreatureObject* creature, CreatureObject* creatureTarget, uint64 oid, float range, bool area) const {
		String crc;

		if (range < 10.0f) {
			if (area)
				crc = "throw_grenade_near_area_poison";
			else
				crc = "throw_grenade_near_poison";
		}
		else if (10.0f <= range && range < 20.f) {
			if (area)
				crc = "throw_grenade_medium_area_poison";
			else
				crc = "throw_grenade_medium_poison";
		}
		else {
			if (area)
				crc = "throw_grenade_far_area_poison";
			else
				crc = "throw_grenade_far_poison";
		}

		CombatAction* action = new CombatAction(creature, creatureTarget,  crc.hashCode(), 1, 0L);

		creature->broadcastMessage(action, true);
	}

	DotPack* findDotPack(CreatureObject* creature, uint8 pool, bool poolGiven) const {
		SceneObject* inventory = creature->getSlottedObject("inventory");

		if (inventory == nullptr) {
			return nullptr;
		}

		for (int i = 0; i < inventory->getContainerObjectsSize(); ++i) {
			SceneObject* item = inventory->getContainerObject(i);

			if (!item->isDotPackObject()) {
				continue;
			}

			DotPack* pack = cast<DotPack*>(item);

			if ((skillName == "applypoison") && pack->isPoisonDeliveryUnit()) {
				if (!poolGiven) {
					return pack;
				} else if (pack->getPool() == pool) {
					return pack;
				}
			}

			if ((skillName == "applydisease") && pack->isDiseaseDeliveryUnit()) {
				if (!poolGiven) {
					return pack;
				} else if (pack->getPool() == pool) {
					return pack;
				}
			}
		}

		return nullptr;
	}

	void parseModifier(const String& modifier, uint8& pool, uint64& objectId) const {
		if (!modifier.isEmpty()) {
			StringTokenizer tokenizer(modifier);
			tokenizer.setDelimeter("|");

			String poolName;

			tokenizer.getStringToken(poolName);
			pool = BuffAttribute::getAttribute(poolName);

			if (tokenizer.hasMoreTokens()) {
				objectId = tokenizer.getLongToken();
			}
		} else {
			pool = BuffAttribute::UNKNOWN;
			objectId = 0;
		}
	}

	bool checkTarget(CreatureObject* creature, CreatureObject* targetCreature, uint32 dotType) const {
		if (!targetCreature->isAttackableBy(creature))
			return false;

		if (targetCreature->hasDotImmunity(dotType))
			return false;

		if (creature != targetCreature && !CollisionManager::checkLineOfSight(creature, targetCreature))
			return false;

		return true;
	}

	void awardXp(CreatureObject* creature, const String& type, int power) const {
		if (!creature->isPlayerCreature())
			return;

		CreatureObject* player = cast<CreatureObject*>(creature);

		int amount = (int)round((float)power);

		if (amount <= 0)
			return;

		PlayerManager* playerManager = server->getZoneServer()->getPlayerManager();
		playerManager->awardExperience(player, type, amount, true);
	}

	void handleArea(CreatureObject* creature, CreatureObject* areaCenter, DotPack* pharma,
			float range) const {

		Zone* zone = creature->getZone();

		if (zone == nullptr)
			return;


		//TODO: Convert this to a CombatManager::getAreaTargets call
		try {
			SortedVector<QuadTreeEntry*> closeObjects;
			CloseObjectsVector* vec = (CloseObjectsVector*) areaCenter->getCloseObjects();
			vec->safeCopyReceiversTo(closeObjects, CloseObjectsVector::CREOTYPE);

			for (int i = 0; i < closeObjects.size(); i++) {
				SceneObject* object = static_cast<SceneObject*>( closeObjects.get(i));

				if (!object->isCreatureObject())
					continue;

				if (object == areaCenter || object == creature)
					continue;

				if (areaCenter->getWorldPosition().distanceTo(object->getWorldPosition()) - object->getTemplateRadius() > range)
					continue;

				if (creature->isPlayerCreature() && object->getParentID() != 0 && creature->getParentID() != object->getParentID()) {
					Reference<CellObject*> targetCell = object->getParent().get().castTo<CellObject*>();

					if (targetCell != nullptr) {
						if (object->isPlayerCreature()) {
							auto perms = targetCell->getContainerPermissions();

							if (!perms->hasInheritPermissionsFromParent()) {
								if (!targetCell->checkContainerPermission(creature, ContainerPermissions::WALKIN))
									continue;
							}
						}

						ManagedReference<SceneObject*> parentSceneObject = targetCell->getParent().get();

						if (parentSceneObject != nullptr) {
							BuildingObject* buildingObject = parentSceneObject->asBuildingObject();

							if (buildingObject != nullptr && !buildingObject->isAllowedEntry(creature))
								continue;
						}
					}
				}

				CreatureObject* creatureTarget = cast<CreatureObject*>( object);

				try {
					Locker crossLocker(creatureTarget, creature);

					if (!creatureTarget->isAttackableBy(creature))
						continue;

					if (checkTarget(creature, creatureTarget, pharma->getDotType()))
						doAreaMedicActionTarget(creature, creatureTarget, pharma);

				} catch (Exception& e) {
				}
			}
		} catch (Exception& e) {
		}
	}

	void doAreaMedicActionTarget(CreatureObject* creature, CreatureObject* creatureTarget, DotPack* dotPack) const {
		int dotPower = dotPack->calculatePower(creature);

		//sendDotMessage(creature, creatureTarget, dotPower);

		int dotDMG = 0;
		if (dotPack->isPoisonDeliveryUnit()) {
			StringIdChatParameter stringId("healing", "apply_poison_self");
			stringId.setTT(creatureTarget->getObjectID());

			creature->sendSystemMessage(stringId);

			StringIdChatParameter stringId2("healing", "apply_poison_other");
			stringId2.setTU(creature->getObjectID());

			creatureTarget->sendSystemMessage(stringId2);

			dotDMG = creatureTarget->addDotState(creature, CreatureState::POISONED, dotPack->getServerObjectCRC(), dotPower, dotPack->getPool(), dotPack->getDuration(), dotPack->getPotency(), creatureTarget->getSkillMod("resistance_poison") + creatureTarget->getSkillMod("poison_disease_resist"));
		} else {
			StringIdChatParameter stringId("healing", "apply_disease_self");
			stringId.setTT(creatureTarget->getObjectID());

			creature->sendSystemMessage(stringId);

			StringIdChatParameter stringId2("healing", "apply_disease_other");
			stringId2.setTU(creature->getObjectID());

			creatureTarget->sendSystemMessage(stringId2);

			dotDMG = creatureTarget->addDotState(creature, CreatureState::DISEASED, dotPack->getServerObjectCRC(), dotPower, dotPack->getPool(), dotPack->getDuration(), dotPack->getPotency(), creatureTarget->getSkillMod("resistance_disease") + creatureTarget->getSkillMod("poison_disease_resist"));
		}

		if (dotDMG) {
			awardXp(creature, "medical", dotDMG); //No experience for healing yourself.

			creatureTarget->addDefender(creature);
			creatureTarget->getThreatMap()->addDamage(creature, dotDMG, "");
			creature->addDefender(creatureTarget);
		} else {
			StringIdChatParameter stringId("dot_message", "dot_resisted");
			stringId.setTT(creatureTarget->getObjectID());

			creature->sendSystemMessage(stringId);
		}

		checkForTef(creature, creatureTarget);
	}

	int hasCost(CreatureObject* creature) const {
		if (!creature->isPlayerCreature())
			return 0;

		CreatureObject* player = cast<CreatureObject*>(creature);

		int wpnMind = 150;

		int mindAttackCost = wpnMind - (wpnMind * creature->getHAM(CreatureAttribute::FOCUS) / 1500);

		if (mindAttackCost < 0)
			mindAttackCost = 0;

		if (player->getHAM(CreatureAttribute::MIND) < mindAttackCost)
			return -1;

		return mindAttackCost;
	}

	void applyCost(CreatureObject* creature, int mindDamage) const {
		if (mindDamage == 0)
			return;

		creature->inflictDamage(creature, CreatureAttribute::MIND, mindDamage, false);
	}

	int doQueueCommand(CreatureObject* creature, const uint64& target, const UnicodeString& arguments) const {

		int result = doCommonMedicalCommandChecks(creature);

		if (result != SUCCESS)
			return result;

		int cost = hasCost(creature);

		if (cost < 0)
			return INSUFFICIENTHAM;

		ManagedReference<SceneObject*> object = server->getZoneServer()->getObject(target);
		if (object == nullptr || !object->isCreatureObject() || creature == object)
			return INVALIDTARGET;


		uint8 pool = BuffAttribute::UNKNOWN;
		bool poolGiven = false;
		uint64 objectId = 0;

		ManagedReference<DotPack*> dotPack;

		parseModifier(arguments.toString(), pool, objectId);

		if (objectId == 0) {
			if (pool != BuffAttribute::UNKNOWN) {
				poolGiven = true;
			}

			dotPack = findDotPack(creature, pool, poolGiven);
		} else {
			SceneObject* inventory = creature->getSlottedObject("inventory");

			if (inventory != nullptr) {
				dotPack = inventory->getContainerObject(objectId).castTo<DotPack*>();
			}
		}

		if (dotPack == nullptr) {
			return GENERALERROR;
		}

		PlayerManager* playerManager = server->getPlayerManager();
		CombatManager* combatManager = CombatManager::instance();

		CreatureObject* creatureTarget = cast<CreatureObject*>(object.get());

		if (creature != creatureTarget && !CollisionManager::checkLineOfSight(creature, creatureTarget)) {
			creature->sendSystemMessage("@healing:no_line_of_sight"); // You cannot see your target.
			return GENERALERROR;
		}

		ManagedReference<SceneObject*> targetObject = server->getZoneServer()->getObject(target);

		if (creature->isPlayerCreature() && targetObject->getParentID() != 0 && creature->getParentID() != targetObject->getParentID()) {
			Reference<CellObject*> targetCell = targetObject->getParent().get().castTo<CellObject*>();

			if (targetCell != nullptr) {
				if (!targetObject->isPlayerCreature()) {
					auto perms = targetCell->getContainerPermissions();

					if (!perms->hasInheritPermissionsFromParent()) {
						if (!targetCell->checkContainerPermission(creature, ContainerPermissions::WALKIN)) {
							creature->sendSystemMessage("@combat_effects:cansee_fail"); // You cannot see your target.
							return GENERALERROR;
						}
					}
				}

				ManagedReference<SceneObject*> parentSceneObject = targetCell->getParent().get();

				if (parentSceneObject != nullptr) {
					BuildingObject* buildingObject = parentSceneObject->asBuildingObject();

					if (buildingObject != nullptr && !buildingObject->isAllowedEntry(creature)) {
						creature->sendSystemMessage("@combat_effects:cansee_fail"); // You cannot see your target.
						return GENERALERROR;
					}
				}
			}
		}

		int	range = int(dotPack->getRange() + creature->getSkillMod("healing_range") / 100 * 14);

		if(!checkDistance(creature, creatureTarget, range))
					return TOOFAR;
		//timer
		if (!creature->checkCooldownRecovery(skillName)) {
			creature->sendSystemMessage("@healing_response:healing_must_wait"); //You must wait before you can do that.
			return GENERALERROR;

		} else {
			float modSkill = (float)creature->getSkillMod("healing_range_speed");
			int delay = (int)round(12.0f - (6.0f * modSkill / 100 ));

			if (creature->hasBuff(BuffCRC::FOOD_HEAL_RECOVERY)) {
				DelayedBuff* buff = cast<DelayedBuff*>( creature->getBuff(BuffCRC::FOOD_HEAL_RECOVERY));

				if (buff != nullptr) {
					float percent = buff->getSkillModifierValue("heal_recovery");

					delay = round(delay * (100.0f - percent) / 100.0f);
				}
			}

			//Force the delay to be at least 4 seconds.
			delay = (delay < 4) ? 4 : delay;

			creature->addCooldown(skillName, delay * 1000);
		}

		Locker clocker(creatureTarget, creature);

		if (!combatManager->startCombat(creature, creatureTarget))
			return INVALIDTARGET;

		applyCost(creature, cost);

		int dotPower = dotPack->calculatePower(creature);
		int dotDMG = 0;

		if (dotPack->isPoisonDeliveryUnit()) {
			if (!creatureTarget->hasDotImmunity(dotPack->getDotType())) {
				StringIdChatParameter stringId("healing", "apply_poison_self");
				stringId.setTT(creatureTarget->getObjectID());

				creature->sendSystemMessage(stringId);

				StringIdChatParameter stringId2("healing", "apply_poison_other");
				stringId2.setTU(creature->getObjectID());

				creatureTarget->sendSystemMessage(stringId2);

				dotDMG = creatureTarget->addDotState(creature, CreatureState::POISONED, dotPack->getServerObjectCRC(), dotPower, dotPack->getPool(), dotPack->getDuration(), dotPack->getPotency(), creatureTarget->getSkillMod("resistance_poison") + creatureTarget->getSkillMod("poison_disease_resist"));
			}

		} else {
			if (!creatureTarget->hasDotImmunity(dotPack->getDotType())) {
				StringIdChatParameter stringId("healing", "apply_disease_self");
				stringId.setTT(creatureTarget->getObjectID());

				creature->sendSystemMessage(stringId);

				StringIdChatParameter stringId2("healing", "apply_disease_other");
				stringId2.setTU(creature->getObjectID());

				creatureTarget->sendSystemMessage(stringId2);

				dotDMG = creatureTarget->addDotState(creature, CreatureState::DISEASED, dotPack->getServerObjectCRC(), dotPower, dotPack->getPool(), dotPack->getDuration(), dotPack->getPotency(), creatureTarget->getSkillMod("resistance_disease") + creatureTarget->getSkillMod("poison_disease_resist"));
			}
		}

		if (dotDMG) {
			awardXp(creature, "medical", dotDMG); //No experience for healing yourself.
			creatureTarget->getThreatMap()->addDamage(creature, dotDMG, "");
		} else {
			StringIdChatParameter stringId("dot_message", "dot_resisted");
			stringId.setTT(creatureTarget->getObjectID());

			creature->sendSystemMessage(stringId);

			StringIdChatParameter stringId2("healing", "dot_resist_other");

			creatureTarget->sendSystemMessage(stringId2);
		}

		if (dotPack->isArea()) {
			if (creatureTarget != creature)
				clocker.release();

			handleArea(creature, creatureTarget, dotPack, dotPack->getArea());
		}

		if (dotPack != nullptr) {
			if (creatureTarget != creature)
				clocker.release();

			Locker dlocker(dotPack, creature);
			dotPack->decreaseUseCount();
		}

		if (creature->isPlayerCreature()) {
			bool shouldGcwCrackdownTef = false, shouldGcwTef = false, shouldBhTef = false;

			CombatManager::instance()->checkForTefs(creature, creatureTarget, &shouldGcwCrackdownTef, &shouldGcwTef, &shouldBhTef);

			PlayerObject* ghost = creature->getPlayerObject().get();

			if (ghost != nullptr) {
				ghost->updateLastCombatActionTimestamp(shouldGcwCrackdownTef, shouldGcwTef, shouldBhTef);
			}
		}

		doAnimationsRange(creature, creatureTarget, dotPack->getObjectID(), creature->getWorldPosition().distanceTo(creatureTarget->getWorldPosition()), dotPack->isArea());

		creature->notifyObservers(ObserverEventType::MEDPACKUSED);

		return SUCCESS;
	}

};

#endif /* DOTPACKCOMMAND_H_ */
