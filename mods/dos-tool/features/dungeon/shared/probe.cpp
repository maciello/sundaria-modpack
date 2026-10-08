#include "probe.hpp"
#include "event.hpp"
#include "ref.hpp"
#include "tmap.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"
#include "BP_GameState_classes.hpp"
#include "BP_Dungeon_classes.hpp"
#include "BP_DungeonFloor_classes.hpp"
#include "BP_BreadSlice_classes.hpp"
#include "BP_BreadCrumb_classes.hpp"
#include "BP_TriggerBase_classes.hpp"
#include "BP_Door_classes.hpp"
#include "BP_DungeonExitVolume_classes.hpp"
#include "BP_ChildActorDungeonTriggerBase_classes.hpp"
#include "BP_StaticMinimapObjectComponent_classes.hpp"
#include "DungeonChunkComponentStatic_classes.hpp"
#include "WidgetMinimap_classes.hpp"
#include "NavigationSystem_classes.hpp"
#include "NavigationSystem_parameters.hpp"

// Dev probe (dungeon-map.probe file trigger, game thread). Reaches everything through owners: world → game state →
// DungeonActor → floors → chunk actors (slices) → crumbs / trigger child actors. No GObjects walk.
using namespace SDK;
using umg::PtrOk;

namespace {
    std::string F(const char* fmt, ...) {
        char b[768];
        va_list a;
        va_start(a, fmt);
        std::vsnprintf(b, sizeof b, fmt, a);
        va_end(a);
        return b;
    }
    std::string Cls(const UObject* o) { return PtrOk(o) && PtrOk(o->Class) ? o->Class->GetName() : "-"; }
    std::string Name(const UObject* o) { return PtrOk(o) ? o->GetName() : "-"; }
    std::string V(const FVector& v) { return F("[%.0f, %.0f, %.0f]", v.X, v.Y, v.Z); }
    std::string At(AActor* a) { return PtrOk(a) ? V(a->K2_GetActorLocation()) : "-"; }
    std::string Names(const TArray<FName>& a) {
        std::string s = "[";
        for (int i = 0; i < a.Num(); i++) s += (i ? ", " : "") + a[i].ToString();
        return s + "]";
    }
    std::string Rules(const TArray<FSLockByTypes>& a) {
        std::string s = "[";
        for (int i = 0; i < a.Num(); i++)
            s += F("%s{type: %d, value: %d, tag: %s}", i ? ", " : "", int(a[i].LockType_14_82F9F5F8476ED7CC39F082803E772AFC),
                   a[i].Value_15_10BCADD84AFD09A3D190E1B885A7D0D5, a[i].TargetTag_51_23F84A6B427EA5121104F4ABCDEAB3F3.ToString().c_str());
        return s + "]";
    }

    std::string Trigger(AActor* a) {
        if (!PtrOk(a) || !a->IsA(ABP_TriggerBase_C::StaticClass())) return F("{cls: %s, at: %s}", Cls(a).c_str(), At(a).c_str());
        auto* t = static_cast<ABP_TriggerBase_C*>(a);
        std::string s = F("{cls: %s, name: %s, at: %s, actorTags: %s, anim: %d, lock: %d, canOpen: %d, canClose: %d, startOpen: %d, lockOnce: %d, autoOpenOnUnlock: %d, "
                          "lockTags: %s, rules: %s, itemLockGroup: %s",
                          Cls(a).c_str(), Name(a).c_str(), At(a).c_str(), Names(a->Tags).c_str(), int(t->mOpenCloseAnimState), int(t->LockStatus), t->CanBeOpened, t->CanBeClosed,
                          t->bStartOpen, t->LockOnlyOnce, t->AutoOpenOnUnlock, Names(t->LockTags).c_str(), Rules(t->LockRules_OR).c_str(),
                          Names(t->ItemLockGroup).c_str());
        if (a->IsA(ABP_Door_C::StaticClass())) s += ", doorLockTags: " + Names(static_cast<ABP_Door_C*>(a)->LockTags_0);
        return s + "}";
    }

