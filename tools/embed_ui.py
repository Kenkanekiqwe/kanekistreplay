import pathlib
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
out = pathlib.Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
values = ",".join(str(byte) for byte in data)
out.write_text(
    "#pragma once\ninline constexpr char kEmbeddedUiHtml[] = {"
    + values
    + ",0};\n",
    encoding="ascii",
)
