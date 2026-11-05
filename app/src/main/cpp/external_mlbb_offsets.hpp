#pragma once
#include <cstddef>
#include <cstdint>

/** Offsets from EXTERNAL-MLBB (2).rar — libcsharp/libil2cpp static RVAs. */
namespace ExtOff {

namespace BattleManager {
constexpr size_t BasePtr = 0x635b290;
constexpr size_t LocalPlayerShow = 0x48;
constexpr size_t ShowPlayers = 0x70;
constexpr size_t ShowMonsters = 0x78;
constexpr size_t PtrChain1 = 0xa8;
constexpr size_t ListDataOffset = 0x10;
constexpr size_t ListCountOffset = 0x18;
constexpr size_t ListArrayOffset = 0x20;
} // namespace BattleManager

namespace ShowEntity {
constexpr size_t Hp = 0x1a4;
constexpr size_t HpMax = 0x1a8;
constexpr size_t bDeath = 0xc5;
constexpr size_t bSameCampType = 0x2a9;
constexpr size_t vCachePosition = 0x28c;
constexpr size_t HeroID = 0x18c;
} // namespace ShowEntity

namespace Camera {
constexpr size_t MainCameraPtr = 0x635b620;
constexpr size_t CameraData = 0xa8;
constexpr size_t CameraData2 = 0x08;
constexpr size_t SmoothFollow = 0x10;
constexpr size_t ViewMatrix = 0x5c;
} // namespace Camera

} // namespace ExtOff