    std::string Slice(Abp_breadslice_C* s, const FVector& pawn) {
        std::string o = F("  - {cls: %s, name: %s, at: %s, discovered: %d, needsDiscovery: %d", Cls(s).c_str(), Name(s).c_str(), At(s).c_str(),
                          s->Discovered, s->NeedsToBeDiscovered);
        if (UBoxComponent* b = s->DiscoveryBounds; PtrOk(b)) {
            const FVector c = b->K2_GetComponentLocation(), sc = b->K2_GetComponentScale(), e = b->BoxExtent;
            const FVector ext{e.X * sc.X, e.Y * sc.Y, e.Z * sc.Z};
            const bool in = std::abs(pawn.X - c.X) <= ext.X && std::abs(pawn.Y - c.Y) <= ext.Y && std::abs(pawn.Z - c.Z) <= ext.Z;
            o += F(", box: {c: %s, ext: %s, yaw: %.0f}%s", V(c).c_str(), V(ext).c_str(), b->K2_GetComponentRotation().Yaw, in ? ", PAWN_INSIDE: 1" : "");
        }
        const FSDungeonSliceMapData& m = s->DungeonSliceMapData;
        o += F(", mapData: {origin: %s, extend: %s, offset: [%.1f, %.1f], scale: %.2f}", V(m.BoundOrigin_16_17215E37461A5E520F514A9EA75F3D60).c_str(),
               V(m.BoundExtend_17_6D7488934E590FB5B3CF53AD59373061).c_str(), m.OffsetAdjustment_5_B9D9FC7E434C44BCA9F31F99CA4A2C92.X,
               m.OffsetAdjustment_5_B9D9FC7E434C44BCA9F31F99CA4A2C92.Y, m.ScaleAdjustment_11_DFA355714D3C57D3B06193A2C4CD7382);
        AActor* ac = s->AttachedCrumb;
        o += F(", attachedCrumb: {cls: %s, name: %s, at: %s, owner: %s}}\n", Cls(ac).c_str(), Name(ac).c_str(), At(ac).c_str(),
               PtrOk(ac) ? Name(ac->Owner).c_str() : "-");
        for (int i = 0; i < s->InitialCrumbs.Num(); i++) {
            AActor* c = s->InitialCrumbs[i];
            if (!PtrOk(c) || !c->IsA(Abp_breadcrumb_C::StaticClass())) continue;
            auto* b = static_cast<Abp_breadcrumb_C*>(c);
            o += F("      crumb: {cls: %s, name: %s, at: %s, type: %d, attachedTo: %d, spawned: %s %s, owner: %s}\n", Cls(c).c_str(), Name(c).c_str(),
                   At(c).c_str(), int(b->CrumbType), int(b->AttachedToCrumbType), Cls(b->SpawnedActor).c_str(), At(b->SpawnedActor).c_str(),
                   Name(c->Owner).c_str());
        }
        for (int i = 0; i < s->TriggerBaseComponents.Num(); i++) {
            UChildActorComponent* c = s->TriggerBaseComponents[i];
            if (!PtrOk(c)) continue;
            o += "      trigger: " + Trigger(c->ChildActor);
            if (c->IsA(UBP_ChildActorDungeonTriggerBase_C::StaticClass())) {
                const FSLockConfig& l = static_cast<UBP_ChildActorDungeonTriggerBase_C*>(c)->ConfigLock;
                o += F(" config: {rules: %s, lootKeys: %s, guarded: %d}", Rules(l.LockRules_6_7CB3782F4D092E9DA3BD819CAB768395).c_str(),
                       Names(l.LootKeys_15_E8C3CC644AE9A744789665A3623FE4EF).c_str(), int(l.GuardedLockType_18_44D7775444C7FA69E2E5E0B924BA1C09));
            }
            o += "\n";
        }
        return o;
    }

    std::string Widget(UWidget* w) {
        if (!PtrOk(w)) return "-";
        const FWidgetTransform& t = w->RenderTransform;
        std::string s = F("{t: [%.1f, %.1f], scale: [%.2f, %.2f], angle: %.1f", t.Translation.X, t.Translation.Y, t.Scale.X, t.Scale.Y, t.Angle);
        if (PtrOk(w->Slot) && w->Slot->IsA(UCanvasPanelSlot::StaticClass())) {
            const FAnchorData& d = static_cast<UCanvasPanelSlot*>(w->Slot)->LayoutData;
            s += F(", offsets: [%.1f, %.1f, %.1f, %.1f]", d.Offsets.Left, d.Offsets.Top, d.Offsets.Right, d.Offsets.Bottom);
        }
        return s + "}";
    }

