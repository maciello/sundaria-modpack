# Game events: react to what the game does (`core/game.hpp`)

A feature never polls the world and never sees every ProcessEvent call. It subscribes to the calls it needs;
core keeps a map FName(function) → subscribers and runs only the matching callbacks (#109).

```cpp
// features/<kebab>/<kebab>.cpp (SDK-including): runs after the game's own BP_TriggerBase_C::OnTriggerStateChanged
void OnLever(void* obj, void* fn, void* parms) { if (game::OnGameThread()) g_dirty = true; }
void OnTick(void*, void*, void*) { if (g_dirty.exchange(false)) Update(); }   // world tick: widgets may change here
void OnFrame(const feature::Frame&) override { if (!g_on) { g_on = true; game::On("BP_TriggerBase_C", "OnTriggerStateChanged", &OnLever, true); game::OnWorldTick(&OnTick, true); } }
void Off() override { g_on = false; game::On("BP_TriggerBase_C", "OnTriggerStateChanged", &OnLever, false); game::OnWorldTick(&OnTick, false); }
```

```yaml
api:   # cb = void(void* obj, void* fn, void* parms): UObject*, UFunction*, the call's params; runs after the game's call
  game::On(cls, fn, cb, on):    "one function by FName: fn = its name, cls = the class that DECLARES it (a Blueprint
                                 override is declared by the Blueprint: BP_CharacterBase_C::ReceiveTick runs on NPC_* objects).
                                 cls nullptr = that name on every class (all ReceiveBeginPlay overrides)"
  game::OnWorldTick(cb, on):    "local player controller's ReceiveTick (BP_PlayerControllerGame_C / _Online_C, = umg::IsWorldTick):
                                 the only point to add/remove widgets (#50). None in the main menu"
  game::OnClass(cls, cb, on):   "every function called on an object of exactly class cls. For Blueprint events whose names carry
                                 a K2Node number (BndEvt__…_K2Node_ComponentBoundEvent_974_…, InpActEvt_Ability1_…_40): those
                                 change with game builds, so match them by prefix inside cb (item-sort Triggers, input-feel)"
  game::OnGameTick(cb, on):     "game thread, at most every 8 ms, in every map and menu; obj = fn = parms = null. For work another
                                 thread posted (requests, probes, cvars)"
  game::OnEvery(cb, on):        "every ProcessEvent call: traces only (live-bridge trace, cast-indicator dev trace), subscribed while
                                 the trace runs"
which_one: "know the function → On; widgets/world writes → + OnWorldTick; posted work → OnGameTick; find names with
            `just game trace '<regex>' 5` (prints DeclaringClass::Function = the cls/fn to pass) or `just data events <BP>`"
rules:
  - "ProcessEvent also runs on worker threads: shared state and UFunction calls only behind game::OnGameThread()"
  - "on=false returns once no in-flight call is inside cb (bounded 2 s wait), so Off() may then free what cb uses.
     From inside a callback it returns at once (no self-wait)"
  - "subscribing twice is one subscription; any number of subscriptions (no cap)"
  - "one cb on overlapping events (OnClass(X) + On(X's function)) runs twice for that call: keep them disjoint"
  - "names resolve to FNames once, on the next game-thread call; the class may load later (Blueprints load with a world)"
  - "SetEventFilter (runs BEFORE the game's call, can skip it) is separate: one filter, item-sort"
cost: "per ProcessEvent: one atomic load + one hash lookup by function FName (+ one by object class while any OnClass is on)
       + the matching callbacks. Dev install logs `[cost] ProcessEvent dispatch: avg … us` every 8192 sampled game-thread calls"
code: core/events.hpp (table, SDK-free, test core/test/events_test.cpp), core/events.cpp (subscribe, resolve, dispatch),
      core/game.cpp hkProcessEvent (calls events::Dispatch)
```
