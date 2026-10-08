#include "umg.hpp"
#include "ref.hpp"

#include <Windows.h>
#include "Engine_classes.hpp"
#include "Engine_parameters.hpp"
#include "BP_PlayerControllerGame_classes.hpp"

using namespace SDK;

namespace umg {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    void CallNative(const UObject* obj, UFunction* fn, void* parms) {
        if (!fn) return;  // a ref::Fn whose class is not loaded (yet)
        auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }
    bool Live(const UObject* o) { return PtrOk(o) && !(int(o->Flags) & 0x30); }

    APlayerController* LocalPC() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() < 1) return nullptr;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        return PtrOk(lp) && PtrOk(lp->PlayerController) ? lp->PlayerController : nullptr;
    }

    bool IsWorldTick(const void* fn) {
        static ref::Fn online{ABP_PlayerControllerOnline_C::StaticClass, "BP_PlayerControllerOnline_C", "ReceiveTick"};
        static ref::Fn game{ABP_PlayerControllerGame_C::StaticClass, "BP_PlayerControllerGame_C", "ReceiveTick"};
        return online.Is(fn) || game.Is(fn);
    }

    FText Text(const std::string& s) {
        static ref::Fn conv{UKismetTextLibrary::StaticClass, "KismetTextLibrary", "Conv_StringToText"};
        UFunction* fn = conv.Get();
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
        Params::KismetTextLibrary_Conv_StringToText t{};
        t.inString = FString(w.c_str());
        if (fn) CallNative(UKismetTextLibrary::GetDefaultObj(), fn, &t);
        return t.ReturnValue;
    }

    UObject* Spawn(UClass* cls, UObject* outer) {
        static ref::Fn spawn{UGameplayStatics::StaticClass, "GameplayStatics", "SpawnObject"};
        UFunction* fn = spawn.Get();
        if (!fn || !PtrOk(outer)) return nullptr;
        Params::GameplayStatics_SpawnObject p{};
        p.objectClass = cls;
        p.Outer_0 = outer;
        CallNative(UGameplayStatics::GetDefaultObj(), fn, &p);
        return PtrOk(p.ReturnValue) ? p.ReturnValue : nullptr;
    }
}
