# Releasing

Publishing a GitHub release uploads openc166 to
[PyPI](https://pypi.org/project/openc166/). The `Wheels` workflow
(`.github/workflows/wheels.yml`) builds the source package and the wheels,
tests them, and uploads them. It uses PyPI's *trusted publishing*, so no
password or API token is stored anywhere.

## One-time setup

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

3. On GitHub, open *Settings → Environments*, create an environment named
   `pypi`, and add yourself under *Required reviewers*. Each upload then
   waits until you approve it.

## Each release

1. Set the new version in `CMakeLists.txt`
   (`project(openc166 VERSION x.y.z ...)`), commit, push, and wait for the
   CI and Wheels workflows to pass.
2. On GitHub, open *Releases → Draft a new release*. Create the tag
   `vx.y.z` on `main`, click *Generate release notes*, and publish.
3. Approve the `pypi` deployment in the workflow run. A few minutes later
   `pip install openc166==x.y.z` works.

The workflow stops before uploading anything if the tag does not match the
version in `CMakeLists.txt`. PyPI never accepts the same version twice: to
fix a broken release, raise the version and release again.
