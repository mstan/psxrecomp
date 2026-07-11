# Game tweaks without per-game forks

The Einhander, Tobal 2, and Mega Man Legends forks contain three kinds of work:

1. General runtime fixes, which belong in the shared runtime.
2. Reusable renderer/runtime capabilities selected by `game.toml`, such as the
   existing widescreen, culling, backdrop, controller, and pacing options.
3. Exact guest-code changes, such as gameplay bounds or 30-to-60 FPS divisors.

The third category is now declarative. Games keep guarded replacements beside
their own configuration instead of adding game IDs and addresses to
`code_generator.cpp`:

```toml
[[recompiler.patch]]
id = "60fps-update-divisor"
address = "0x80012340"
expected = "0x24020002"    # addiu v0, zero, 2
replacement = "0x24020001" # addiu v0, zero, 1
note = "Advance gameplay on every vblank"
```

Code generation aborts unless the instruction at `address` exactly equals
`expected`. Combined with the normal disc identity, this prevents a patch for
one revision from silently corrupting another. Duplicate addresses are rejected.

Migration policy:

- Promote generally correct fixes into the shared runtime with cross-title tests.
- Express reusable graphics behavior as an off-by-default config capability.
- Express small guest-code changes as guarded instruction patches.
- Keep patches in the game repository, not this repository.
- Use separate game configs/build variants for optional enhancement sets; keep
  an unpatched fidelity configuration available.

This does not accept arbitrary source snippets or host callbacks. If an exact
replacement is insufficient, add a typed, validated schema feature with an
inert default rather than recreating an unsafe plugin-shaped fork boundary.
