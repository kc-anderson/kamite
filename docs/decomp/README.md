# Giving back to the decomp

What Kamite has learned about the game that the matching decompilation (LucasPicoli/god-hand-decomp) does not have yet, packaged in its formats, plus the steps to send it. Kamite uses the decomp's names in return: `recomp/kamite_functions.csv` (function names, refreshed from its `config/symbol_addrs.txt`) and `src/kamite/game_layout.h` (struct offsets under its field names).

## What's here

| File | What | Where it goes |
|---|---|---|
| `pr-1-message-font-names.md` + `patches/pr-1-message-font-names.patch` | Six descriptive names for the message font's draw helpers (glyph, icon, colour, icon test, icon advance, per-code draw). | Pull request |
| `pr-2-gamework-fields.md` + `patches/pr-2-gamework-fields.patch` | GameWork's pad words (held, previous, pressed, released), the vblanks-per-frame byte at 0x569, the KeyStop bit of flags5D8. | Pull request |
| `issue-camera-eye-target.md` | cCamera's `eye` (0x200) and `target` (0x210) look swapped (sceVu0CameraMatrix's arguments). | Issue |
| `issue-player-mode5.md` | `UpdateBossPhase_10B780` is entry 5 of the player's mode table, not a boss routine. | Issue |
| `findings.md` | Things we know with less proof, or that need a body matched first. For later PRs, not to send as they are. | Nothing yet |

Every patch is checked against the decomp at beb2575f (the unmodified checkout builds an ELF whose sha256 equals retail); each PR file's "Checks" section records what has actually been run and its result. **Don't submit a PR whose Checks section still says pending.**

## Their rules that matter here (CONTRIBUTING.md §5, §6)

- **You are the author.** Read every line of the patch and the evidence before you push; be ready to explain it in review. The evidence for each line is in the PR file.
- **No tool attribution.** No `Co-authored-by` trailers, no "Generated with ..." lines in commits, PR text, issues or comments, no tool config files (`CLAUDE.md`, `.claude/`...). The texts here have none; don't add any.
- **One focused change per PR**, subject tagged like theirs (`names: ...`, `headers: ...`).
- **No game data, no SDK material** in commits, PRs or issues. The patches and texts here contain only names, offsets and addresses.
- **The gate decides**: run the build and `scripts/session_check.sh` yourself before pushing, even though they passed here.

## Step by step

### 0. Decide what name the PRs show

Commits record an author name and email, and the PR shows your GitHub account. To keep your real name off it:

- Commit as `K` (or any handle) with your GitHub no-reply address: GitHub → Settings → Emails → "Keep my email addresses private" shows it (`<id>+<username>@users.noreply.github.com`).
- The GitHub account name itself shows on every PR and issue. If your account name is your real name, use a second account for public work (or rename the account; GitHub redirects the old name).

### 1. One-time setup (Windows: in WSL, Ubuntu)

The decomp's tooling is Linux-only. In PowerShell: `wsl --install -d Ubuntu`, reboot, open Ubuntu, then:

```bash
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y git python3 python3-venv curl build-essential libc6:i386 binutils-mips-linux-gnu binutils-mipsel-linux-gnu
```

(`binutils-mipsel-linux-gnu` may not exist on every Ubuntu; `binutils-mips-linux-gnu` plus the toolchain script's own check was enough here.)

Fork on GitHub: open github.com/LucasPicoli/god-hand-decomp → **Fork** → your account. Then:

```bash
git clone https://github.com/<you>/god-hand-decomp.git
cd god-hand-decomp
git remote add upstream https://github.com/LucasPicoli/god-hand-decomp.git
git config user.name "K"
git config user.email "<id>+<you>@users.noreply.github.com"
```

The last two lines set the identity for this repository only.

Put your disc image at the repo root as `God Hand (USA).iso` (the same dump Kamite uses; from Windows it is under `/mnt/c/...`, copy it in). It is git-ignored; never commit it. Then the README's setup, which takes a while the first time (about an hour on two cores; the split and the first build are the slow part):

```bash
./scripts/extract_iso.sh
python3 scripts/afs_extract.py
./scripts/setup_toolchain.sh
mkdir -p build
mipsel-linux-gnu-objcopy -O binary disc_extract/SLUS_215.03 build/SLUS_215.03.rom
./scripts/resplat.sh
.venv/bin/python -m splat split config/r207.yaml
.venv/bin/python scripts/extract_elf_metadata.py
.venv/bin/python compile.py --setup
.venv/bin/python compile.py
sha256sum build/SLUS_215.03.elf disc_extract/SLUS_215.03
```

The two sums must be equal. If they are, your setup is right.

Copy the patches into WSL so git can read them, e.g. `cp /mnt/c/Users/<you>/Desktop/Claude/godhand-recomp/docs/decomp/patches/* ~/` (or `git pull` Kamite in WSL and use its path).

### 2. One PR (repeat for each)

```bash
git fetch upstream
git switch -c names-message-font upstream/main
git apply --check ~/pr-1-message-font-names.patch   # no output = it applies
git apply ~/pr-1-message-font-names.patch
git diff                                            # read every line against pr-1-*.md
.venv/bin/python scripts/check_naming_sync.py
.venv/bin/python compile.py && sha256sum build/SLUS_215.03.elf disc_extract/SLUS_215.03
scripts/session_check.sh
```

If `git apply --check` fails, upstream moved since beb2575f: say so in the Kamite chat and the patch gets rebased.

Commit with the message from the PR file (also saved as `patches/msg-pr-1.txt` / `msg-pr-2.txt`; copy them next to the patches). The build regenerates `config/r207.undefined_funcs_auto.txt` and `config/r207.undefined_syms_auto.txt` (tracked as stubs upstream): never commit those.

```bash
git status            # the patch's files, plus two the build rewrites by itself
git restore config/r207.undefined_funcs_auto.txt config/r207.undefined_syms_auto.txt
git add -u            # only tracked files that changed: the patch's
git status            # check again: nothing else staged
git commit -F ~/msg-pr-1.txt
git push -u origin names-message-font
```

Then on GitHub, your fork shows **Compare & pull request**. Base: `LucasPicoli/god-hand-decomp` `main`. Title = the commit subject; description = the commit body, plus one line on how you checked it ("Built: ELF sha256 matches retail; check_naming_sync and session_check pass."). Nothing about tools.

PR 2 the same way from a fresh branch off `upstream/main` (`git switch -c headers-gamework upstream/main`), with `pr-2-gamework-fields.patch`. The two PRs don't depend on each other.

### 3. The issues

github.com/LucasPicoli/god-hand-decomp → **Issues** → **New issue**: title and body from the issue file, after the "Before you post it" checks in it. Issues need no setup or build.

### 4. Review

Answer review comments yourself; if a reviewer asks for a change you're not sure of, paste it into a Kamite chat. When something is merged, the weekly upstream watch picks up the names and Kamite takes them in (`tools/symbols/import_decomp_names.py`, `src/kamite/game_layout.h`).

## Order

Issues first or PRs first doesn't matter. Suggested: PR 1 (smallest, cleanest), then the camera issue (the most useful correction for them), then PR 2, then the mode-5 issue.
