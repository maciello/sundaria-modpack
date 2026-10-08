#include "reflect.hpp"
#include "umg.hpp"
#include "CoreUObject_classes.hpp"

using namespace SDK;
using umg::PtrOk;

namespace reflect {
    std::vector<const FProperty*> Props(const UStruct* s, bool withSupers) {
        std::vector<const FProperty*> out;
        for (const UStruct* c = s; PtrOk(c); c = withSupers ? c->SuperStruct : nullptr)
            for (const FField* f = c->ChildProperties; PtrOk(f); f = f->Next) out.push_back(static_cast<const FProperty*>(f));
        return out;
    }
    std::string Type(const FProperty* p) { return PtrOk(p) && PtrOk(p->ClassPrivate) ? p->ClassPrivate->Name.ToString() : "?"; }
    std::string Name(const FProperty* p) { return PtrOk(p) ? p->Name.ToString() : "?"; }
}
