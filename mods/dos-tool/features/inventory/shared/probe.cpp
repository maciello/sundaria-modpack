#include "sdk.hpp"
#include "logger.hpp"

// Dev probe (item-sort.probe file trigger): every live item container. The one allowlisted GObjects walk here.
using namespace items::sdk;

namespace items::io {
    std::string ContainersReport() {
        std::string out;
        APlayerController* pc = LocalPC();
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            UBP_ItemContainerComponent_C* c = PtrOk(o) && !o->IsDefaultObject() ? Container(o) : nullptr;
            if (!c || c->Items.Num() == 0) continue;
            out += "\n  " + o->Class->GetName() + " " + o->GetName() + " outer " + (PtrOk(o->Outer) ? o->Outer->GetName() : "-") +
                   ": Items " + I(c->Items.Num()) + (c->PlayerController == pc ? " (mine)" : "");
        }
        return out;
    }
}
