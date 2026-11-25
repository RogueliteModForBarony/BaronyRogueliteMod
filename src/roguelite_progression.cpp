#include "roguelite_progression.hpp"

#include "files.hpp"
#include "game.hpp"
#include "stat.hpp"

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Roguelite
{
	namespace
	{
		struct Profile
		{
			std::unordered_set<std::string> unlocked;
		};

		struct ScoreProfile
		{
			uint64_t lifetimeScore = 0;
			uint64_t earnedPoints = 0;
			uint64_t scoreProgress = 0;
		};

		constexpr uint64_t legacyScorePerPoint = 25000;

		bool initialized = false;
		bool configValid = false;
		bool featureEnabled = false;
		bool profileLoaded = false;
		bool scoresLoaded = false;
		std::array<bool, MAXPLAYERS> rewardGrantedThisRun;
		int pointsPerScore = 0;
		uint32_t treeHash = 2166136261u;
		std::vector<Node> treeNodes;
		std::unordered_map<std::string, size_t> treeNodeIndices;
		std::array<Profile, MAXPLAYERS> profiles;
		std::array<ScoreProfile, MAXPLAYERS> scoreProfiles;
		std::array<bool, MAXPLAYERS> bonusesApplied;

		const char* treeConfigPath = "data/roguelite_skill_tree.json";
		const char* profilePath = "savegames/roguelite_progress.json";
		const char* scorePath = "savegames/roguelite_score.json";

		void hashBytes(const char* bytes, size_t length)
		{
			for ( size_t i = 0; i < length; ++i )
			{
				treeHash ^= static_cast<unsigned char>(bytes[i]);
				treeHash *= 16777619u;
			}
		}

		void hashString(const std::string& value)
		{
			hashBytes(value.data(), value.size());
			const char separator = '\0';
			hashBytes(&separator, 1);
		}

		bool isSupportedStat(const std::string& stat)
		{
			return stat == "max_hp" || stat == "max_mp"
				|| stat == "strength" || stat == "dexterity"
				|| stat == "constitution" || stat == "intelligence"
				|| stat == "perception" || stat == "charisma"
				|| stat == "strength_skill_growth" || stat == "dexterity_skill_growth"
				|| stat == "constitution_skill_growth" || stat == "intelligence_skill_growth"
				|| stat == "perception_skill_growth" || stat == "charisma_skill_growth"
				|| stat == "xp_growth" || stat == "gold"
				|| stat == "magic_resistance" || stat == "burning_resistance"
				|| stat == "poison_resistance" || stat == "weight_reduction"
				|| stat == "food_meat";
		}

		bool isSupportedNodeType(const std::string& type)
		{
			return type == "strength" || type == "dexterity"
				|| type == "constitution" || type == "intelligence"
				|| type == "perception" || type == "charisma";
		}

		bool readJsonFile(const char* virtualPath, rapidjson::Document& document)
		{
			const char* realDirectory = PHYSFS_getRealDir(virtualPath);
			if ( !realDirectory )
			{
				return false;
			}

			std::string path(realDirectory);
			path.append(PHYSFS_getDirSeparator()).append(virtualPath);
			File* file = FileIO::open(path.c_str(), "rb");
			if ( !file )
			{
				printlog("[Roguelite]: Failed to open '%s'.", path.c_str());
				return false;
			}
			const size_t fileSize = file->size();
			if ( fileSize == 0 || fileSize > 1024 * 1024 )
			{
				printlog("[Roguelite]: Invalid JSON file size for '%s'.", path.c_str());
				FileIO::close(file);
				return false;
			}
			std::vector<char> buffer(fileSize + 1, '\0');
			const size_t bytesRead = file->read(buffer.data(), sizeof(char), fileSize);
			FileIO::close(file);
			if ( bytesRead != fileSize )
			{
				printlog("[Roguelite]: Failed to read '%s'.", path.c_str());
				return false;
			}

			document.Parse(buffer.data());
			if ( document.HasParseError() || !document.IsObject() )
			{
				printlog("[Roguelite]: Invalid JSON in '%s'.", path.c_str());
				return false;
			}
			return true;
		}

		bool hasValidRequires(const Node& node)
		{
			for ( const auto& requirement : node.prerequisites )
			{
				if ( requirement == node.id || !findNode(requirement) )
				{
					return false;
				}
			}
			return true;
		}

		bool visitForCycle(const std::string& nodeId, std::unordered_set<std::string>& visiting,
			std::unordered_set<std::string>& visited)
		{
			if ( visited.count(nodeId) )
			{
				return true;
			}
			if ( !visiting.insert(nodeId).second )
			{
				return false;
			}
			const Node* node = findNode(nodeId);
			if ( !node )
			{
				return false;
			}
			for ( const auto& requirement : node->prerequisites )
			{
				if ( !visitForCycle(requirement, visiting, visited) )
				{
					return false;
				}
			}
			visiting.erase(nodeId);
			visited.insert(nodeId);
			return true;
		}

		bool parseConfig()
		{
			rapidjson::Document document;
			if ( !readJsonFile(treeConfigPath, document) )
			{
				printlog("[Roguelite]: Feature disabled; configuration '%s' was not loaded.", treeConfigPath);
				return false;
			}

			if ( !document.HasMember("version") || !document["version"].IsInt()
				|| document["version"].GetInt() != 1
				|| !document.HasMember("enabled") || !document["enabled"].IsBool()
				|| !document.HasMember("score_per_point") || !document["score_per_point"].IsInt()
				|| document["score_per_point"].GetInt() <= 0
				|| !document.HasMember("nodes") || !document["nodes"].IsArray() )
			{
				printlog("[Roguelite]: Invalid required fields in '%s'.", treeConfigPath);
				return false;
			}

			std::vector<Node> parsedNodes;
			std::unordered_set<std::string> ids;
			for ( const auto& entry : document["nodes"].GetArray() )
			{
				if ( !entry.IsObject() || !entry.HasMember("id") || !entry["id"].IsString()
					|| !entry.HasMember("name") || !entry["name"].IsString()
					|| !entry.HasMember("x") || !entry["x"].IsInt()
					|| !entry.HasMember("y") || !entry["y"].IsInt()
					|| !entry.HasMember("cost") || !entry["cost"].IsInt()
					|| entry["cost"].GetInt() <= 0
					|| !entry.HasMember("size") || !entry["size"].IsString()
					|| !entry.HasMember("type") || !entry["type"].IsString()
					|| !entry.HasMember("effects") || !entry["effects"].IsArray() )
				{
					printlog("[Roguelite]: Skipping a malformed node in '%s'.", treeConfigPath);
					return false;
				}

				Node node;
				node.id = entry["id"].GetString();
				node.name = entry["name"].GetString();
				node.type = entry["type"].GetString();
				node.x = entry["x"].GetInt();
				node.y = entry["y"].GetInt();
				node.cost = entry["cost"].GetInt();
				const std::string size = entry["size"].GetString();
				if ( node.id.empty() || !ids.insert(node.id).second
					|| node.x < 0 || node.x > 1000 || node.y < 0 || node.y > 1000
					|| (size != "small" && size != "large")
					|| !isSupportedNodeType(node.type) )
				{
					printlog("[Roguelite]: Invalid ID, position, or size in '%s'.", treeConfigPath);
					return false;
				}
				node.large = size == "large";
				if ( entry.HasMember("description") && entry["description"].IsString() )
				{
					node.description = entry["description"].GetString();
				}
				if ( entry.HasMember("icon") )
				{
					if ( !entry["icon"].IsString() )
					{
						printlog("[Roguelite]: Invalid icon for node '%s'.", node.id.c_str());
						return false;
					}
					node.icon = entry["icon"].GetString();
				}
				if ( entry.HasMember("requires") )
				{
					if ( !entry["requires"].IsArray() )
					{
						printlog("[Roguelite]: Invalid prerequisites for node '%s'.", node.id.c_str());
						return false;
					}
					for ( const auto& requirement : entry["requires"].GetArray() )
					{
						if ( !requirement.IsString() )
						{
							printlog("[Roguelite]: Invalid prerequisite for node '%s'.", node.id.c_str());
							return false;
						}
						node.prerequisites.emplace_back(requirement.GetString());
					}
				}
				for ( const auto& effectValue : entry["effects"].GetArray() )
				{
					if ( !effectValue.IsObject() || !effectValue.HasMember("stat")
						|| !effectValue["stat"].IsString() || !effectValue.HasMember("value")
						|| !effectValue["value"].IsInt() )
					{
						printlog("[Roguelite]: Invalid effect in node '%s'.", node.id.c_str());
						return false;
					}
					Effect effect;
					effect.stat = effectValue["stat"].GetString();
					effect.value = effectValue["value"].GetInt();
					if ( !isSupportedStat(effect.stat) || effect.value == 0
						|| effect.value < -1000 || effect.value > 1000 )
					{
						printlog("[Roguelite]: Unsupported or out-of-range effect in node '%s'.", node.id.c_str());
						return false;
					}
					node.effects.push_back(effect);
				}
				if ( node.effects.empty() )
				{
					printlog("[Roguelite]: Node '%s' has no effects.", node.id.c_str());
					return false;
				}
				parsedNodes.push_back(node);
			}

			if ( parsedNodes.empty() || parsedNodes.size() > maxSkillTreeNodes )
			{
				printlog("[Roguelite]: The skill tree must contain between 1 and %d nodes.",
					static_cast<int>(maxSkillTreeNodes));
				return false;
			}
			treeNodes.swap(parsedNodes);
			treeNodeIndices.clear();
			treeNodeIndices.reserve(treeNodes.size());
			for ( size_t i = 0; i < treeNodes.size(); ++i )
			{
				treeNodeIndices.emplace(treeNodes[i].id, i);
			}
			for ( const auto& node : treeNodes )
			{
				if ( !hasValidRequires(node) )
				{
					printlog("[Roguelite]: Node '%s' has an unknown or self prerequisite.", node.id.c_str());
					treeNodes.clear();
					treeNodeIndices.clear();
					return false;
				}
			}
			std::unordered_set<std::string> visiting;
			std::unordered_set<std::string> visited;
			for ( const auto& node : treeNodes )
			{
				if ( !visitForCycle(node.id, visiting, visited) )
				{
					printlog("[Roguelite]: Prerequisite cycle detected at node '%s'.", node.id.c_str());
					treeNodes.clear();
					treeNodeIndices.clear();
					return false;
				}
			}

			pointsPerScore = document["score_per_point"].GetInt();
			featureEnabled = document["enabled"].GetBool();
			treeHash = 2166136261u;
			hashBytes(reinterpret_cast<const char*>(&pointsPerScore), sizeof(pointsPerScore));
			for ( const auto& node : treeNodes )
			{
				hashString(node.id);
				hashString(node.name);
				hashString(node.type);
				hashString(node.icon);
				hashBytes(reinterpret_cast<const char*>(&node.x), sizeof(node.x));
				hashBytes(reinterpret_cast<const char*>(&node.y), sizeof(node.y));
				hashBytes(reinterpret_cast<const char*>(&node.cost), sizeof(node.cost));
				hashBytes(reinterpret_cast<const char*>(&node.large), sizeof(node.large));
				for ( const auto& requirement : node.prerequisites )
				{
					hashString(requirement);
				}
				for ( const auto& effect : node.effects )
				{
					hashString(effect.stat);
					hashBytes(reinterpret_cast<const char*>(&effect.value), sizeof(effect.value));
				}
			}
			return true;
		}

		bool loadProfiles()
		{
			char path[PATH_MAX] = "";
			completePath(path, profilePath, outputdir);
			File* file = FileIO::open(path, "rb");
			if ( !file )
			{
				profileLoaded = true;
				return true;
			}
			const size_t fileSize = file->size();
			if ( fileSize == 0 || fileSize > 1024 * 1024 )
			{
				printlog("[Roguelite]: Invalid profile file size in '%s'.", path);
				FileIO::close(file);
				return false;
			}
			std::vector<char> buffer(fileSize + 1, '\0');
			const size_t bytesRead = file->read(buffer.data(), sizeof(char), fileSize);
			FileIO::close(file);
			if ( bytesRead != fileSize )
			{
				printlog("[Roguelite]: Failed to read profile '%s'.", path);
				return false;
			}
			rapidjson::Document document;
			document.Parse(buffer.data());
			if ( document.HasParseError() || !document.IsObject()
				|| !document.HasMember("version") || !document["version"].IsInt()
				|| (document["version"].GetInt() != 1 && document["version"].GetInt() != 2)
				|| !document.HasMember("players") || !document["players"].IsArray() )
			{
				printlog("[Roguelite]: Invalid profile JSON in '%s'; progression was not loaded.", path);
				return false;
			}
			const int profileVersion = document["version"].GetInt();
			for ( const auto& entry : document["players"].GetArray() )
			{
				if ( !entry.IsObject() || !entry.HasMember("slot") || !entry["slot"].IsInt()
					|| (profileVersion == 1 && (!entry.HasMember("score") || !entry["score"].IsUint64()))
					|| !entry.HasMember("unlocked") || !entry["unlocked"].IsArray() )
				{
					printlog("[Roguelite]: Skipping malformed player profile in '%s'.", path);
					continue;
				}
				const int player = entry["slot"].GetInt();
				if ( player < 0 || player >= MAXPLAYERS )
				{
					continue;
				}
				Profile& profile = profiles[player];
				if ( profileVersion == 1 )
				{
					ScoreProfile& scoreProfile = scoreProfiles[player];
					scoreProfile.lifetimeScore = entry["score"].GetUint64();
					scoreProfile.earnedPoints = scoreProfile.lifetimeScore / legacyScorePerPoint;
					scoreProfile.scoreProgress = scoreProfile.lifetimeScore % legacyScorePerPoint;
				}
				for ( const auto& nodeId : entry["unlocked"].GetArray() )
				{
					if ( nodeId.IsString() && findNode(nodeId.GetString()) )
					{
						profile.unlocked.insert(nodeId.GetString());
					}
				}
			}
			profileLoaded = true;
			return true;
		}

		bool saveProfiles()
		{
			char path[PATH_MAX] = "";
			completePath(path, profilePath, outputdir);
			rapidjson::Document document;
			document.SetObject();
			auto& allocator = document.GetAllocator();
			document.AddMember("version", 2, allocator);
			rapidjson::Value playerArray(rapidjson::kArrayType);
			for ( int player = 0; player < MAXPLAYERS; ++player )
			{
				rapidjson::Value entry(rapidjson::kObjectType);
				entry.AddMember("slot", player, allocator);
				rapidjson::Value unlocked(rapidjson::kArrayType);
				std::vector<std::string> sorted(profiles[player].unlocked.begin(), profiles[player].unlocked.end());
				std::sort(sorted.begin(), sorted.end());
				for ( const auto& nodeId : sorted )
				{
					unlocked.PushBack(rapidjson::Value(nodeId.c_str(), allocator), allocator);
				}
				entry.AddMember("unlocked", unlocked, allocator);
				playerArray.PushBack(entry, allocator);
			}
			document.AddMember("players", playerArray, allocator);

			rapidjson::StringBuffer buffer;
			rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
			writer.SetIndent(' ', 2);
			document.Accept(writer);
			File* file = FileIO::open(path, "wb");
			if ( !file )
			{
				printlog("[Roguelite]: Failed to open profile '%s' for writing.", path);
				return false;
			}
			const size_t written = file->write(buffer.GetString(), sizeof(char), buffer.GetSize());
			FileIO::close(file);
			if ( written != buffer.GetSize() )
			{
				printlog("[Roguelite]: Failed to write complete profile '%s'.", path);
				return false;
			}
			return true;
		}

		bool saveScores()
		{
			char path[PATH_MAX] = "";
			completePath(path, scorePath, outputdir);
			rapidjson::Document document;
			document.SetObject();
			auto& allocator = document.GetAllocator();
			document.AddMember("version", 2, allocator);
			rapidjson::Value playerArray(rapidjson::kArrayType);
			for ( int player = 0; player < MAXPLAYERS; ++player )
			{
				rapidjson::Value entry(rapidjson::kObjectType);
				entry.AddMember("slot", player, allocator);
				entry.AddMember("lifetime_score", rapidjson::Value(scoreProfiles[player].lifetimeScore), allocator);
				entry.AddMember("earned_points", rapidjson::Value(scoreProfiles[player].earnedPoints), allocator);
				entry.AddMember("score_progress", rapidjson::Value(scoreProfiles[player].scoreProgress), allocator);
				playerArray.PushBack(entry, allocator);
			}
			document.AddMember("players", playerArray, allocator);

			rapidjson::StringBuffer buffer;
			rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
			writer.SetIndent(' ', 2);
			document.Accept(writer);
			File* file = FileIO::open(path, "wb");
			if ( !file )
			{
				printlog("[Roguelite]: Failed to open score file '%s' for writing.", path);
				return false;
			}
			const size_t written = file->write(buffer.GetString(), sizeof(char), buffer.GetSize());
			FileIO::close(file);
			if ( written != buffer.GetSize() )
			{
				printlog("[Roguelite]: Failed to write complete score file '%s'.", path);
				return false;
			}
			return true;
		}

		bool loadScores()
		{
			char path[PATH_MAX] = "";
			completePath(path, scorePath, outputdir);
			File* file = FileIO::open(path, "rb");
			if ( !file )
			{
				printlog("[Roguelite]: Initializing separate score data from the existing progression profile.");
				return saveScores();
			}
			const size_t fileSize = file->size();
			if ( fileSize == 0 || fileSize > 1024 * 1024 )
			{
				printlog("[Roguelite]: Invalid score file size in '%s'.", path);
				FileIO::close(file);
				return false;
			}
			std::vector<char> buffer(fileSize + 1, '\0');
			const size_t bytesRead = file->read(buffer.data(), sizeof(char), fileSize);
			FileIO::close(file);
			if ( bytesRead != fileSize )
			{
				printlog("[Roguelite]: Failed to read score file '%s'.", path);
				return false;
			}
			rapidjson::Document document;
			document.Parse(buffer.data());
			if ( document.HasParseError() || !document.IsObject()
				|| !document.HasMember("version") || !document["version"].IsInt()
				|| (document["version"].GetInt() != 1 && document["version"].GetInt() != 2)
				|| !document.HasMember("players") || !document["players"].IsArray() )
			{
				printlog("[Roguelite]: Invalid score JSON in '%s'; progression was not loaded.", path);
				return false;
			}
			const int scoreVersion = document["version"].GetInt();
			std::array<ScoreProfile, MAXPLAYERS> loadedProfiles{};
			std::array<bool, MAXPLAYERS> loadedSlots{};
			bool needsRewrite = scoreVersion == 1;
			for ( const auto& entry : document["players"].GetArray() )
			{
				if ( !entry.IsObject() || !entry.HasMember("slot") || !entry["slot"].IsInt()
					|| (scoreVersion == 1 && (!entry.HasMember("score") || !entry["score"].IsUint64()))
					|| (scoreVersion == 2 && (!entry.HasMember("lifetime_score") || !entry["lifetime_score"].IsUint64()
						|| !entry.HasMember("earned_points") || !entry["earned_points"].IsUint64()
						|| !entry.HasMember("score_progress") || !entry["score_progress"].IsUint64())) )
				{
					printlog("[Roguelite]: Invalid player entry in score file '%s'.", path);
					return false;
				}
				const int player = entry["slot"].GetInt();
				if ( player < 0 || player >= MAXPLAYERS || loadedSlots[player] )
				{
					printlog("[Roguelite]: Invalid or duplicate player slot in score file '%s'.", path);
					return false;
				}
				loadedSlots[player] = true;
				ScoreProfile& scoreProfile = loadedProfiles[player];
				if ( scoreVersion == 1 )
				{
					scoreProfile.lifetimeScore = entry["score"].GetUint64();
					scoreProfile.earnedPoints = scoreProfile.lifetimeScore / legacyScorePerPoint;
					scoreProfile.scoreProgress = scoreProfile.lifetimeScore % legacyScorePerPoint;
				}
				else
				{
					scoreProfile.lifetimeScore = entry["lifetime_score"].GetUint64();
					scoreProfile.earnedPoints = entry["earned_points"].GetUint64();
					scoreProfile.scoreProgress = entry["score_progress"].GetUint64();
				}
				const uint64_t additionalPoints = scoreProfile.scoreProgress / static_cast<uint64_t>(pointsPerScore);
				if ( additionalPoints > std::numeric_limits<uint64_t>::max() - scoreProfile.earnedPoints )
				{
					printlog("[Roguelite]: Score progression overflow in '%s'.", path);
					return false;
				}
				if ( additionalPoints > 0 )
				{
					scoreProfile.earnedPoints += additionalPoints;
					scoreProfile.scoreProgress %= static_cast<uint64_t>(pointsPerScore);
					needsRewrite = true;
				}
			}
			if ( std::find(loadedSlots.begin(), loadedSlots.end(), false) != loadedSlots.end() )
			{
				printlog("[Roguelite]: Score file '%s' is missing player data.", path);
				return false;
			}
			scoreProfiles = loadedProfiles;
			return !needsRewrite || saveScores();
		}

		bool ensureReady()
		{
			if ( !initialized )
			{
				initialized = true;
				configValid = parseConfig();
				if ( configValid )
				{
					profileLoaded = loadProfiles();
					if ( profileLoaded )
					{
						scoresLoaded = loadScores();
					}
				}
			}
			return configValid && profileLoaded && scoresLoaded;
		}

		int profileSlot(int player)
		{
			if ( multiplayer == CLIENT && player == clientnum )
			{
				return 0;
			}
			return player;
		}

		int spentPoints(const Profile& profile)
		{
			int spent = 0;
			for ( const auto& node : treeNodes )
			{
				if ( profile.unlocked.count(node.id) )
				{
					spent += node.cost;
				}
			}
			return spent;
		}

		bool validateLoadout(const std::vector<std::string>& nodeIds, uint64_t score,
			uint64_t earnedPoints, std::unordered_set<std::string>& unlocked)
		{
			unlocked.clear();
			if ( earnedPoints > score )
			{
				return false;
			}
			int spent = 0;
			const int points = static_cast<int>(std::min<uint64_t>(
				earnedPoints, std::numeric_limits<int>::max()));
			for ( const auto& nodeId : nodeIds )
			{
				const Node* node = findNode(nodeId);
				if ( !node || !unlocked.insert(nodeId).second )
				{
					return false;
				}
				spent += node->cost;
				if ( spent > points )
				{
					return false;
				}
			}
			for ( const auto& nodeId : unlocked )
			{
				const Node* node = findNode(nodeId);
				for ( const auto& requirement : node->prerequisites )
				{
					if ( !unlocked.count(requirement) )
					{
						return false;
					}
				}
			}
			return true;
		}

		void applyEffects(Stat& stat, const std::unordered_set<std::string>& unlocked)
		{
			int maxHpBonus = 0;
			int maxMpBonus = 0;
			int foodBonus = 0;
			for ( const auto& node : treeNodes )
			{
				if ( !unlocked.count(node.id) )
				{
					continue;
				}
				for ( const auto& effect : node.effects )
				{
					if ( effect.stat == "max_hp" )
					{
						maxHpBonus += effect.value;
					}
					else if ( effect.stat == "max_mp" )
					{
						maxMpBonus += effect.value;
					}
					else if ( effect.stat == "strength" )
					{
						stat.STR += effect.value;
					}
					else if ( effect.stat == "dexterity" )
					{
						stat.DEX += effect.value;
					}
					else if ( effect.stat == "constitution" )
					{
						stat.CON += effect.value;
					}
					else if ( effect.stat == "intelligence" )
					{
						stat.INT += effect.value;
					}
					else if ( effect.stat == "perception" )
					{
						stat.PER += effect.value;
					}
					else if ( effect.stat == "charisma" )
					{
						stat.CHR += effect.value;
					}
					else if ( effect.stat == "strength_skill_growth" )
					{
						stat.rogueliteSkillGrowthPercent[STAT_STR] += effect.value;
					}
					else if ( effect.stat == "dexterity_skill_growth" )
					{
						stat.rogueliteSkillGrowthPercent[STAT_DEX] += effect.value;
					}
					else if ( effect.stat == "constitution_skill_growth" )
					{
						stat.rogueliteSkillGrowthPercent[STAT_CON] += effect.value;
					}
					else if ( effect.stat == "intelligence_skill_growth" )
					{
						stat.rogueliteSkillGrowthPercent[STAT_INT] += effect.value;
					}
					else if ( effect.stat == "perception_skill_growth" )
					{
						stat.rogueliteSkillGrowthPercent[STAT_PER] += effect.value;
					}
					else if ( effect.stat == "charisma_skill_growth" )
					{
						stat.rogueliteSkillGrowthPercent[STAT_CHR] += effect.value;
					}
					else if ( effect.stat == "xp_growth" )
					{
						stat.rogueliteXpGrowthPercent += effect.value;
					}
					else if ( effect.stat == "gold" )
					{
						stat.GOLD += effect.value;
					}
					else if ( effect.stat == "magic_resistance" )
					{
						stat.rogueliteMagicResistancePercent += effect.value;
					}
					else if ( effect.stat == "burning_resistance" )
					{
						stat.rogueliteBurningResistancePercent += effect.value;
					}
					else if ( effect.stat == "poison_resistance" )
					{
						stat.roguelitePoisonResistancePercent += effect.value;
					}
					else if ( effect.stat == "weight_reduction" )
					{
						stat.rogueliteWeightReductionPercent += effect.value;
					}
					else if ( effect.stat == "food_meat" )
					{
						foodBonus += effect.value;
					}
				}
			}
			stat.STR = std::max(-128, std::min(127, stat.STR));
			stat.DEX = std::max(-128, std::min(127, stat.DEX));
			stat.CON = std::max(-128, std::min(127, stat.CON));
			stat.INT = std::max(-128, std::min(127, stat.INT));
			stat.PER = std::max(-128, std::min(127, stat.PER));
			stat.CHR = std::max(-128, std::min(127, stat.CHR));
			for ( int i = 0; i < NUMSTATS; ++i )
			{
				stat.rogueliteSkillGrowthPercent[i] = std::max(0, std::min(100, stat.rogueliteSkillGrowthPercent[i]));
			}
			stat.MAXHP = std::max(1, std::min(32767, stat.MAXHP + maxHpBonus));
			stat.HP = std::max(1, std::min(stat.MAXHP, stat.HP + maxHpBonus));
			stat.MAXMP = std::max(1, std::min(32767, stat.MAXMP + maxMpBonus));
			stat.MP = std::max(0, std::min(stat.MAXMP, stat.MP + maxMpBonus));
			stat.GOLD = std::max(0, stat.GOLD);
			stat.rogueliteXpGrowthPercent = std::min(1000, stat.rogueliteXpGrowthPercent);
			stat.rogueliteMagicResistancePercent = std::min(100, stat.rogueliteMagicResistancePercent);
			stat.rogueliteBurningResistancePercent = std::min(100, stat.rogueliteBurningResistancePercent);
			stat.roguelitePoisonResistancePercent = std::min(100, stat.roguelitePoisonResistancePercent);
			stat.rogueliteWeightReductionPercent = std::min(100, stat.rogueliteWeightReductionPercent);
			if ( foodBonus > 0 )
			{
				newItem(FOOD_MEAT, EXCELLENT, 0,
					static_cast<Sint16>(std::min(99, foodBonus)), 0, true, &stat.inventory);
			}
		}
	}

	bool initialize()
	{
		return ensureReady();
	}

	bool enabled()
	{
		return ensureReady() && featureEnabled;
	}

	int scorePerPoint()
	{
		return ensureReady() ? pointsPerScore : 0;
	}

	uint32_t configHash()
	{
		return ensureReady() ? treeHash : 0;
	}

	const std::vector<Node>& nodes()
	{
		ensureReady();
		return treeNodes;
	}

	const Node* findNode(const std::string& id)
	{
		const auto nodeIndex = treeNodeIndices.find(id);
		return nodeIndex == treeNodeIndices.end() ? nullptr : &treeNodes[nodeIndex->second];
	}

	bool isUnlocked(int player, const std::string& nodeId)
	{
		if ( !ensureReady() || player < 0 || player >= MAXPLAYERS )
		{
			return false;
		}
		return profiles[profileSlot(player)].unlocked.count(nodeId) != 0;
	}

	int availablePoints(int player)
	{
		if ( !ensureReady() || player < 0 || player >= MAXPLAYERS )
		{
			return 0;
		}
		const int slot = profileSlot(player);
		const Profile& profile = profiles[slot];
		const uint64_t earned = scoreProfiles[slot].earnedPoints;
		const int earnedPoints = static_cast<int>(std::min<uint64_t>(earned, std::numeric_limits<int>::max()));
		return std::max(0, earnedPoints - spentPoints(profile));
	}

	uint64_t lifetimeScore(int player)
	{
		if ( !ensureReady() || player < 0 || player >= MAXPLAYERS )
		{
			return 0;
		}
		return scoreProfiles[profileSlot(player)].lifetimeScore;
	}

	uint64_t scoreToNextPoint(int player)
	{
		if ( !ensureReady() || player < 0 || player >= MAXPLAYERS )
		{
			return 0;
		}
		const uint64_t progress = scoreProfiles[profileSlot(player)].scoreProgress;
		return static_cast<uint64_t>(pointsPerScore) - progress;
	}

	uint64_t earnedPoints(int player)
	{
		if ( !ensureReady() || player < 0 || player >= MAXPLAYERS )
		{
			return 0;
		}
		return scoreProfiles[profileSlot(player)].earnedPoints;
	}

	bool canPurchase(int player, const std::string& nodeId)
	{
		if ( !enabled() || player < 0 || player >= MAXPLAYERS )
		{
			return false;
		}
		player = profileSlot(player);
		const Node* node = findNode(nodeId);
		if ( !node || profiles[player].unlocked.count(nodeId) || availablePoints(player) < node->cost )
		{
			return false;
		}
		for ( const auto& requirement : node->prerequisites )
		{
			if ( !profiles[player].unlocked.count(requirement) )
			{
				return false;
			}
		}
		return true;
	}

	bool purchase(int player, const std::string& nodeId)
	{
		if ( !canPurchase(player, nodeId) )
		{
			return false;
		}
		player = profileSlot(player);
		profiles[player].unlocked.insert(nodeId);
		if ( !saveProfiles() )
		{
			profiles[player].unlocked.erase(nodeId);
			return false;
		}
		return true;
	}

	bool awardRunScore(int player, int score)
	{
		if ( !enabled() || player < 0 || player >= MAXPLAYERS || score < 0 )
		{
			return false;
		}
		player = profileSlot(player);
		if ( rewardGrantedThisRun[player] )
		{
			return false;
		}
		ScoreProfile& scoreProfile = scoreProfiles[player];
		const uint64_t scoreIncrement = static_cast<uint64_t>(score);
		if ( scoreIncrement > std::numeric_limits<uint64_t>::max() - scoreProfile.lifetimeScore
			|| scoreIncrement > std::numeric_limits<uint64_t>::max() - scoreProfile.scoreProgress )
		{
			printlog("[Roguelite]: Score total overflow for player %d.", player);
			return false;
		}
		const uint64_t oldPoints = scoreProfile.earnedPoints;
		const uint64_t oldScoreProgress = scoreProfile.scoreProgress;
		scoreProfile.lifetimeScore += scoreIncrement;
		scoreProfile.scoreProgress += scoreIncrement;
		const uint64_t pointsEarned = scoreProfile.scoreProgress / static_cast<uint64_t>(pointsPerScore);
		if ( pointsEarned > std::numeric_limits<uint64_t>::max() - scoreProfile.earnedPoints )
		{
			scoreProfile.lifetimeScore -= scoreIncrement;
			scoreProfile.scoreProgress = oldScoreProgress;
			printlog("[Roguelite]: Earned-point total overflow for player %d.", player);
			return false;
		}
		scoreProfile.earnedPoints += pointsEarned;
		scoreProfile.scoreProgress %= static_cast<uint64_t>(pointsPerScore);
		if ( !saveScores() )
		{
			scoreProfile.lifetimeScore -= scoreIncrement;
			scoreProfile.earnedPoints = oldPoints;
			scoreProfile.scoreProgress = oldScoreProgress;
			return false;
		}
		rewardGrantedThisRun[player] = true;
		printlog("[Roguelite]: Player %d earned %llu score and %llu progression point(s).",
			player, static_cast<unsigned long long>(score),
			static_cast<unsigned long long>(pointsEarned));
		return true;
	}

	void beginRun()
	{
		rewardGrantedThisRun.fill(false);
		bonusesApplied.fill(false);
	}

	bool applyPlayerBonuses(int player)
	{
		if ( !enabled() || player < 0 || player >= MAXPLAYERS || !stats[player] || bonusesApplied[player] )
		{
			return false;
		}
		applyEffects(*stats[player], profiles[profileSlot(player)].unlocked);
		bonusesApplied[player] = true;
		return true;
	}

	bool applyRemoteLoadout(int player, uint64_t claimedLifetimeScore, uint64_t claimedEarnedPoints,
		const std::vector<std::string>& nodeIds)
	{
		if ( !enabled() || player < 0 || player >= MAXPLAYERS || !stats[player] )
		{
			return false;
		}
		if ( bonusesApplied[player] )
		{
			return true;
		}
		std::unordered_set<std::string> unlocked;
		if ( !validateLoadout(nodeIds, claimedLifetimeScore, claimedEarnedPoints, unlocked) )
		{
			printlog("[Roguelite]: Rejected invalid loadout for player %d.", player);
			return false;
		}
		applyEffects(*stats[player], unlocked);
		bonusesApplied[player] = true;
		return true;
	}

	std::vector<Effect> appliedEffects(int player)
	{
		std::vector<Effect> result;
		if ( !ensureReady() || player < 0 || player >= MAXPLAYERS )
		{
			return result;
		}
		const Profile& profile = profiles[profileSlot(player)];
		for ( const auto& node : treeNodes )
		{
			if ( !profile.unlocked.count(node.id) )
			{
				continue;
			}
			for ( const auto& effect : node.effects )
			{
				const auto existing = std::find_if(result.begin(), result.end(), [&](const Effect& applied) {
					return applied.stat == effect.stat;
				});
				if ( existing == result.end() )
				{
					result.push_back(effect);
				}
				else
				{
					existing->value += effect.value;
				}
			}
		}
		return result;
	}

	std::vector<std::string> unlockedNodes(int player)
	{
		std::vector<std::string> result;
		if ( !ensureReady() || player < 0 || player >= MAXPLAYERS )
		{
			return result;
		}
		const Profile& profile = profiles[profileSlot(player)];
		for ( const auto& node : treeNodes )
		{
			if ( profile.unlocked.count(node.id) )
			{
				result.push_back(node.id);
			}
		}
		return result;
	}
}
