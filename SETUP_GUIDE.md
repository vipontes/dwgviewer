# Setting up the DWG viewer in Qt Creator — start to finish

This picks up from the prototype already in this zip (`src/`, `third_party/libdxfrw/`,
`CMakeLists.txt`) — it already builds and renders correctly; this guide is about
getting it running *inside Qt Creator* as a real project you'll keep working in,
and about exactly what to (and not to) pull from LibreCAD.

## The short answer to "what do I copy from LibreCAD"

**Just one folder: `libraries/libdxfrw`.** That's it. Everything else in this
project (`src/dwg_document.*`, `src/viewer_widget.*`, `src/main.cpp`) is new
code written against libdxfrw's own `DRW_Interface`, not copied from
LibreCAD's `lib/engine` or `lib/gui`. That's the architecture we validated
together — writing a small fresh implementation of `DRW_Interface` turned out
to be less code and fewer hidden dependencies than forking LibreCAD's
`RS_Graphic`/`RS_GraphicView` classes and stripping them down.

If later on you specifically want to match LibreCAD's own visual conventions
— its exact line-type dash patterns, its font rendering for `TEXT`/`MTEXT`,
its ACI color table — those live in `lib/engine/document/patterns`,
`lib/engine/document/fonts`, and `rs_pen.cpp`/`rs_pen.h`, and can be pulled in
piecemeal later. You don't need any of them to get a working viewer.

---

## 0. Install prerequisites

**Ubuntu/Debian** (what I tested this against):
```bash
sudo apt update
sudo apt install cmake qt6-base-dev build-essential
```
Boost is *not* needed for this — LibreCAD's own README lists Boost as a
requirement for the full application, but the parser-only path we're using
(`libdxfrw` + our own viewer code) never touches it. I built and ran this
prototype without it.

