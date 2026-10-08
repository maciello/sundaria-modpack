#include "umg.hpp"

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
        auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }
    bool Alive(const UObject* o, int32_t idx) { return PtrOk(o) && UObject::GObjects->GetByIndex(idx) == o; }
    bool Live(const UObject* o) { return PtrOk(o) && !(int(o->Flags) & 0x30); }

    APlayerController* LocalPC() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() < 1) return nullptr;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        return PtrOk(lp) && PtrOk(lp->PlayerController) ? lp->PlayerController : nullptr;
    }

    bool IsWorldTick(const void* fn) {
        static UFunction *online = nullptr, *game = nullptr;  // classes load late (main menu has neither): resolve until found
        if (!online) if (UClass* c = ABP_PlayerControllerOnline_C::StaticClass()) online = c->GetFunction("BP_PlayerControllerOnline_C", "ReceiveTick");
        if (!game) if (UClass* c = ABP_PlayerControllerGame_C::StaticClass()) game = c->GetFunction("BP_PlayerControllerGame_C", "ReceiveTick");
        return fn && (fn == online || fn == game);
    }

    FText Text(const std::string& s) {
        static UFunction* fn = UKismetTextLibrary::StaticClass()->GetFunction("KismetTextLibrary", "Conv_StringToText");
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
        Params::KismetTextLibrary_Conv_StringToText t{};
        t.inString = FString(w.c_str());
        if (fn) CallNative(UKismetTextLibrary::GetDefaultObj(), fn, &t);
        return t.ReturnValue;
    }

    UObject* Spawn(UClass* cls, UObject* outer) {
        static UFunction* fn = UGameplayStatics::StaticClass()->GetFunction("GameplayStatics", "SpawnObject");
        if (!fn || !PtrOk(outer)) return nullptr;
        Params::GameplayStatics_SpawnObject p{};
        p.objectClass = cls;
        p.Outer_0 = outer;
        CallNative(UGameplayStatics::GetDefaultObj(), fn, &p);
        return PtrOk(p.ReturnValue) ? p.ReturnValue : nullptr;
    }
}
