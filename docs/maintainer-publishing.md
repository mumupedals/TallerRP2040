# Maintainer Publishing Guide

This is the recommended way to turn the workshop folder into a GitHub repository for students.

## 1. Keep the repository simple

Use this structure:

```text
README.md
docs/
FourVoiceGate/
DrumLoop/
Dronaldo/
submissions/
.github/
.gitignore
```

Do not commit compiled output such as `.uf2`, `.bin`, `.elf`, `.map`, `build/`, or `build-rp2040/`. The `.gitignore` already excludes those.

## 2. Add missing class material

Before sharing the repo, add:

- Photos of the instrument.
- A wiring diagram or schematic.
- A short video or audio example, if useful.
- A license, if students are allowed to reuse/remix the examples.
- Any extra attribution notes for the bundled `BRAIDS` and `STMLIB` libraries.

Recommended folders:

```text
media/
  instrument-photo.jpg
  wiring-diagram.png
```

The repo already includes `libraries/BRAIDS` and `libraries/STMLIB` so students can compile the workshop sketches without searching for those dependencies.

## 3. Create the GitHub repository

On GitHub:

1. Click `New repository`.
2. Name it something clear, such as `mumu-rp2040-workshop`.
3. Add a short description.
4. Choose public or private.
5. Do not initialize with a README, because this folder already has one.
6. Create the repository.

Then run these commands from this folder:

```bash
git add .
git commit -m "Initial workshop materials"
git branch -M main
git remote add origin REPLACE_WITH_GITHUB_REPOSITORY_URL
git push -u origin main
```

If Git asks who you are:

```bash
git config --global user.name "Your Name"
git config --global user.email "your-email@example.com"
```

## 4. Student upload workflow

For the smoothest class workflow:

- Students fork the repository.
- Students add one folder inside `submissions/`.
- Students open a pull request.
- You review the pull request, compile if needed, then merge.

This avoids students pushing directly to the main repository and accidentally overwriting each other.

## 5. Alternative for beginners

If Git is too much for the group:

1. Ask each student to download the repository ZIP.
2. They create a new folder using the format in `submissions/README.md`.
3. They upload their folder through the GitHub website with `Add file > Upload files`.
4. You move or clean up files if needed.

This is less clean than pull requests, but it is friendlier for first-time GitHub users.
