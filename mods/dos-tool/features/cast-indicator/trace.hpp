#pragma once
// Dev trace (#81): touch `cast-indicator.trace` next to the game exe; the next multi-hit cast logs every
// UFunction that passes ProcessEvent (game::OnEvery while armed) (object class, count, first..last offset) from montage start until
// kTail s after it ends (at most kMax s), as `[cast-trace]` lines in dos-tool.log. One cast per trigger.
namespace cast_trace {
    constexpr double kTail = 1.0, kMax = 8.0;
    bool Active();                    // any thread
    bool Wanted();                    // any thread: armed or active (the every-call subscription is needed)
    void Arm();                       // render thread: the trigger file was found
    void Begin(const char* what);     // game thread: a multi-hit cast started
    void Event(void* obj, void* fn);  // game thread, every ProcessEvent while Active()
    void End();                       // game thread: the montage ended
    void Tick();                      // game thread: dumps once the window closed
    void Note(const char* line);      // any thread: an extra line, with its offset, while Active()
}
