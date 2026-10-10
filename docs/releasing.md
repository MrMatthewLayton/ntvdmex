# Releasing

Two long-lived branches:

- **`main`** is the working trunk and the default branch. Every change lands here by pull
  request with green CI. It builds and passes its tests, but it has not necessarily been run on
  Windows XP: main might not work.
- **`release`** only ever holds builds that work. Nothing is committed to it directly: it is
  main, promoted by pull request once main's build has been run on a real Windows XP machine.

A release is published automatically when `release` moves to a version that has not been
released yet.

```
main  --(pull request, merge commit)-->  release  --(CI green)-->  tag v<VERSION> + GitHub release
```

## Versions

`VERSION` at the top of the repository names the next release, in semantic versioning:
`0.1.0-alpha`, `0.1.0-alpha.2`, ..., `0.1.0-beta`, `0.1.0`. A version with a suffix is published
as a GitHub *pre-release*. The tag is `v<VERSION>`; the zip is `ntvdmex-v<VERSION>.zip`, and its
`VERSION.txt` also carries the build ID (`ntvdmex-<date>-<sha>`) and the commit.

## Making a release

1. **On main, by pull request:** set `VERSION`, and write the notes in
   `docs/releases/v<VERSION>.md` -- what it is, what was confirmed working and on what, install and
   uninstall, before-you-install warnings, known issues (the open bugs a user will meet, with
   their issue numbers), and how to report a problem.
2. **Run main's build on a real Windows XP SP3 machine** -- the programs the notes say were
   confirmed. This is the check CI cannot make, and it is what makes `release` trustworthy.
3. **Promote:** open a pull request from `main` into `release` and merge it with a **merge
   commit** (not rebase or squash), so `release` stays a copy of main's history. CI must be
   green, and the *Promoted from main* check refuses a pull request from any other branch.
4. **Automatic from here:** when CI passes on `release`, `.github/workflows/release.yml` builds
   the zip from that commit with `SHA256SUMS`, creates the tag `v<VERSION>` and publishes the
   GitHub release with the notes.

Promoting main without changing `VERSION` publishes nothing -- `release` simply moves to a newer
known-good build. A fix is never made on `release`: it lands on main and is promoted.

A published release is never replaced. A mistake found later is fixed by the next release. If
the release workflow fails (the notes missing, a build error), fix it on main and promote again;
nothing is tagged until it succeeds.
