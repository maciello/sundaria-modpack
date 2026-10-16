#pragma once

// Roll cancel (#12) logic, SDK-free: which key presses end the combat roll at once (maintainer: "Sofort", any time).
namespace roll_cancel {
    enum class Input { Jump, Ability, Other };  // the controller's key event: InpActEvt_Jump*, InpActEvt_Ability<N>, rest
    struct Options { bool jump = true, abilities = true; };

    // true = end the roll before the game handles this press, so the game's own jump / ability runs
    inline bool Cancel(bool rolling, bool press, Input in, const Options& o) {
        if (!rolling || !press) return false;  // releases never cancel (a held key would end the next roll)
        if (in == Input::Jump) return o.jump;
        if (in == Input::Ability) return o.abilities;
        return false;
    }
}
