#!/usr/bin/env bash
# Generate the CoreS3 offline font from Noto Sans CJK SC Regular v2.004.
# The generated bitmap is distributed under fonts/OFL.txt, not Apache 2.0.
set -euo pipefail

if [[ $# != 1 ]]; then
    echo "Usage: LV_FONT_CONV=/path/to/lv_font_conv $0 NotoSansCJKsc-Regular.otf" >&2
    exit 2
fi

project_dir="$(cd "$(dirname "$0")/../.." && pwd)"
font_source="$1"
converter="${LV_FONT_CONV:-lv_font_conv}"
font_output="$project_dir/components/muse/fonts/muse_font_cjk_16.c"
expected_sha="2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
if [[ "$("$converter" --version)" != "1.5.3" ]]; then
    echo "Use lv_font_conv 1.5.3 to reproduce the committed bitmap" >&2
    exit 2
fi

python3 - "$font_source" "$expected_sha" <<'PY'
import hashlib, sys
from pathlib import Path
if hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() != sys.argv[2]:
    raise SystemExit("Expected the pinned Noto Sans CJK SC Regular v2.004 source font")
PY

"$converter" --font "$font_source" \
    -r 0x3000-0x301F,0x3030,0x4E00-0x9FFF,0xFF01-0xFF60 \
    --size 16 --bpp 2 --no-compress --no-kerning --format lvgl \
    --lv-font-name muse_font_cjk_16 --lv-include lvgl.h \
    --lv-fallback lv_font_unscii_16 -o "$font_output"

python3 - "$font_output" "$font_source" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text().replace(sys.argv[2], "NotoSansCJKsc-Regular.otf", 1)
s = s.replace(str(p), "components/muse/fonts/muse_font_cjk_16.c", 1)
p.write_text("/* Bitmap derivative of Noto Sans CJK SC Regular v2.004.\n"
             " * Copyright 2014-2021 Adobe (http://www.adobe.com/).\n"
             " * Licensed under SIL Open Font License 1.1; see OFL.txt.\n"
             " * Reproduce with tools/muse/generate_cjk_font.sh and lv_font_conv 1.5.3.\n"
             " */\n" + s)
PY
