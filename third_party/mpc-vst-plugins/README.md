# Vendored: sd88me/mpc-vst-plugins (MIT)

Copied from https://github.com/sd88me/mpc-vst-plugins at commit
`670b20b9ab655140f324f9d093aa5c6f35929f73` (2026-10-04). License: see `LICENSE`
(MIT, Copyright (c) 2026 sd88me); `tools/vendor/force-shadow/` carries its own MIT LICENSE.
Unmodified apart from the marked local patches listed below.

**Release and installer files are newer:** `tools/release.py`, `tools/release/*` (`install.sh`, `uninstall.sh`,
`sync.sh`, `plugin_list.awk`), `tools/catalog_check.py`, `tools/deps.py`, `tools/skin_compat.py`,
`tools/skin_roles_2x.json`, `tools/user_files.py` and `tools/install_smoke.py` are copied unmodified from
https://github.com/open-mpc/mpc-vst-plugins (the project's new home) at commit
`1af05b911e8580c8c2e473a1cc10119132e06eca` (2026-11-20): the installer then finds MPC OS 3.10.1+ and installs
into `<drive>/VST2 Plugins` (an existing copy there is updated in place), older systems keep `/sdcard/Synths`.
The skin tools stay at the commit above (they carry the local patches).

What we use it for (we do NOT link their `vst2_wrap.c` / `engine.h` wrapper — our plugin glue is
`plugin/` in this repo):

| File | Used for |
|---|---|
| `tools/gen_vst.py`, `shadow_skin.py`, `skin_assets.py`, `params.py`, `shadow_art.c`, `vendor/force-shadow/` | `params.json` + `layout.conf` -> `params.h`, the touchscreen skin (`TUI.json`, `Q-Links.json`, PNGs) and the `pluginList-arm` entry |
| `tools/studio.py`, `studio_web.py` | preview skin pages as PNG; browser layout editor |
| `tools/release.py`, `tools/release/*`, `catalog_check.py`, `deps.py`, `skin_compat.py`, `user_files.py` | release zip + on-device `install.sh` / `uninstall.sh` (stop MPC, back up and edit `MPC.settings`; `VST2 Plugins` on MPC OS 3.10.1+), the zip check |
| `tools/install_smoke.py` | runs the zip's own install/upgrade/uninstall against a scratch folder (CI, after packaging) |
| `wrapper/popup.h`, `wrapper/plugin_dir.h` | popup-picker param helpers; find the plugin's own folder at runtime |

Do not edit files here beyond the marked patches; patch around them so an upgrade stays a plain
re-copy.

## Local patches (PolyForce)

`tools/shadow_skin.py` carries five small, marked (`PolyForce local patch`) changes:
1. `theme_title=` / `title_size=`: frame title colour and size (were fixed ACCENT_HI / 26 px).
2. Popup list options are drawn with the real title font (`SHADOW_TITLE_FONT`) like enum
   segments, instead of the 9x9 bitmap font.
3. `theme_tile_on=`: a filled lit state for `list` tiles (was a 3 px outline only).
4. `slider_v`/`slider_h` accept `cw=` (component width), so a tight row of sliders doesn't
   overlap hit areas and value labels.
5. A `meter` without `strip=` builds with the C renderer: its strip is drawn as a vertical
   slider's (`sh_meter_<w>x<h>.png`) for a post-step to redraw (PolyForce's wave view:
   `surface/skin_polish.py`). With the browser renderer a meter still needs `strip=`.
Re-apply them after re-copying upstream.