**macOS:**
```bash
brew install cmake qt
```
(Homebrew's `qt` formula is Qt6. Qt Creator itself is usually easier to get
via the [Qt Online Installer](https://www.qt.io/download-qt-installer) if you
want the IDE too, not just the libraries.)

**Windows:** easiest path is the [Qt Online Installer](https://www.qt.io/download-qt-installer),
which installs Qt Creator, a Qt6 kit (MinGW or MSVC), and CMake together in
one go — pick that over a manual toolchain if you're starting fresh.

---

## 1. Vendor libdxfrw into your own repo

Rather than reusing my copy blindly, pull a fresh one so you know exactly
where it came from and can diff against upstream later:

```bash
git clone --depth 1 https://github.com/LibreCAD/LibreCAD.git /tmp/librecad-src
mkdir -p ~/projects/dwgviewer/third_party
cp -r /tmp/librecad-src/libraries/libdxfrw ~/projects/dwgviewer/third_party/libdxfrw
rm -rf ~/projects/dwgviewer/third_party/libdxfrw/.git   # if any nested .git exists
```

**Why a plain vendored copy instead of a git submodule pointing at the
standalone `LibreCAD/libdxfrw` repo:** that standalone repo has drifted from
the DWG-reading code that actually lives in LibreCAD's in-tree fork (see the
note in `libdxfrw/README.md` about this). Pointing a submodule at the
standalone repo would silently give you a *different, less complete* DWG
reader than the one this prototype was tested against. A manual vendored
copy, refreshed occasionally with the same `clone` + `cp` above and diffed
before you overwrite, is the safer call until that divergence is resolved
upstream.

Keep `third_party/libdxfrw/COPYING` and `README.md` intact — that's the
GPLv2 license text and attribution, and you'll want it if you ever
distribute this.

---

## 2. Lay out the rest of the project

```
dwgviewer/
├── CMakeLists.txt
├── CLAUDE.md              # project brief for Claude Code — see below
├── README.md
├── third_party/
│   └── libdxfrw/           # from step 1, unmodified
├── src/
│   ├── main.cpp
│   ├── dwg_document.h/.cpp
│   └── viewer_widget.h/.cpp
└── sample_data/
    └── basic.dxf
```

Copy `CMakeLists.txt`, everything under `src/`, and `sample_data/basic.dxf`
from the zip I gave you straight into this layout — that code already
compiles and renders correctly, no changes needed to get started.

Initialize git now, before opening Qt Creator, so the IDE picks it up as a
version-controlled project from the start:
```bash
cd ~/projects/dwgviewer
git init
git add .
git commit -m "Initial viewer prototype: vendored libdxfrw + custom viewer"
```

---

## 3. Open it in Qt Creator

**Don't use the "New Project" wizard for this.** The wizard generates its own
`CMakeLists.txt`, a `mainwindow.ui` form file, and other boilerplate that
doesn't match this project's structure — you'd just be reconciling two
skeletons. Since you already have a working `CMakeLists.txt`, open it
directly instead:

1. **File → Open File or Project…**
2. Navigate to `~/projects/dwgviewer/CMakeLists.txt` and select it.
3. Qt Creator recognizes this as a CMake project (there's no separate
   Qt Creator "project file" — `CMakeLists.txt` *is* the project file for a
   CMake-based build, unlike the older `.pro`/qmake style LibreCAD itself
   still partially uses).

## 4. Configure the Kit

Qt Creator will show a **"Configure Project"** screen listing available
**Kits** (a Kit = a specific Qt version + compiler + debugger combination).

- Pick whichever Kit has **Qt 6.x** and a Desktop target.
- If no Qt6 kit shows up (common if you installed Qt via `apt` rather than
  the Qt Online Installer): go to **Edit → Preferences → Kits**, check
  under **Qt Versions** that Qt Creator found `qmake6` (usually at
  `/usr/lib/qt6/bin/qmake6` on Ubuntu), and add a Kit pairing that Qt
  version with your default compiler (GCC/Clang) if one isn't already
  auto-created.
- Click **Configure Project**. Qt Creator runs `cmake` in the background —
  watch the **General Messages** / **Compile Output** pane at the bottom;
  it should end with `Configuring done` / `Generating done`, matching what
  we saw when I ran this same configure step directly.

## 5. Set run arguments (important — easy to miss)

`dwgviewer` expects a file path as its first argument (`./dwgviewer
file.dxf`). Running from Qt Creator's ▶ button with no arguments will just
print the usage message and exit. Set this once:

1. Left sidebar → **Projects** icon.
2. Under your Kit, click **Run** (in the "Run Settings" list).
3. In **Command line arguments**, put the path to a test file, e.g.:
   ```
   %{sourceDir}/sample_data/basic.dxf
   ```
   (`%{sourceDir}` is a Qt Creator variable that expands to your project
   root, so this keeps working regardless of where you build.)

## 6. Build and run

- **Build** with the hammer icon (or `Ctrl+B`). First build compiles all of
  `libdxfrw` (~30 files) plus your three source files — takes a little
  while the first time, incremental builds after that are fast since
  `libdxfrw` won't have changed.
- **Run** with the green ▶ (or `Ctrl+R`). You should get a window showing
  the sample DXF's line, circle, arc, and closed polyline — the same
  geometry I rendered and checked in the earlier PNG.
- Mouse wheel zooms around the cursor, left-drag pans.

If the build fails at the CMake configure step specifically complaining it
can't find Qt6, that's almost always the Kit's Qt version not being
registered correctly — back to step 4.

## 7. Test against a real DWG

The sample DXF only proves the pipeline works structurally — the real test
is a DWG file from an actual CAD export, since that's where DWG version
quirks and unsupported entity types tend to surface. If you don't have one
handy, LibreCAD's own repo has none checked in either (I checked — there's
no bundled sample DWG), so pull a small one from any CAD software you have
access to, or search for a public-domain sample DWG online. Run it the same
way:
```
./dwgviewer path/to/real_file.dwg
```
Watch stderr — `DwgDocument::loadFile` prints how many shapes it parsed, and
any entity types this prototype doesn't handle yet (`TEXT`, `INSERT`,
`SPLINE`, `HATCH`, …) will simply be silently skipped rather than crashing,
since their callbacks are no-ops right now. That's useful signal for what to
implement next.

---

## 8. Working with Claude Code on this project

I've included a `CLAUDE.md` at the project root (below) — Claude Code reads
this automatically at the start of a session in this directory, so it'll
have the architecture context without you re-explaining it each time.

A few practical notes specific to this project:

- **Treat `third_party/libdxfrw/` as read-only/vendored.** It's noted as
  such in `CLAUDE.md`, but that's guidance Claude Code follows as
  instructions, not a hard technical block — if you want an actual
  enforced restriction, you can add a `deny` rule for that path in
  `.claude/settings.json`, though be aware there have been reported
  reliability issues with `deny` enforcement in some Claude Code versions,
  so don't treat it as a substitute for reviewing diffs before committing.
- **Good first task to hand it**: extending entity coverage. The pattern is
  identical each time — override one more `DRW_Interface` callback in
  `dwg_document.h`/`.cpp` to collect the geometry, add one more `ShapeKind`
  case in `viewer_widget.cpp`'s paint switch. `TEXT`/`MTEXT` and `INSERT`
  (block references) are the two most commonly-needed ones missing right
  now.
- Point it at real DWG files early and often — ask it to add a
  `--dump` mode to `main.cpp` that lists every entity type encountered
  (including ones we currently no-op) so you can see what real-world files
  actually contain before deciding what to implement next.
