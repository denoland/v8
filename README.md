# denoland/v8

This repository exists to float patches ontop the of
[upstream v8 repo][upstream] for use in [rusty_v8][rusty_v8]. The patches that
are floated are not functional changes to V8 code. Instead all patches are only
here to help accomodate build system differences between Chromium and rusty_v8.

[upstream]: https://chromium.googlesource.com/v8/v8.git
[rusty_v8]: https://github.com/denoland/rusty_v8

## How this repo works

This repository contains an autoroll script that runs every night. This script
maintains branches in the format `$V8_VERSION-lkgr-denoland` for the last few V8
releases. These branches are a mirror of the upstream `$V8_VERSION-lkgr`
branches, except with the patches from the `patches/` directory applied.

Currently the following V8 branches are actively updated:

- `9.7-lkgr-denoland`
- `9.8-lkgr-denoland`
- `9.9-lkgr-denoland`

Old V8 branches are also still available:

- `9.2-lkgr-denoland`
- `9.3-lkgr-denoland`
- `9.4-lkgr-denoland`
- `9.5-lkgr-denoland`
- `9.6-lkgr-denoland`

## Contributing

For submitting patches to this repo, please contact the maintainers
[on Discord][discord] in the `#dev-rusty_v8` channel.

[discord]: https://discord.gg/deno
