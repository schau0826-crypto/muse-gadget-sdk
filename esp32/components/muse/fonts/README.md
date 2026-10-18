# CoreS3 Chinese caption font

`muse_font_cjk_16.c` is an offline, 16 px, 2 bpp bitmap derivative of
**Noto Sans CJK SC Regular v2.004**, generated with `lv_font_conv` **1.5.3**.
It is licensed under **SIL Open Font License 1.1** (see [OFL.txt](OFL.txt)).
Copyright 2014-2021 Adobe (http://www.adobe.com/).

The asset contains 21,105 glyphs: available Unified CJK ideographs in
U+4E00–U+9FFF, CJK punctuation U+3000–U+301F and U+3030, and fullwidth forms
U+FF01–U+FF60. Simplified and Traditional characters use SC regional shapes.
Extension A/B ideographs and emoji are outside this asset's coverage.
ASCII falls back to the existing `lv_font_unscii_16`, so both ASCII and CJK
characters advance 16 px and the caption's cell-based wrapping stays exact.
The resulting line height is 20 px; bitmap compression and kerning are off.

This asset is linked only for CoreS3. Its bitmap exceeds LVGL's compact
descriptor limit, so the CoreS3 overlay enables `LV_FONT_FMT_TXT_LARGE`.
Do not copy this asset into a smaller app partition without checking the
linked firmware size.

Pinned upstream source:
<https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf>

Source SHA-256:
`2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`

Upstream license:
<https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/LICENSE>

Regenerate from `esp32/` (the converter can be installed locally with
`npm install --prefix /path/to/font-tools lv_font_conv@1.5.3`):

```sh
LV_FONT_CONV=/path/to/font-tools/node_modules/.bin/lv_font_conv \
  bash tools/muse/generate_cjk_font.sh /path/to/NotoSansCJKsc-Regular.otf
```
