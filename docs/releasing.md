# Releasing

A release is a tag on `main`, a zip that CI builds from it, and notes written by hand. Nothing
is published until the maintainer has run that exact zip on a real Windows XP machine.

## Versions

Semantic versions, `v<major>.<minor>.<patch>`, with a pre-release suffix while it is early:
`v0.1.0-alpha`, `v0.1.0-alpha.2`, ..., `v0.1.0-beta`, `v0.1.0`. A tag with a suffix becomes a
GitHub *pre-release*. The zip is named for the version (`ntvdmex-v0.1.0-alpha.zip`); its
`VERSION.txt` also carries the build ID (`ntvdmex-<date>-<sha>`) and the commit.

## Steps

1. **The notes.** Write `docs/releases/<tag>.md` and merge it through a pull request: what it
   is, what was confirmed working and on what, install and uninstall, before-you-install
   warnings, known issues (the open bugs a user will meet, with their issue numbers), and how
   to report a problem. The release workflow refuses a tag without it.
2. **The tag.** On an up-to-date `main` whose CI is green:

   ```sh
   git tag -a v0.1.0-alpha -m "NTVDMEX v0.1.0-alpha"
   git push origin v0.1.0-alpha
   ```

3. **The draft.** `.github/workflows/release.yml` checks that the tag is on `main`, builds,
   checks the imports, runs the battery, packages the zip with `SHA256SUMS`, and creates a
   **draft** release with the notes.
4. **The check.** Download the draft's zip, verify it against `SHA256SUMS`, install it on a real
   Windows XP SP3 machine, and run the programs the notes say were confirmed. If anything is
   wrong, delete the draft and the tag (`git push origin :refs/tags/<tag>`), fix it, and start
   again with the same or the next version.
5. **Publish** the draft on GitHub.

A published release is never replaced. A mistake found later is fixed by the next release.
