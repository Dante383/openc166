# Releasing

Every push to `main` is released automatically to
[PyPI](https://pypi.org/project/openc166/) and as a GitHub release. The
`Build and release` workflow (`.github/workflows/wheels.yml`):

1. works out the version (see below),
2. builds and tests the source package and the wheels for every platform,
3. runs the CI workflow (`.github/workflows/ci.yml`),
4. if all of that passes, uploads the packages to PyPI, and
5. creates the GitHub release `vx.y.z` with the packages attached and the
   commit messages since the previous release as notes.

If anything fails, nothing is uploaded. Pull requests run the same builds
and tests but never release anything.

It uses PyPI's *trusted publishing*, so no password or API token is stored
anywhere.

## Version numbers

The version in `CMakeLists.txt` (`project(openc166 VERSION x.y.z ...)`) is
the starting point. Each commit after the one that last changed that line
adds one to the last number. With `VERSION 0.1.0` there, the 15th commit
after it is released as `0.1.15`.

To start a new series, change the line, for example to `VERSION 0.2.0`. The
commit that changes it is released as exactly `0.2.0`, the next one as
`0.2.1`, and so on. Only ever raise it: the workflow refuses to release a
version that was already released from a different commit.

The version in the repository is not edited by the workflow; it only patches
its own copy before building. A local build from a checkout reports the
starting version (`0.1.0` above).

## Skipping a release

Put `[skip ci]` in the commit message (of the last commit, when pushing
several) and GitHub runs no workflows for that push: no tests, no release.
The commits still count towards the next version number.

## Fixing a broken release

PyPI never accepts the same version twice, and a release cannot be changed
after it is uploaded. Fix the problem and push again: the next commit gets
the next version number.

If a run fails after the upload (for example while creating the GitHub
release), use *Re-run failed jobs*. PyPI skips files it already has.

## One-time setup

This is already done for `Dante383/openc166`; it is only needed for a fork
or a new PyPI project.

1. Create an account on <https://pypi.org> and turn on two-factor
   authentication.
2. Open <https://pypi.org/manage/account/publishing/> and add a pending
   publisher on the GitHub tab:

   | Field | Value |
   |---|---|
   | PyPI Project Name | `openc166` |
   | Owner | `Dante383` |
   | Repository name | `openc166` |
   | Workflow name | `wheels.yml` |
   | Environment name | `pypi` |

3. On GitHub, open *Settings → Environments* and create an environment
   named `pypi`. Under *Deployment branches and tags*, choose *Selected
   branches and tags* and add `main`, so that only `main` can upload. Do not
   add *Required reviewers*: each upload would then wait for someone to
   approve it.
