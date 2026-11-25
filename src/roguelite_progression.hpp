#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Roguelite
{
	constexpr std::size_t maxSkillTreeNodes = 2048;

	struct Effect
	{
		std::string stat;
		int value = 0;
	};

	struct Node
	{
		std::string id;
		std::string name;
		std::string description;
		std::string type;
		std::string icon;
		int x = 0;
		int y = 0;
		int cost = 1;
		bool large = false;
		std::vector<std::string> prerequisites;
		std::vector<Effect> effects;
	};

	bool initialize();
	bool enabled();
	int scorePerPoint();
	uint32_t configHash();
	const std::vector<Node>& nodes();
	const Node* findNode(const std::string& id);
	bool isUnlocked(int player, const std::string& nodeId);
	int availablePoints(int player);
	uint64_t lifetimeScore(int player);
	uint64_t scoreToNextPoint(int player);
	uint64_t earnedPoints(int player);
	bool canPurchase(int player, const std::string& nodeId);
	bool purchase(int player, const std::string& nodeId);
	bool awardRunScore(int player, int score);
	void beginRun();
	bool applyPlayerBonuses(int player);
	bool applyRemoteLoadout(int player, uint64_t claimedLifetimeScore, uint64_t claimedEarnedPoints,
		const std::vector<std::string>& nodeIds);
	std::vector<Effect> appliedEffects(int player);
	std::vector<std::string> unlockedNodes(int player);
}