    std::string Minimap(UWidgetMiniMap_C* m) {
        if (!PtrOk(m)) return "minimap: none (no WidgetMiniMap_C Tick seen yet)\n";
        std::string o = F("minimap: {unitToPixel: %d, alignToPlayer: %d, revealSpeed: %.2f, partyPawns: %d}\n", m->UnitToPixel, m->bAlignMapToPlayer,
                          m->HiddenPieceRevealSpeed, m->PartyPawns.Num());
        o += "  map: " + Widget(m->CanvasPanel_Map) + "\n  static: " + Widget(m->CanvasPanel_StaticMinimap) + "\n  dynamic: " +
             Widget(m->CanvasPanel_DynamicMinimp) + "\n  retainer: " + Widget(m->RetainerBox_Minimap) + "\n  pawnIcon: " + Widget(m->Image_Pawn_Owner) +
             "\n  view: " + Widget(m->Image_View) + "\n  staticPieces:\n";
        tmap::ForEach(m->StaticMinimapObjects, [&](UObject* k, UImage* img) {
            std::string origin = "-";
            if (PtrOk(k) && k->IsA(UBP_StaticMinimapObjectComponent_C::StaticClass())) {
                auto* c = static_cast<UBP_StaticMinimapObjectComponent_C*>(k);
                origin = V(c->MinimapOrigin) + " owner " + Cls(c->GetOwner()) + " " + At(c->GetOwner());
            }
            o += F("    - {key: %s, origin: %s, image: %s}\n", Cls(k).c_str(), origin.c_str(), Widget(img).c_str());
        });
        o += "  dynamicObjects:\n";
        tmap::ForEach(m->DynamicMinimapObjects, [&](UObject* k, UImage* img) {
            AActor* owner = PtrOk(k) && k->IsA(UActorComponent::StaticClass()) ? static_cast<UActorComponent*>(k)->GetOwner() : nullptr;
            o += F("    - {key: %s, owner: %s %s, image: %s}\n", Cls(k).c_str(), Cls(owner).c_str(), At(owner).c_str(), Widget(img).c_str());
        });
        return o;
    }

    // Navmesh path pawn -> goal (game's own synchronous query): point count, length, cost.
    std::string Nav(APawn* pawn, const FVector& goal) {
        UFunction* fn = UNavigationSystemV1::StaticClass()->GetFunction("NavigationSystemV1", "FindPathToLocationSynchronously");
        if (!PtrOk(fn) || !PtrOk(pawn)) return "nav: no function/pawn\n";
        Params::NavigationSystemV1_FindPathToLocationSynchronously q{};
        q.WorldContextObject = pawn;
        q.PathStart = pawn->K2_GetActorLocation();
        q.PathEnd = goal;
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        umg::CallNative(UNavigationSystemV1::GetDefaultObj(), fn, &q);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        UNavigationPath* np = q.ReturnValue;
        if (!PtrOk(np)) return F("nav: {to: %s, path: none, ms: %.2f}\n", V(goal).c_str(), ms);
        float len = 0;
        std::string pts;
        for (int i = 0; i < np->PathPoints.Num(); i++) {
            if (i) len += std::hypot(np->PathPoints[i].X - np->PathPoints[i - 1].X, np->PathPoints[i].Y - np->PathPoints[i - 1].Y);
            pts += (i ? ", " : "") + V(np->PathPoints[i]);
        }
        return F("nav: {to: %s, points: %d, length: %.0f, ms: %.2f, path: [%s]}\n", V(goal).c_str(), np->PathPoints.Num(), len, ms, pts.c_str());
    }

    struct Watch {
        dungeon_map::Event discovered{Abp_breadslice_C::StaticName, L"BecomeDiscovered"};
        dungeon_map::Event floorOn{ABP_DungeonFloor_C::StaticName, L"I_SetFloorActivated"};
        dungeon_map::Event state{ABP_TriggerBase_C::StaticName, L"OnTriggerStateChanged"};
        dungeon_map::Event lockRep{ABP_TriggerBase_C::StaticName, L"OnRep_LockStatus"};
        dungeon_map::Event activate{ABP_TriggerBase_C::StaticName, L"I_ActivateTrigger_Server"};
        dungeon_map::Event unlock{ABP_TriggerBase_C::StaticName, L"I_UnlockByRule"};
        void Warm() { discovered.Warm(), floorOn.Warm(), state.Warm(), lockRep.Warm(), activate.Warm(), unlock.Warm(); }
    } g_w;
}

