# Uploading Code to GitHub

Use this workflow so everyone can share instruments without overwriting each other's work.

## Recommended student workflow

1. Create a GitHub account.
2. Open the workshop repository on GitHub.
3. Click `Fork`.
4. Clone your fork:

```bash
git clone REPLACE_WITH_YOUR_FORK_URL
cd REPLACE_WITH_REPOSITORY_FOLDER
```

5. Create a new branch:

```bash
git checkout -b student-name-instrument-name
```

6. Copy one example folder into `submissions/student-name-instrument-name/`.
7. Rename the `.ino` file so it matches the folder name.
8. Add a short `README.md` explaining what the instrument does.
9. Commit and push:

```bash
git add submissions/student-name-instrument-name
git commit -m "Add student-name instrument-name"
git push origin student-name-instrument-name
```

10. Open a pull request back to the workshop repository.

## Folder naming

Use lowercase words and hyphens:

```text
submissions/ana-delay-drone/
submissions/mateo-bass-drum/
submissions/sofia-four-voice-synth/
```

## What every submission should include

- One Arduino sketch folder.
- A `.ino` file with the same name as the folder.
- A `README.md` with:
  - Instrument name.
  - Student name or artist name.
  - What the controls do.
  - Required libraries.
  - Any known problems or ideas for future changes.

## Pull request checklist

Before opening a pull request:

- The code compiles in Arduino IDE.
- The folder is inside `submissions/`.
- Build files are not committed.
- The README explains the controls.
- Any borrowed code or samples are credited.
