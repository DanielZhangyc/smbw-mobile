# Notes for agents

- This repository runs Nintendo Switch games natively on Apple platforms: the game's own code runs, rewritten only where Apple's kernel requires it. One game is supported so far: Super Mario Bros. Wonder (base game v0 and update 1.2.1, profile `smbw`). Start with [README.md](README.md) for status and build steps.
- Each game's constants and shader tables form a game profile in `dm/games/<id>/`: `game.tsv` plus the tables and `dmsh_chain.h`. Mario's profile is `smbw`. Tools pick the profile with `DM_GAME`; `make -C dm/host` and `build_ipa.sh` pick it with `GAME`. All default to `smbw`. Put per-game values in the profile, not in code ([dm/games/README.md](dm/games/README.md)).
- Per-build knowledge is keyed by the NSO build ID: rewriter cuts in `dm/tools/patch_nso.py`, the profile's `build` rows (the builds the NSP script accepts, and their display versions), and host behaviour that uses fixed module addresses checks `module_t.build_id` (from `fixups.tsv`; `dm_main_build()`, `DM_D2R_BUILD` for code inherited from D2R). When a new game build appears, carry addresses over with `dm/tools/nso_fnmap.py` and check them by disassembly; never let fixed addresses run on a build they were not found in.
- To analyse or port **another** Switch title, first run `dm/ios/tools/build_ipa_from_nsp.py` on its NSP folder. With no matching profile, it writes a survey to `<NSP folder>/<title ID>_survey/survey.md`. Use that survey to add a profile following [dm/games/README.md](dm/games/README.md), then validate host initialization and graphics against the new build.
- Game files, keys and anything extracted from them live in the user's workspace (`~/<id>-work`, Mario `~/smbw-work`) or in the NSP folder's `<id>_build/`, never in this repository. Do not print key values.
- Code comments are bilingual (Chinese line, then English line); red log lines start with `【红】`. Keep both conventions.
- After changing anything on the shader translation chain, run the chain procedure for each affected profile. The chain covers `dm/gfx/tools/dmsh_*`, `Program.cs`, the profile's `shader_map.tsv` / `shader_facts.tsv` / `shader_pairs.tsv`, and its `nvn_*` / `shader_*` keys. The procedure:
  1. Run `dmsh_chain.py --header`, which writes the profile's `dmsh_chain.h`.
  2. Rebuild the shaders (Mac and, with `IOSOUT`, iOS).
  3. Regenerate the profile's `c1_full.tsv`.
  4. Rebuild the host.

  The host only loads libraries of its own chain version. The translator is `dm/gfx/tools/dmsh_xlate_bin`, which matches `dmsh_chain.lock`; a fresh `dotnet publish` does not.
- iOS builds are unsigned IPAs that the user sideloads and tests; diagnostics come back as the app's `Documents/log.txt`. Do not sign or install builds on the user's devices.
- Commit only when asked, using the repository owner's GitHub identity: `git -c user.name=DanielZhangyc -c user.email=134716838+DanielZhangyc@users.noreply.github.com commit`.
