# Privacy audit of the Steam Frame fork

Date: 2026-10-11. Branch: `steam-frame`. This file records what was checked for personal data,
what was changed, and the commands that repeat the check.

## What was searched

All tracked files on `steam-frame`: the READMEs, `DISTRIBUTION.md`, `RELEASE_VALIDATION.md`,
`OPENXR.md`, `CONTRIBUTING.md`, `THIRD-PARTY-NOTICES.md`, `docs/`, `.github/` (CI and issue
templates), `steam-frame/` (scripts, binding, seed config, generated build files), test fixtures
and binary assets (with `strings`). Also the commit messages, author and committer identities
and tag identities of the fork's commits, and the history for secrets.

Categories:

- Names and old account names of the owner, and school or employer names.
- Host names of the owner's computers and client names.
- LAN, hotspot, VPN and public IP addresses. MAC addresses.
- Wi-Fi network names, including the Frame's own access point name.
- UUIDs and IDs tied to the owner's computers, Steam user IDs, device serial numbers, system IDs,
  headset and Android device IDs in probe dumps, logcat excerpts and validation logs.
- GitHub and API tokens, API keys, keystore passwords, paths to a signing key,
  PEM and certificate material, email addresses.

The owner's specific strings are kept in a private list outside the repository. They are not
written here.

## Results

- Tracked files: no personal data found. Email addresses appear only in third-party copyright
  headers and upstream contributors' co-author lines. `/home/steamos` paths are SteamOS's default
  user and stay.
- Commit and tag identities of the fork use the GitHub noreply address.
- History of `steam-frame`: no tokens, private keys, certificates or keystore references.
- Release APK `v1.3.1-frame.3`: the v2 signing certificate has a generic subject and no personal
  fields. Unpacked, the APK contains no personal strings. Two kit objects contain the generic build
  path under `/home/steamos`. `SHA256SUMS.txt` holds only the hash and file name.

## Changes

- `steam-frame/build.sh`: a comment that referred to another project of the owner was made
  generic.
- `steam-frame/collect-logs.sh` (new): gathers the logs for a bug report and masks IP and MAC
  addresses, UUIDs, host names, Wi-Fi names, serial numbers, Steam IDs, email addresses, tokens,
  certificate blocks and user names in `/home` paths. Lepton's log contains the Frame's kernel log,
  which lists hardware serial numbers, so this matters for anyone who posts logs.
- `README.md`: rewritten. It tells people to run `collect-logs.sh` and read the result before
  posting.

## Verification

Run from the repository root. Each command must print `0`.

```sh
# The owner's private list: one string per line, kept outside the repository.
git grep -IinF -f ~/.config/wcf-private-terms.txt | wc -l
git log --all --format='%an %ae %cn %ce %B' | grep -icF -f ~/.config/wcf-private-terms.txt

# Generic patterns in the fork's own files.
FORK="README.md CONTRIBUTING.md .github steam-frame :!steam-frame/build-deps/Dependencies :!steam-frame/build-deps/generated/sdl-build"
git grep -IhE '\b(10\.[0-9]+|192\.168|172\.(1[6-9]|2[0-9]|3[01])|100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7]))\.[0-9]+\.[0-9]+\b' -- $FORK | wc -l
git grep -IhE '\b([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b' -- $FORK | wc -l
git grep -IhE '\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b' -- $FORK | wc -l
git grep -IhE '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z.]{2,}' -- $FORK | wc -l
git grep -IhoE '/home/[A-Za-z0-9_.-]+' -- $FORK | sort -u | grep -vxE '/home/steamos' | wc -l

# Secrets anywhere in the tree and in the branch history.
git grep -IhE '\b(ghp|gho|ghu|ghs|github_pat)_[A-Za-z0-9_]{20,}|\bsk-[A-Za-z0-9]{20,}|BEGIN [A-Z ]*(PRIVATE KEY|CERTIFICATE)-----|(store|key)Password' | wc -l
git log steam-frame -p -G '(ghp|gho|ghu|ghs|github_pat)_[A-Za-z0-9_]{20,}' -- | wc -l
git log steam-frame -p -G 'BEGIN [A-Z ]*(PRIVATE KEY|CERTIFICATE)-----' -- | wc -l
git log steam-frame -p -G '[.](jks|keystore)\b|(store|key)Password' -- | wc -l
```

The fork's commit identities can be listed with
`git log --format='%ae%n%ce' origin/openxr-work..steam-frame | sort -u`. It must print only the
GitHub noreply address.

To check a release APK's signing certificate without Java, extract the certificate from the APK
Signing Block and run `openssl x509 -inform DER -noout -subject -issuer` on it.
