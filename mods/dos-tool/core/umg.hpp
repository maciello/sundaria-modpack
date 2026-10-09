#pragma once
// Game-thread helpers for features that put game widgets into game screens (references/game-ui.md).
// Call only inside a game-event callback on the game thread (game::On / OnWorldTick, core/game.hpp).
#include <cstdint>
#include <string>

namespace SDK { class UObject; class UFunction; class UClass; class APlayerController; class FText; }

namespace umg {
    bool PtrOk(const void* p);
    // Native UFunctions whose bodies are not compiled in: same call shape as Dumper-7's native bodies.
    void CallNative(const SDK::UObject* obj, SDK::UFunction* fn, void* parms);
    // Live screen widget, not a class default or designer template (never modify those).
    bool Live(const SDK::UObject* o);
    SDK::APlayerController* LocalPC();
    // fn is a player controller's ReceiveTick: the world tick, outside Slate paint/layout and widget Construct.
    // The only point where a listener may add or remove widgets (AddChild/RemoveChild); elsewhere it crashes the game.
    bool IsWorldTick(const void* fn);
    // UTF-8 -> FText. ponytail: each call leaks one FText reference (a few bytes); call on change only.
    SDK::FText Text(const std::string& s);
    // New plain widget (UImage, UTextBlock …⊇) owned by a user widget's tree; add it to a panel afterwards.
    SDK::UObject* Spawn(SDK::UClass* cls, SDK::UObject* outer);
}
