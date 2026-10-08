// just test. Writes build/live-bridge_test.png (2x2) when given a path: open it to see red, green, blue, white.
#include "../live-bridge.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace live_bridge;

int main(int argc, char** argv) {
    std::vector<size_t> at;
    auto w = Words("  call pawn Foo  [1, \"a b\"]", &at);
    assert(w.size() == 6 && w[0] == "call" && w[2] == "Foo");
    assert(std::string("  call pawn Foo  [1, \"a b\"]").substr(at[2]) == "Foo  [1, \"a b\"]");

    Path p = ParsePath("pawn.mAbilitySystemComponent.SpawnedAttributes[1].RAP");
    assert(p.err.empty() && p.root == "pawn" && p.steps.size() == 3);
    assert(p.steps[1].name == "SpawnedAttributes" && p.steps[1].index == 1 && p.steps[2].index == -1);
    assert(ParsePath("@3").root == "@3" && ParsePath("@3").steps.empty());
    assert(ParsePath("obj:Foo.Bar").root == "obj:Foo");
    assert(!ParsePath("pawn..X").err.empty());
    assert(!ParsePath("pawn.X[").err.empty());
    assert(!ParsePath("pawn.X[a]").err.empty());
    assert(!ParsePath("pawn.X[1]Y").err.empty());
    assert(!ParsePath("pawn[0]").err.empty());

    Path al = ParsePath("pawn.Health");
    ExpandAlias(al);
    assert(al.root == "pc" && al.steps.size() == 2 && al.steps[0].name == "Pawn" && al.steps[1].name == "Health");
    Path pl = ParsePath("pc.Pawn");
    ExpandAlias(pl);
    assert(pl.root == "pc" && pl.steps.size() == 1);
    assert(Err("a\"b") == "{\"ok\":false,\"error\":\"a\\\"b\"}");

    std::vector<reflect::Scalar> a;
    std::string err;
    assert(ParseArgs("[1.5, \"Fire \\\"x\\\"\", true, false, -2]", a, err) && a.size() == 5);
    assert(a[0].num == 1.5 && a[1].isStr && a[1].str == "Fire \"x\"" && a[2].num == 1 && a[3].num == 0 && a[4].num == -2);
    a.clear();
    assert(ParseArgs("", a, err) && a.empty());
    assert(ParseArgs("[]", a, err) && a.empty());
    assert(ParseArgs(" 7 ", a, err) && a.size() == 1 && a[0].num == 7);
    a.clear();
    assert(!ParseArgs("[1 2]", a, err));
    assert(!ParseArgs("[\"x", a, err));
    assert(!ParseArgs("nope", a, err));

    assert(Match("onprojectilehit", "BP_Arrow_C::OnProjectileHit"));
    assert(Match("Montage|Hit$", "Actor::ReceiveHit") && Match("Montage|Hit$", "X::MontagePlay") && !Match("Montage|Hit$", "X::HitMe"));
    assert(Match("^BP_.*::Receive", "BP_Foo_C::ReceiveTick") && !Match("^BP_.*::Receive", "ABP_Foo_C::Tick"));
    assert(Match("a.c", "xxABCyy") && !Match("a.c", "ac") && Match("", "anything") && Match("^$", "") && !Match("^$", "x"));
    assert(Match("tick_*$", "Tick") && !Match("x_1", "x_2"));

    const uint8_t s[] = "123456789";
    assert(Crc(s, 9) == 0xCBF43926u);                                // CRC-32 check value
    assert(Adler(reinterpret_cast<const uint8_t*>("Wikipedia"), 9) == 0x11E60398u);
    const uint8_t px[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    const std::vector<uint8_t> png = Png(2, 2, px);
    assert(png.size() > 8 && !std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8));
    const uint8_t iend[] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    assert(!std::memcmp(png.data() + png.size() - 12, iend, 12));
    std::vector<uint8_t> big(300 * 300 * 3, 7);  // > one 64 KiB stored block
    assert(Png(300, 300, big.data()).size() > big.size());
    if (argc > 1)
        if (FILE* f = std::fopen(argv[1], "wb")) { std::fwrite(png.data(), 1, png.size(), f); std::fclose(f); }
    std::puts("live-bridge_test: ok");
}
