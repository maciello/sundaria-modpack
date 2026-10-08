-- Ability mods: examples. Files starting with "_" are NOT loaded: copy this file to e.g. my.lua
-- (next to it, in <game>/Archon/Binaries/Win64/dos-mods/abilities/) and edit. Saving reloads it in ~1 s.
-- Errors (file:line), what loaded and ctx:log output go to log.txt in this folder.
--
-- Names: the short ability name ("FireBall" for BP_GameAbility_FireBall_C), the full class name, or a
-- pattern with * ("Bash*" also matches the scroll versions, "*" matches every ability).
-- Host only in co-op: as a client the host's game decides.

-- 1. Tweaks: animation speed (0.1..5), cooldown scale (0..1, 0.5 = half the cooldown), projectile class.
ability.tweak("FireBall", {anim_rate = 1.3, cooldown = 0.5})
ability.tweak("*", {anim_rate = 1.1})              -- every ability a bit faster; named tweaks win
-- Projectile swap: shoot another projectile class (it must be loaded in the game: a class some
-- ability already used this session). Works for abilities that shoot projectiles.
ability.tweak("ToxicArrow", {projectile = "BP_Projectile_PoisonArrow_C"})

-- 2. Events: "activate" (cast starts), "out" (its effect/projectile frame), "end" (animation over).
ability.on("FireBall", "out", function(ctx)
  ctx:log(ctx.ability, "is out")                    -- goes to log.txt
end)

-- Actions inside a handler:
--   ctx:dash(speed)            launch forward (where the hero faces), 0..5000
--   ctx:apply_effect("BP_GameplayEffect_X_C")   apply a GameplayEffect class to yourself
--   ctx:play_rate(x)           change the running animation's speed
--   ctx:cancel()               cancel the ability
--   ctx:log(...)               write to log.txt

-- 3. New abilities on a donor: takes over the donor's casts (its animation and effect), adds its own
-- tweaks and handlers. Switch it on/off per ability in the menu.
ability.new{
  name = "Blink Shot",
  donor = "FireBall",
  cooldown = 0.6,
  anim_rate = 1.5,
  on_activate = function(ctx) ctx:dash(1200) end,
  on_out = function(ctx) ctx:log("Blink Shot fired") end,
}
