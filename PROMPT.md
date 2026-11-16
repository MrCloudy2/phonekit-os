# Prompt for your AI coding assistant

Open this repository in a coding assistant that can run commands (Claude Code, Codex CLI, Gemini CLI, …),
plug the phone into the PC, and paste everything below the line. Replace the parts in `<…>`.

---

I want to run PhoneKit on my phone: **<brand and exact model number, e.g. Samsung SM-G715FN>**.
It currently runs **<stock Android / LineageOS / …>**, and the bootloader is **<unlocked / locked / don't know>**.
My PC runs **<Linux distro / Windows / macOS>**.

You are helping me port and install PhoneKit from this repository. Read `AGENTS.md`, `README.md`,
`docs/recovery.md`, `docs/porting-guide.md`, and `docs/how-it-was-built.md` first. Then follow the porting
guide stage by stage. Rules:

1. **Feasibility before flashing.** Do Stage 0 of the porting guide first: research my exact model
   (LineageOS wiki, TWRP, postmarketOS wiki, XDA) and tell me plainly whether it's likely possible, what the
   blockers are, and what I'll lose (data wipe on unlock, warranty, Samsung Knox). If it isn't feasible, say so
   and stop.
2. **Check for existing work.** Look in `devices/` for my model and in `socs/` for my SoC. Reuse what's
   there; don't rediscover known quirks.
3. **Backups before every first.** Before the first flash of any partition, make sure I have a known-good
   image of it (dumped or official) with a sha256, stored off the phone. Tell me my phone's emergency mode
   and button combo, and have me try it once.
4. **Ask before anything irreversible or risky:** unlocking, every flash, partition writes, kernel config
   trims. Say what will happen and how we undo it. Never write to bootloader, modem, TrustZone, or
   preloader partitions. Never relock the bootloader.
5. **Use the repo's tools** (`make import/deploy/flash/logs`, `scripts/ppctl.py`, `scripts/test-*.py`)
   rather than ad-hoc commands, so backups, verification and the watchdog apply.
6. **Verify, don't assume.** Use `ppctl.py screenshot` captures and the test suites to check each stage. When
   something has to be seen with eyes (colours, picture after wake), ask me a specific question.
7. **One change at a time** when debugging a boot that doesn't work, and read `make logs` right after a
   failed boot.
8. **Write it down as data.** Record device facts and quirks in `devices/<vendor>-<codename>/device.yaml`,
   SoC-wide ones in `socs/<soc>/quirks.md`. Put device-specific values in `device.mk`, not `#ifdef`s in paneld.
9. **Keep the battery safe.** The charge limiter must work before I leave the phone plugged in long-term.
10. When we're done (or stuck), write a short verification note and help me open a pull request with the
    device folder, even if it's a partial port.

Start with Stage 0 and tell me what you find.
