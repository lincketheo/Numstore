A rehearsal of release.md that publishes nothing real. Run it once now, and
again whenever release.yml changes.

What stands in for what:

- PyPI: TestPyPI, a throwaway index with the same API.
- A pushed vX.Y.Z tag: a draft GitHub release. GitHub does not create the git
  tag until a draft is published, so nothing triggers and no tag exists.
- The version: 1.5.0rc1, never a real one.
- Pick a fresh rc number every attempt. Neither index lets you re-upload a
  version, even a failed one.

Route A runs the real workflow. Route B does the same steps by hand, for when
you do not want to wait on CI. Route A needs the testpypi environment and
publisher from the one-time setup in release.md.

Route A, dry-run the real workflow:

- Put a pre-release number on a scratch branch.

```sh
git checkout -b release-rehearsal
echo 1.5.0rc1 > version.txt
git commit -am "rehearsal"
git push -u origin release-rehearsal
```

- CMake ignores the rc1 and builds 1.5.0; the wheel keeps the full 1.5.0rc1.
  project(VERSION) only accepts digits, so that trim is deliberate.
- Run the workflow against TestPyPI.

```sh
gh workflow run release.yml --ref release-rehearsal -f testpypi=true
gh run watch
```

- No tag is involved, so the version check skips its comparison and the
  github-release job does not run. What you are testing is the hard part:
  every wheel builds, the sdist builds, twine check passes, and trusted
  publishing is wired up.
- Install it from TestPyPI. The extra index is for numpy, which TestPyPI does
  not have. Run it from /tmp: inside the repo, import pynumstore picks up the
  source directory instead of the installed package.

```sh
cd /tmp
python3 -m venv rehearsal && . rehearsal/bin/activate
pip install --index-url https://test.pypi.org/simple/ \
            --extra-index-url https://pypi.org/simple/ \
            pynumstore==1.5.0rc1
python -c "import pynumstore as ns; print(ns.__version__)"
deactivate
cd -
```

- Rehearse the notes and the draft release. This is what the github-release job
  does, by hand, and the only part of the tag path you can exercise without
  a tag.

```sh
awk '/^## 1\.5\.0( |$)/ { f = 1; next } f && /^## / { exit } f' CHANGELOG.md > /tmp/notes.md
cat /tmp/notes.md

make sdist
gh release create v1.5.0rc1 \
  --draft --prerelease \
  --target release-rehearsal \
  --title "v1.5.0rc1" \
  --notes-file /tmp/notes.md \
  dist/*.tar.gz
gh release view v1.5.0rc1 --web
```

- Read the notes as a stranger would. Thin notes mean a thin changelog section,
  and that is the thing to fix.
- Clean up, then check version.txt says what it should.

```sh
gh release delete v1.5.0rc1 --yes
git checkout develop
git branch -D release-rehearsal
git push origin --delete release-rehearsal
rm -rf dist /tmp/rehearsal /tmp/notes.md
cat version.txt
```

- Deleting the rehearsal files from
  https://test.pypi.org/manage/project/pynumstore/releases/ is optional.

Route B, the same thing locally:

- No CI and no branch. You get one wheel, for your own Python and platform,
  instead of all of them.
- Get a TestPyPI token once, at https://test.pypi.org/manage/account/token/
  Trusted publishing only works from the workflow, so a hand upload needs a
  token. Put it in ~/.pypirc:

```ini
[testpypi]
username = __token__
password = pypi-AgENdGVzdC5weXBpLm9yZ...
```

- Build and check. twine check is what catches a README or metadata problem,
  the same check the workflow runs before it uploads anything.

```sh
echo 1.5.0rc1 > version.txt
make clean
make sdist
make wheel
twine check dist/*
```

- Upload to the mock index.

```sh
twine upload --repository testpypi dist/*
```

- Install and verify it the same way as Route A.
- Clean up.

```sh
git checkout version.txt
make clean
rm -rf dist
```

What a rehearsal cannot tell you:

- Whether the real PyPI publisher is configured. TestPyPI and PyPI are separate
  registrations. If you only ever rehearse, the first real tag can still fail on
  the publish step. That is safe: it fails before the release is created, so you
  fix the publisher and re-push the tag.
- Whether the pypi environment gate works. Add the reviewer to testpypi too if
  you want to see the approve button in a rehearsal.