namespace dungeon_map::probe {
    std::string Report(void* minimap) {
        std::string o;
        APlayerController* pc = umg::LocalPC();
        APawn* pawn = pc ? pc->Pawn : nullptr;
        const FVector p = PtrOk(pawn) ? pawn->K2_GetActorLocation() : FVector{};
        o += F("pawn: {at: %s, yaw: %.0f}\n", V(p).c_str(), PtrOk(pawn) ? pawn->K2_GetActorRotation().Yaw : 0.f);
        o += Minimap(static_cast<UWidgetMiniMap_C*>(minimap));
        UWorld* w = UWorld::GetWorld();
        AGameStateBase* gs = PtrOk(w) ? w->GameState : nullptr;
        if (!PtrOk(gs) || !gs->IsA(ABP_GameState_C::StaticClass())) return o + "gameState: " + Cls(gs) + " (not BP_GameState_C)\n";
        AActor* da = static_cast<ABP_GameState_C*>(gs)->DungeonActor;
        o += "gameState: " + Cls(gs) + ", dungeonActor: " + Cls(da) + "\n";
        if (!PtrOk(da) || !da->IsA(ABP_Dungeon_C::StaticClass())) return o;
        auto* d = static_cast<ABP_Dungeon_C*>(da);
        o += F("dungeon: {currentFloor: %d, floors: %d, playerFloors: %d, mapDataBound: %d, displayingMapFloor: %d}\n", d->CurrentActiveFloor,
               d->FloorActors.Num(), d->PlayerFloors.Num(), d->MapDataBound.Num(), d->DisplayingMAPDataOnFloor);
        for (int i = 0; i < d->PlayerFloors.Num(); i++)
            o += F("  playerFloor: {player: %s, floor: %d}\n", Name(d->PlayerFloors[i].Player_4_C2DC159B4A7E4469EC8C77A7786CDECF).c_str(),
                   d->PlayerFloors[i].Floor_5_8932747C453474903A3D86B97700EC81);
        for (int i = 0; i < d->MapDataBound.Num() && i < 80; i++) {
            const FSDungeonBoundDataMinimap& b = d->MapDataBound[i];
            o += F("  bound: {origin: %s, extend: %s, rot: %.0f, chunk: %s}\n", V(b.Origin_2_1DC09D964F495E240B55409FAB8DF6F0).c_str(),
                   V(b.Extend_4_9C4C3E5042CEA17113849A80DE445F9E).c_str(), b.Rotation_7_080473DD47819DCEFB0476BB526E086F,
                   Name(b.DungeonChunk_24_31EA35A14046AE6EDFFA579BC890B668).c_str());
        }
        for (int i = 0; i < d->FloorActors.Num(); i++) {
            AActor* fa = d->FloorActors[i];
            if (!PtrOk(fa) || !fa->IsA(ABP_DungeonFloor_C::StaticClass())) { o += "floor: " + Cls(fa) + "\n"; continue; }
            auto* f = static_cast<ABP_DungeonFloor_C*>(fa);
            o += F("floor: {i: %d, name: %s, number: %d, activated: %d, chunks: %d, spawned: %d, entry: %s, minimapOrigin: %s}\n", i, Name(f).c_str(),
                   f->FloorNumber, f->bHasBeenActivated, f->ChunkActors.Num(), f->ChunkSpawnedActors.Num(),
                   PtrOk(f->Entry) ? Name(f->Entry->ChildActor).c_str() : "-",
                   PtrOk(f->BP_MinimapObject) ? V(f->BP_MinimapObject->MinimapOrigin).c_str() : "-");
            for (int j = 0; j < f->ChunkActors.Num(); j++) {
                AActor* c = f->ChunkActors[j];
                if (PtrOk(c) && c->IsA(Abp_breadslice_C::StaticClass())) o += Slice(static_cast<Abp_breadslice_C*>(c), p);
                else o += F("  - {cls: %s, at: %s}\n", Cls(c).c_str(), At(c).c_str());
            }
            for (int j = 0; j < f->ChunkActors.Num(); j++)  // the floor's stairs-down room: navmesh path test
                if (AActor* c = f->ChunkActors[j]; PtrOk(c) && Cls(c).find("Stairs_Down") != std::string::npos) o += "  " + Nav(pawn, c->K2_GetActorLocation());
            for (int j = 0; j < f->ChunkSpawnedActors.Num(); j++) {
                AActor* a = f->ChunkSpawnedActors[j];
                if (PtrOk(a) && (a->IsA(ABP_TriggerBase_C::StaticClass()) || a->IsA(ABP_DungeonExitVolume_C::StaticClass())))
                    o += "  spawned: " + Trigger(a) + "\n";
            }
        }
        return o;
    }

    void Warm() { g_w.Warm(); }

    std::string Event(void* obj, void* fn) {
        auto* o = static_cast<UObject*>(obj);
        if (g_w.discovered.Is(fn) || g_w.floorOn.Is(fn) || g_w.lockRep.Is(fn) || g_w.activate.Is(fn) || g_w.unlock.Is(fn) || g_w.state.Is(fn)) {
            const std::string what = static_cast<UFunction*>(fn)->GetName();
            AActor* a = PtrOk(o) && o->IsA(AActor::StaticClass()) ? static_cast<AActor*>(o) : nullptr;
            return "[dungeon-map] event " + what + " on " + (a ? Trigger(a) : Cls(o));
        }
        return "";
    }
}
