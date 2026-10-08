#pragma once

// Volume of one hub NPC's own sounds (e.g. the blacksmith's hammering), applied on the game thread to the audio
// components of NPC actors whose class name contains `who`; volume 0 also stops the NPC's playing montage.
// Each change and each map load applies once; what the NPC plays (anim instance, montage) is logged, to find
// where a sound comes from when it is not a component (`anim` is reserved for that).
namespace npc_audio {
    void Set(const char* who, const char* anim, float volume);  // render thread; nullptr who = off (volume 1 restored)
    void Tick();                                                 // render thread, every frame while enabled
}
