Release checklist. Not part of the docs site. X.Y.Z is the new version.

One-time setup:

- Add a PyPI trusted publisher at https://pypi.org/manage/account/publishing/
  Project pynumstore, owner NumstoreDB, repo Numstore, workflow release.yml,
  environment pypi.
- Do the same at https://test.pypi.org/manage/account/publishing/ with
  environment testpypi. The dry run in moc_release.md uses it.
- Create both environments: Settings > Environments > New environment.
  Name them pypi and testpypi. Add yourself as a required reviewer on pypi
  if you want to approve before anything goes public.
- Settings > Pages > Source > GitHub Actions.
- Install and log in to the GitHub CLI: brew install gh && gh auth login.
- There is no token to store. PyPI trusts the workflow, not a secret.

Release:

- Start clean and green. Check the Actions tab: last CI and Wheels run passed.

```sh
git checkout develop
git pull
git status
```

- Bump the version. version.txt is the only place a version number lives.
  The wheel, the CMake package and ns.__version__ all read it.

```sh
echo X.Y.Z > version.txt
```

- Breaking API change: new major. New features: new minor. Fixes only: new patch.
- Update CHANGELOG.md. Add a section at the top:

```
## X.Y.Z - YYYY-MM-DD
```

- The GitHub release notes are copied from that section, so the heading has to
  match the version exactly.
- Update CITATION.cff: version: X.Y.Z and date-released: "YYYY-MM-DD".
  It and the changelog are the only two files that cannot read version.txt.
- Test it. test-python includes the check that the installed version equals
  version.txt, so this catches a half-finished bump.

```sh
make test-unit
make test-python
make docs
```

- Commit and push, then wait for CI and Wheels to go green on that commit.

```sh
git commit -am "release X.Y.Z"
git push
```

- Do not tag a red commit. The tag is what publishes to PyPI, and a PyPI
  version can never be reused.
- Tag it. The leading v matters, the workflow only triggers on v*.

```sh
git tag vX.Y.Z
git push origin vX.Y.Z
```

- Watch it: gh run watch
- The workflow checks the tag against version.txt, builds every wheel and the
  sdist, publishes to PyPI, then creates the GitHub release with your changelog
  section as the notes and the sdist and wheels attached. GitHub adds the source
  tar.gz and zip itself.
- If pypi is a protected environment, the publish step waits for you to approve.
- Verify:

```sh
pip install --no-cache-dir --upgrade pynumstore
python -c "import pynumstore as ns; print(ns.__version__)"
gh release view vX.Y.Z
```

- Open https://numstoredb.github.io/Numstore/ - the push in step 5 rebuilt it.

When it goes wrong:

- Tagged the wrong commit, nothing published yet: delete and redo the tag.

```sh
git push --delete origin vX.Y.Z
git tag -d vX.Y.Z
```

- Workflow failed before the publish step: fix it, then delete and re-push
  the tag.
- Already published to PyPI and found a bug: you cannot replace or reuse a
  version. Bump the patch and release again.
- Release notes read badly: edit them on the release page, or
  gh release edit vX.Y.Z --notes-file -
- Changing this process: rehearse it first, see moc_release.md.
