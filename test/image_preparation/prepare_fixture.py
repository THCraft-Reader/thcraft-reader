"""Extract the checked-in JPEG and a complete raw-DEFLATE stream for host tests."""

import pathlib
import sys
import zipfile
import zlib


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: prepare_fixture.py INPUT_EPUB OUTPUT_DIR")
    with zipfile.ZipFile(sys.argv[1]) as epub:
        jpeg = epub.read("OEBPS/images/grayscale_test.jpg")
    compressor = zlib.compressobj(wbits=-15)
    compressed = compressor.compress(jpeg) + compressor.flush()
    output = pathlib.Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    (output / "grayscale.jpg").write_bytes(jpeg)
    (output / "grayscale.rawdeflate").write_bytes(compressed)


if __name__ == "__main__":
    main()
