Release checklist. Not part of the docs site. X.Y.Z = new version.

## One-time setup

- PyPI trusted publisher — https://pypi.org/manage/account/publishing/
  - project `pynumstore`, owner `NumstoreDB`, repo `Numstore`, workflow `release.yml`, env `pypi`
- Same at https://test.pypi.org/manage/account/publishing/ — env `testpypi`
- Settings > Environments > New environment — create `pypi` + `testpypi`
  - optional: yourself as required reviewer on `pypi`
- Settings > Pages > Source > GitHub Actions
- `brew install gh && gh auth login`
- No token to store — PyPI trusts the workflow, not a secret

## Release

- Actions tab: last CI + Wheels green

```sh
git checkout develop && git pull && git status
```

- Bump `version.txt` — only place a version lives (wheel, CMake package, `ns.__version__` all read it)

```sh
echo X.Y.Z > version.txt
```

- major = breaking / minor = features / patch = fixes
- `CHANGELOG.md` — new section at top, heading must match version exactly:

```
## X.Y.Z - YYYY-MM-DD
```

- `CITATION.cff` — `version: X.Y.Z`, `date-released: "YYYY-MM-DD"`
- Changelog + CITATION.cff = only files that can't read `version.txt`

```sh
make test-unit
make test-python
make docs
```

- `test-python` asserts installed version == `version.txt` — catches a half-done bump

```sh
git commit -am "release X.Y.Z"
git push
```

- Wait for CI + Wheels green **on that commit**
- Never tag red — PyPI versions are permanent

```sh
git tag vX.Y.Z
git push origin vX.Y.Z
```

- Leading `v` required — workflow triggers on `v*` only
- `gh run watch`
- Workflow: tag vs `version.txt` -> wheels + sdist -> PyPI -> GitHub release (notes = changelog section, sdist + wheels attached)
- Protected `pypi` env -> publish waits on your approval
- Verify:

```sh
pip install --no-cache-dir --upgrade pynumstore
python -c "import pynumstore as ns; print(ns.__version__)"
gh release view vX.Y.Z
```

- https://numstoredb.github.io/Numstore/ — rebuilt by the push
- https://pypi.org/manage/project/pynumstore/releases/ — new version listed

## When it goes wrong

- Wrong commit tagged, nothing published:

```sh
git push --delete origin vX.Y.Z
git tag -d vX.Y.Z
```

- Failed before publish: fix -> delete + re-push tag
- Already on PyPI: no replace, no reuse — bump patch, release again
- Bad notes: edit on release page, or `gh release edit vX.Y.Z --notes-file -`
